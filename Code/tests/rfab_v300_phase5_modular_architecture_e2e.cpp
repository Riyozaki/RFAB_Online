#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <CoopAnimationState.h>
#include <CoopCheckpointStore.h>
#include <CoopDamagePipeline.h>
#include <CoopExperienceLedger.h>
#include <CoopOwnership.h>
#include <CoopPacketSafety.h>
#include <CoopWorldView.h>
#include <RfabModpackCompat.h>
#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/GridCellCoords.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
std::filesystem::path FindPhase5RepoRoot()
{
    auto dir = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (std::filesystem::exists(dir / "Code" / "common" / "CoopExperienceLedger.h"))
            return dir;
        if (!dir.has_parent_path())
            break;
        dir = dir.parent_path();
    }
    return std::filesystem::current_path();
}

std::string LoadPhase5File(const std::filesystem::path& aRoot, const char* apRelPath)
{
    std::ifstream in(aRoot / apRelPath, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t CountLines(const std::string& aText) noexcept
{
    if (aText.empty())
        return 0;
    size_t lines = 0;
    for (char ch : aText)
    {
        if (ch == '\n')
            ++lines;
    }
    return aText.back() == '\n' ? lines : (lines + 1);
}
} // namespace

TEST_CASE("v3.0 Phase 5: CoopExperienceLedger normalizes multipliers and deduplicates quest XP across both race orders", "[rfab][v300][phase5][xp]")
{
    SECTION("Sender normalization and receiver personal multiplier scaling preserve Requiem 5-XP granularity")
    {
        // Sender with 1.5x Adventurer's Ring earns 150 XP locally -> broadcasts exact 100 base XP
        CHECK(CoopExperienceLedger::NormalizeSenderBaseXp(150, 1.5f) == 100);
        // Sender with 0.8x penalty earns 80 XP locally -> broadcasts exact 100 base XP
        CHECK(CoopExperienceLedger::NormalizeSenderBaseXp(80, 0.8f) == 100);
        // Receiver with 1.25x Standing Stone receives 100 base XP -> 125 XP
        CHECK(CoopExperienceLedger::ScaleReceiverPersonalXp(100u, 1.25f) == 125);
        // Small positive grant never rounds down to 0
        CHECK(CoopExperienceLedger::ScaleReceiverPersonalXp(3u, 0.5f) == 5);
        // Out-of-bounds packet is rejected
        CHECK(CoopExperienceLedger::ScaleReceiverPersonalXp(CoopExperienceLedger::kMaxRfabXpDelta + 1u, 1.0f) == 0);
    }

    SECTION("Race Order A: synced quest stage executes locally BEFORE remote SyncRfabExperience arrives")
    {
        CoopExperienceLedger::ClientRfabXpTracker tracker;
        int32_t wallet = 1000;

        // Baseline initialization poll
        auto initPoll = tracker.EvaluateLocalWalletPoll(wallet, true, false, 1.0f, 1000ULL);
        CHECK(initPoll.RefundToApply == 0);
        CHECK(initPoll.BroadcastBaseXp == 0);

        // Remote quest stage packet arrives at t=2000ms and local Papyrus quest fragment awards +500 XP at t=2100ms
        tracker.OnRemoteQuestStageApplied(2000ULL);
        wallet += 500;
        auto stagePoll = tracker.EvaluateLocalWalletPoll(wallet, true, false, 1.0f, 2100ULL);
        CHECK(stagePoll.RefundToApply == 0);
        CHECK(stagePoll.BroadcastBaseXp == 0); // Must NOT echo quest XP back to party!
        CHECK(tracker.GetLocalQuestXpGainedInWindow() == 500);

        // Host's NotifySyncRfabExperience(+500) arrives at t=2400ms -> completely absorbed!
        const int32_t incomingToApply = tracker.EvaluateIncomingRemoteXp(500u, 1.0f, 2400ULL);
        CHECK(incomingToApply == 0);
        CHECK(wallet == 1500); // Exact 1x quest XP!
    }

    SECTION("Race Order B: remote SyncRfabExperience arrives BEFORE synced quest stage executes locally")
    {
        CoopExperienceLedger::ClientRfabXpTracker tracker;
        int32_t wallet = 1000;
        (void)tracker.EvaluateLocalWalletPoll(wallet, true, false, 1.0f, 1000ULL);

        // Host's NotifySyncRfabExperience(+500) arrives FIRST at t=2000ms
        const int32_t delta = tracker.EvaluateIncomingRemoteXp(500u, 1.0f, 2000ULL);
        REQUIRE(delta == 500);
        wallet += delta;
        tracker.CommitAppliedRemoteXp(1000, delta, 2000ULL);
        REQUIRE(wallet == 1500);

        // Synced quest stage arrives 400ms later (within 3000ms lookback) and awards +500 XP locally
        tracker.OnRemoteQuestStageApplied(2400ULL);
        CHECK(tracker.GetCreditToRefundOnLocalQuestXp() == 500);

        wallet += 500; // Local quest fragment momentarily pushes wallet to 2000
        const auto poll = tracker.EvaluateLocalWalletPoll(wallet, true, false, 1.0f, 2500ULL);
        CHECK(poll.BroadcastBaseXp == 0);
        REQUIRE(poll.RefundToApply == 500);

        wallet -= poll.RefundToApply;
        tracker.CommitLocalRefundApplied(poll.RefundToApply, wallet);
        CHECK(wallet == 1500); // Exact 1x quest XP across both race orders!
        CHECK(tracker.GetCreditToRefundOnLocalQuestXp() == 0);
    }

    SECTION("CustomMenu perk spend and undo/cancel never broadcast false XP deltas")
    {
        CoopExperienceLedger::ClientRfabXpTracker tracker;
        int32_t wallet = 2000;
        (void)tracker.EvaluateLocalWalletPoll(wallet, true, false, 1.0f, 1000ULL);

        // Player opens RFAB_Menu ("CustomMenu"), spends 800 XP, then cancels (+800 XP back to 2000)
        wallet = 1200;
        auto spendPoll = tracker.EvaluateLocalWalletPoll(wallet, true, true, 1.0f, 1250ULL);
        CHECK(spendPoll.BroadcastBaseXp == 0);

        wallet = 2000;
        auto undoPoll = tracker.EvaluateLocalWalletPoll(wallet, true, true, 1.0f, 1500ULL);
        CHECK(undoPoll.BroadcastBaseXp == 0);

        // Menu closes; normal +250 kill XP afterwards broadcasts cleanly
        wallet = 2250;
        auto killPoll = tracker.EvaluateLocalWalletPoll(wallet, true, false, 1.0f, 7000ULL);
        CHECK(killPoll.BroadcastBaseXp == 250u);
    }
}

TEST_CASE("v3.0 Phase 5: End-to-end co-op session scenario across Phases 1-5 invariants", "[rfab][v300][phase5][e2e]")
{
    // 1. Phase 1: Reentrant ScratchAllocator safety under nested send simulation
    struct LinearScratchStub
    {
        std::array<uint8_t, 4096> Backing{};
        size_t Offset{0};
        uint32_t ResetCount{0};

        uint8_t* Allocate(size_t aBytes) noexcept
        {
            if (Offset + aBytes > Backing.size())
                return nullptr;
            uint8_t* p = Backing.data() + Offset;
            Offset += aBytes;
            return p;
        }

        void Reset() noexcept
        {
            Offset = 0;
            std::fill(Backing.begin(), Backing.end(), uint8_t{0});
            ++ResetCount;
        }
    };

    LinearScratchStub scratch{};
    uint32_t scratchDepth = 0;
    {
        CoopPacketSafety::ScopedScratchFrame outerFrame(scratch, scratchDepth);
        uint8_t* pOuterPacket = scratch.Allocate(128);
        REQUIRE(pOuterPacket != nullptr);
        pOuterPacket[0] = 0xA5;
        pOuterPacket[127] = 0x5A;

        {
            CoopPacketSafety::ScopedScratchFrame innerFrame(scratch, scratchDepth);
            uint8_t* pInnerPacket = scratch.Allocate(256);
            REQUIRE(pInnerPacket != nullptr);
            REQUIRE(pInnerPacket >= pOuterPacket + 128);
            std::fill(pInnerPacket, pInnerPacket + 256, uint8_t{0xFF});
        }

        // Outer packet payload must remain 100% intact after nested Send() exits!
        CHECK(scratch.ResetCount == 0u);
        CHECK(pOuterPacket[0] == 0xA5);
        CHECK(pOuterPacket[127] == 0x5A);
    }
    CHECK(scratch.ResetCount == 1u);
    CHECK(scratchDepth == 0u);

    // 2. Phase 2: Auto-Party + Sticky Host Cell Authority + Independent Guest Interior Visit
    CoopOwnership::ClientEpochTracker hostEpochTracker;
    CoopOwnership::ClientEpochTracker guestEpochTracker;
    CoopWorldView::ServerInterestRegistry interestRegistry;

    constexpr uint32_t kHostPlayerId = 1u;
    constexpr uint32_t kGuestPlayerId = 2u;
    constexpr uint32_t kBossServerId = 500u;

    // Guest loaded cell 200ms earlier, then Host (Party Leader) arrives -> Sticky Host Cell Authority grants immediately!
    const auto stickyTakeover = CoopOwnership::EvaluateVoluntaryClaim(
        kGuestPlayerId, kHostPlayerId, /*lastChangedAtMs=*/1000ULL, /*nowMs=*/1200ULL,
        /*isPlayerAvatar=*/false, /*isPlayerSummon=*/false,
        /*currentOwnerValidAndInRange=*/true, /*currentOwnerDowned=*/false, /*challengerDowned=*/false,
        CoopOwnership::PolicyConfig{}, /*challengerIsCalmPartyLeader=*/true);
    CHECK(stickyTakeover == CoopOwnership::ClaimDecision::kAccept);

    const uint64_t hostGrantEpoch = 2ULL;
    CHECK(hostEpochTracker.AcceptGrant(kBossServerId, hostGrantEpoch));
    CHECK(guestEpochTracker.AcceptRelinquish(kBossServerId, hostGrantEpoch));
    interestRegistry.MarkInRange(kHostPlayerId, kBossServerId);
    interestRegistry.MarkInRange(kGuestPlayerId, kBossServerId);

    // Guest steps into a neighboring Whiterun shop interior while Host stays in boss cell:
    // Guest unmarks boss interest locally without revoking Host's ownership epoch!
    interestRegistry.UnmarkInRange(kGuestPlayerId, kBossServerId);
    CHECK(interestRegistry.IsMarkedInRange(kHostPlayerId, kBossServerId));
    CHECK_FALSE(interestRegistry.IsMarkedInRange(kGuestPlayerId, kBossServerId));
    CHECK(hostEpochTracker.GetEpoch(kBossServerId) == hostGrantEpoch);

    // Guest returns to boss arena
    interestRegistry.MarkInRange(kGuestPlayerId, kBossServerId);
    CHECK(interestRegistry.IsMarkedInRange(kGuestPlayerId, kBossServerId));

    // 3. Phase 4: Native RFAB MaxHP preservation + 1/M incoming damage scaling + circular/discrete BehaviorVars
    constexpr float kNativeBossMaxHp = 1500.0f;
    constexpr float kCoopEnemyMult = 3.0f;
    float serverBossHp = kNativeBossMaxHp;

    // Host lands a -300 post-armor hit, Guest lands a -600 spell hit -> scaled by 1/3 (-100 and -200)
    const float hostScaledDelta = CoopDamagePipeline::ScaleIncomingEnemyDamageDelta(-300.0f, kCoopEnemyMult, true);
    const float guestScaledDelta = CoopDamagePipeline::ScaleIncomingEnemyDamageDelta(-600.0f, kCoopEnemyMult, true);
    CHECK(hostScaledDelta == Approx(-100.0f));
    CHECK(guestScaledDelta == Approx(-200.0f));

    serverBossHp = CoopDamagePipeline::ApplyAuthoritativeServerHealthDelta(serverBossHp, kNativeBossMaxHp, hostScaledDelta).UpdatedHealth;
    serverBossHp = CoopDamagePipeline::ApplyAuthoritativeServerHealthDelta(serverBossHp, kNativeBossMaxHp, guestScaledDelta).UpdatedHealth;
    CHECK(serverBossHp == Approx(1200.0f));

    // Boss triggers a +150 phase regeneration script -> positive heals are NEVER divided by M!
    const float bossHealDelta = CoopDamagePipeline::ScaleIncomingEnemyDamageDelta(+150.0f, kCoopEnemyMult, true);
    CHECK(bossHealDelta == Approx(+150.0f));
    serverBossHp = CoopDamagePipeline::ApplyAuthoritativeServerHealthDelta(serverBossHp, kNativeBossMaxHp, bossHealDelta).UpdatedHealth;
    CHECK(serverBossHp == Approx(1350.0f));

    // Exterior pursuit across 4096 cell border uses smooth ForcePosition inside 3x3 core and SeamlessCellRehome on outer ring
    const auto innerDecision = CoopDamagePipeline::EvaluateExteriorCellMotion(
        /*stepExceedsTeleportSnap=*/false, /*hasValidAttachedParentCell=*/true,
        /*hasLoadedDestCell=*/true, /*parentEqualsDestCell=*/false,
        /*parentGridX=*/9, /*parentGridY=*/10, /*centerGridX=*/10, /*centerGridY=*/10,
        /*posX=*/41000.0f, /*posY=*/41500.0f, /*nowTick=*/5000ULL, /*lastSeamlessRehomeTick=*/0ULL);
    CHECK(innerDecision == CoopDamagePipeline::ExteriorCellMotionMode::kSmoothForcePosition);

    const auto outerDecision = CoopDamagePipeline::EvaluateExteriorCellMotion(
        /*stepExceedsTeleportSnap=*/false, /*hasValidAttachedParentCell=*/true,
        /*hasLoadedDestCell=*/true, /*parentEqualsDestCell=*/false,
        /*parentGridX=*/8, /*parentGridY=*/10, /*centerGridX=*/10, /*centerGridY=*/10,
        /*posX=*/41500.0f, /*posY=*/41500.0f, /*nowTick=*/5000ULL, /*lastSeamlessRehomeTick=*/0ULL);
    CHECK(outerDecision == CoopDamagePipeline::ExteriorCellMotionMode::kSeamlessCellRehome);

    // Circular yaw 355 -> 5 goes through 0 (not 180) and MCO/TUDM discrete vars stay un-smeared
    CHECK(CoopAnimationState::LerpCircularDegrees360(355.0f, 5.0f, 0.5f) == Approx(0.0f).margin(1e-4f));
    CHECK(CoopAnimationState::ClassifyBehaviorFloatBlendKind("weaponSpeedMult") ==
          CoopAnimationState::BehaviorFloatBlendKind::kDiscreteStep);
    CHECK(CoopAnimationState::ClassifyBehaviorFloatBlendKind("SpeedSampled") ==
          CoopAnimationState::BehaviorFloatBlendKind::kContinuousLinear);

    // 4. Phase 2 + Phase 3: Party Wipe -> Coordinated .ess Checkpoint Reload (never in-session partial rollback!)
    CHECK_FALSE(CoopCheckpoint::ShouldServerPreferInSessionRollback(true, true, "RFAB_Coop_Slot1"));
    CHECK_FALSE(CoopCheckpoint::ShouldServerPreferInSessionRollback(false, true, "RFAB_Coop_Quicksave"));
    CHECK(CoopCheckpoint::ShouldServerPreferInSessionRollback(true, true, ""));

    // After coordinated .ess reload, client trackers reset cleanly and accept fresh post-reload epochs
    hostEpochTracker.Clear();
    guestEpochTracker.Clear();
    const uint64_t postReloadEpoch = 10ULL;
    CHECK(hostEpochTracker.AcceptGrant(kBossServerId, postReloadEpoch));
    CHECK_FALSE(hostEpochTracker.AcceptRelinquish(kBossServerId, hostGrantEpoch));

    // 5. Phase 5: Boss & Quest XP synchronized via CoopExperienceLedger with asymmetric multipliers
    CoopExperienceLedger::ClientRfabXpTracker hostXp;
    CoopExperienceLedger::ClientRfabXpTracker guestXp;
    (void)hostXp.EvaluateLocalWalletPoll(5000, true, false, 1.0f, 10000ULL);
    (void)guestXp.EvaluateLocalWalletPoll(3000, true, false, 1.25f, 10000ULL);

    const auto hostBossKillPoll = hostXp.EvaluateLocalWalletPoll(6000, true, false, 1.0f, 11000ULL);
    REQUIRE(hostBossKillPoll.BroadcastBaseXp == 1000u);
    const int32_t guestBossGain = guestXp.EvaluateIncomingRemoteXp(hostBossKillPoll.BroadcastBaseXp, 1.25f, 11050ULL);
    CHECK(guestBossGain == 1250);
}

TEST_CASE("v3.0 Phase 5: CharacterService and RfabAdapterService modular decomposition invariants", "[rfab][v300][phase5][modularity]")
{
    const auto root = FindPhase5RepoRoot();

    // 1. Dedicated Rfab domain modules exist and RfabAdapterService.cpp is a compact coordinator facade
    const std::string rfabAdapterH = LoadPhase5File(root, "Code/client/Services/RfabAdapterService.h");
    const std::string rfabAdapterCpp = LoadPhase5File(root, "Code/client/Services/RfabAdapterService.cpp");
    const std::string rfabResolverCpp = LoadPhase5File(root, "Code/client/Services/Rfab/RfabFormResolver.cpp");
    const std::string rfabXpBookCpp = LoadPhase5File(root, "Code/client/Services/Rfab/RfabXpAndBookSync.cpp");
    const std::string rfabCombatCpp = LoadPhase5File(root, "Code/client/Services/Rfab/RfabCombatRules.cpp");
    const std::string rfabHudCpp = LoadPhase5File(root, "Code/client/Services/Rfab/RfabHudAndPings.cpp");

    CHECK(CountLines(rfabAdapterCpp) < 600);
    CHECK(rfabAdapterH.find("RfabFormResolver m_formResolver") != std::string::npos);
    CHECK(rfabAdapterH.find("RfabXpAndBookSync m_xpAndBookSync") != std::string::npos);
    CHECK(rfabAdapterH.find("RfabCombatRules m_combatRules") != std::string::npos);
    CHECK(rfabAdapterH.find("RfabHudAndPings m_hudAndPings") != std::string::npos);
    CHECK(rfabResolverCpp.find("m_espEdidIndex.IndexRfabPluginsInDataDir") != std::string::npos);
    CHECK(rfabXpBookCpp.find("ApplyDualPoolExperienceDelta") != std::string::npos);
    CHECK(rfabCombatCpp.find("EvaluateSnapshotAction") != std::string::npos);
    CHECK(rfabHudCpp.find("BuildHudSnapshot") != std::string::npos);

    // Render-thread isolation remains strictly enforced on RfabAdapterService::OnDrawTacticalHud
    const size_t drawPos = rfabAdapterCpp.find("void RfabAdapterService::OnDrawTacticalHud() noexcept");
    REQUIRE(drawPos != std::string::npos);
    const std::string drawTail = rfabAdapterCpp.substr(drawPos);
    CHECK(drawTail.find("m_tacticalHudSnapshotMutex") != std::string::npos);
    CHECK(drawTail.find("BSFixedString") == std::string::npos);
    CHECK(drawTail.find("GetMenuOpen") == std::string::npos);
    CHECK(drawTail.find("m_world.view") == std::string::npos);
    CHECK(drawTail.find("TESForm::GetById") == std::string::npos);
    CHECK(drawTail.find("WorldPtToScreenPt3") == std::string::npos);

    // 2. Client & Server CharacterService modular subservices + CoopExperienceLedger + O(1) pending mount lookup
    const std::string cliCharH = LoadPhase5File(root, "Code/client/Services/CharacterService.h");
    const std::string cliCharCpp = LoadPhase5File(root, "Code/client/Services/Generic/CharacterService.cpp");
    const std::string cliIdxCpp = LoadPhase5File(root, "Code/client/Services/Generic/CharacterEntityIndexSubservice.cpp");
    const std::string cliMoveAssignCpp = LoadPhase5File(root, "Code/client/Services/Generic/CharacterMovementAndAssignmentSubservice.cpp");
    const std::string cliMountPkgCpp = LoadPhase5File(root, "Code/client/Services/Generic/CharacterMountAndPackageSubservice.cpp");
    const std::string cliXpDiagCpp = LoadPhase5File(root, "Code/client/Services/Generic/CharacterXpAndDialogueSubservice.cpp");
    const std::string srvCharCpp = LoadPhase5File(root, "Code/server/Services/CharacterService.cpp");
    const std::string srvSocialCpp = LoadPhase5File(root, "Code/server/Services/CharacterSocialAndMountSubservice.cpp");

    CHECK(CountLines(cliCharCpp) < 2900);
    CHECK(CountLines(srvCharCpp) < 1950);
    CHECK(cliCharH.find("CoopExperienceLedger::ClientRfabXpTracker m_rfabXpTracker") != std::string::npos);
    CHECK(cliIdxCpp.find("CharacterService::FindEntityByServerId") != std::string::npos);
    CHECK(cliMoveAssignCpp.find("CharacterService::OnReferencesMoveRequest") != std::string::npos);
    CHECK(cliMountPkgCpp.find("CharacterService::RetryPendingMounts") != std::string::npos);
    CHECK(cliMountPkgCpp.find("mountActorView") == std::string::npos);
    CHECK(cliMountPkgCpp.find("FindEntityByServerId(riderServerId)") != std::string::npos);
    CHECK(cliMountPkgCpp.find("FindEntityByServerId(pending.MountServerId)") != std::string::npos);
    CHECK(cliXpDiagCpp.find("m_rfabXpTracker.EvaluateLocalWalletPoll") != std::string::npos);
    CHECK(cliXpDiagCpp.find("HasVoicePathGenderMismatch") != std::string::npos);
    CHECK(srvSocialCpp.find("CoopExperienceLedger::kMaxRfabXpDelta") != std::string::npos);
}
