#include <TiltedCore/Stl.hpp>

#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <common/CoopWorldStabilizer.h>
#include <common/DroppedItemMatch.h>
#include <common/PausedInboundCoalescer.h>

// Regression suite for the 2026-11-12 review findings (docs/DEFECT_REGISTRY.md section 9a).

namespace
{
std::filesystem::path FindReviewRepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "PausedInboundCoalescer.h"))
            return cur;
        if (!cur.has_parent_path() || cur.parent_path() == cur)
            break;
        cur = cur.parent_path();
    }
    return {};
}

std::string ReadReviewSource(const char* acpRelative)
{
    const auto root = FindReviewRepoRoot();
    if (root.empty())
        return {};
    std::ifstream in(root / acpRelative, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

ActionEvent MakeAction(uint64_t aTick, uint32_t aActionId)
{
    ActionEvent action{};
    action.Tick = aTick;
    action.ActionId = aActionId;
    return action;
}

ServerReferencesMoveRequest MakeMove(uint64_t aTick, uint32_t aServerId, float aDirection, std::initializer_list<uint32_t> aActions = {})
{
    ServerReferencesMoveRequest move{};
    move.Tick = aTick;
    auto& update = move.Updates[aServerId];
    update.UpdatedMovement.Direction = aDirection;
    for (const auto actionId : aActions)
        update.ActionEvents.push_back(MakeAction(aTick, actionId));
    return move;
}
} // namespace

TEST_CASE("N2: movement received while paused is coalesced, not dropped", "[rfab][review][n2]")
{
    using PausedInbound::MergeMoveRequest;

    ServerReferencesMoveRequest pending{};
    bool hasPending = false;

    MergeMoveRequest(pending, hasPending, MakeMove(100, 7, 1.0f, {11}));
    MergeMoveRequest(pending, hasPending, MakeMove(110, 7, 2.0f));
    MergeMoveRequest(pending, hasPending, MakeMove(120, 7, 3.0f, {12, 13}));
    MergeMoveRequest(pending, hasPending, MakeMove(120, 9, 9.0f, {21}));

    REQUIRE(hasPending);
    CHECK(pending.Tick == 120);
    REQUIRE(pending.Updates.size() == 2);

    const auto& seven = pending.Updates[7];
    CHECK(seven.UpdatedMovement.Direction == 3.0f); // newest snapshot wins
    REQUIRE(seven.ActionEvents.size() == 3);        // every reliable action survives, in causal order
    CHECK(seven.ActionEvents[0].ActionId == 11);
    CHECK(seven.ActionEvents[1].ActionId == 12);
    CHECK(seven.ActionEvents[2].ActionId == 13);

    CHECK(pending.Updates[9].ActionEvents.size() == 1);

    // A reordered (older) unreliable snapshot never overwrites a newer position...
    MergeMoveRequest(pending, hasPending, MakeMove(105, 7, -1.0f));
    CHECK(pending.Updates[7].UpdatedMovement.Direction == 3.0f);
    CHECK(pending.Tick == 120);
}

TEST_CASE("N2: coalesced action backlog is bounded per entity and keeps the newest tail", "[rfab][review][n2]")
{
    using PausedInbound::kMaxCoalescedActionsPerEntity;
    using PausedInbound::MergeMoveRequest;

    ServerReferencesMoveRequest pending{};
    bool hasPending = false;

    const uint32_t cTotal = static_cast<uint32_t>(kMaxCoalescedActionsPerEntity) + 40;
    for (uint32_t i = 0; i < cTotal; ++i)
        MergeMoveRequest(pending, hasPending, MakeMove(1000 + i, 5, static_cast<float>(i), {i}));

    const auto& actions = pending.Updates[5].ActionEvents;
    REQUIRE(actions.size() == kMaxCoalescedActionsPerEntity);
    CHECK(actions.front().ActionId == cTotal - kMaxCoalescedActionsPerEntity);
    CHECK(actions.back().ActionId == cTotal - 1);
}

TEST_CASE("N2: reliable paused packets are never dropped; back-pressure instead", "[rfab][review][n2]")
{
    using namespace CoopWorldStabilizer;

    CHECK(ShouldPumpSocketWhilePaused(0));
    CHECK(ShouldPumpSocketWhilePaused(kMaxPausedInboundQueueSize - 1));
    CHECK_FALSE(ShouldPumpSocketWhilePaused(kMaxPausedInboundQueueSize));

    const auto transport = ReadReviewSource("Code/client/Services/Generic/TransportService.cpp");
    if (!transport.empty())
    {
        CHECK(transport.find("m_pausedInboundQueue.pop_front()") == std::string::npos);
        CHECK(transport.find("ShouldPumpSocketWhilePaused") != std::string::npos);
        CHECK(transport.find("MergeMoveRequest") != std::string::npos);
    }
}

TEST_CASE("N2: session packets survive an in-session world reload, world packets do not", "[rfab][review][n2]")
{
    using CoopWorldStabilizer::MustSurviveWorldReload;

    CHECK(MustSurviveWorldReload(kStringCacheUpdate));
    CHECK(MustSurviveWorldReload(kAuthenticationResponse));
    CHECK(MustSurviveWorldReload(kNotifyPlayerJoined));
    CHECK(MustSurviveWorldReload(kNotifyPlayerLeft));
    CHECK(MustSurviveWorldReload(kNotifyPartyInfo));
    CHECK(MustSurviveWorldReload(kNotifySettingsChange));

    CHECK_FALSE(MustSurviveWorldReload(kNotifySpawnData));
    CHECK_FALSE(MustSurviveWorldReload(kCharacterSpawnRequest));
    CHECK_FALSE(MustSurviveWorldReload(kNotifyInventoryChanges));
    CHECK_FALSE(MustSurviveWorldReload(kServerReferencesMoveRequest));
    CHECK_FALSE(MustSurviveWorldReload(static_cast<uint32_t>(kServerOpcodeMax)));

    const auto save = ReadReviewSource("Code/client/Services/CoordinatedSaveService.cpp");
    if (!save.empty())
        CHECK(save.find("DiscardPausedWorldPacketsForReload") != std::string::npos);
}

TEST_CASE("N3: dropped-item pickup only despawns a plausible match", "[rfab][review][n3]")
{
    using namespace DroppedItemMatch;

    const auto sq = [](float v) { return v * v; };

    // Old behaviour deleted the nearest same-base stack up to 2048 units away; that is now rejected.
    CHECK_FALSE(IsEligible({true, sq(2000.0f)}));
    CHECK_FALSE(IsEligible({false, sq(1000.0f)}));
    CHECK_FALSE(IsEligible({false, sq(300.0f)}));
    CHECK(IsEligible({false, sq(100.0f)}));
    CHECK(IsEligible({true, sq(600.0f)}));

    // An exact count match beats a closer stack of a different size.
    const Candidate countMatchFar{true, sq(500.0f)};
    const Candidate otherCountNear{false, sq(50.0f)};
    CHECK(IsBetter(countMatchFar, true, otherCountNear));
    CHECK_FALSE(IsBetter(otherCountNear, true, countMatchFar));

    // Between equal count matches the closer wins.
    CHECK(IsBetter({true, sq(100.0f)}, true, {true, sq(200.0f)}));
    CHECK_FALSE(IsBetter({true, sq(300.0f)}, true, {true, sq(200.0f)}));

    CHECK(StackCountOrOne(false, 0) == 1);
    CHECK(StackCountOrOne(true, 5) == 5);
    CHECK(StackCountOrOne(true, 0) == 1);
}

TEST_CASE("N1: ExtraDataList uses the engine BSReadWriteLock, never an in-house lock", "[rfab][review][n1]")
{
    const auto extra = ReadReviewSource("Code/client/Games/Skyrim/ExtraData/ExtraDataList.cpp");
    const auto extraH = ReadReviewSource("Code/client/Games/Skyrim/ExtraData/ExtraDataList.h");
    const auto atomic = ReadReviewSource("Code/client/Games/Skyrim/BSCore/BSAtomic.cpp");
    const auto primitives = ReadReviewSource("Code/client/Games/Primitives.h");
    if (extra.empty() || extraH.empty() || atomic.empty() || primitives.empty())
        return;

    CHECK(extraH.find("mutable BSReadWriteLock lock") != std::string::npos);
    CHECK(extra.find("lock.LockForRead()") != std::string::npos);
    CHECK(extra.find("lock.UnlockRead()") != std::string::npos);
    CHECK(extra.find("ScopedExtraDataOverride::IsOverriden()") != std::string::npos);
    CHECK(extra.find("BSRecursiveLock") == std::string::npos);
    CHECK(primitives.find("struct BSRecursiveLock") == std::string::npos);

    // SE 1.5.97 Address Library ids (AE 68233/68239/68234/68240 would resolve to unrelated code on SE).
    CHECK(atomic.find("66976") != std::string::npos);
    CHECK(atomic.find("66982") != std::string::npos);
    CHECK(atomic.find("66977") != std::string::npos);
    CHECK(atomic.find("66983") != std::string::npos);
    CHECK(atomic.find("s_lockForRead, 66976)") != std::string::npos);
    CHECK(atomic.find("s_lockForRead, 68233)") == std::string::npos);
}
