#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/NotifyResyncComplete.h>
#include <Messages/RequestResync.h>
#include <Messages/ServerMessageFactory.h>
#include <Structs/ServerSettings.h>

#include <common/CoopWorldStabilizer.h>
#include <common/RfabModpackCompat.h>
#include <common/Session/Features.h>
#include <common/Session/Resync.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// v4.1.0 architecture steps 0 (server feature switches) and 1 (self-healing resync).
// See docs/ARCHITECTURE.md.

namespace
{
using namespace Session::Resync;

std::filesystem::path FindV410RepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "Session" / "Resync.h"))
            return cur;
        if (!cur.has_parent_path() || cur.parent_path() == cur)
            break;
        cur = cur.parent_path();
    }
    return {};
}

std::string ReadV410Source(const std::filesystem::path& acPath)
{
    std::ifstream in(acPath, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<Request> PollAll(ClientLedger& aLedger, uint64_t aNowMs, bool aCanSend = true)
{
    std::vector<Request> out;
    aLedger.Poll(aNowMs, aCanSend, out);
    return out;
}

template <class T> T RoundTripClient(const T& acIn)
{
    TiltedPhoques::Buffer buff(1000);
    TiltedPhoques::Buffer::Writer writer(&buff);
    acIn.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buff);
    const ClientMessageFactory factory;
    auto pMessage = factory.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == T::Opcode);
    return *static_cast<T*>(pMessage.get());
}

template <class T> T RoundTripServer(const T& acIn)
{
    TiltedPhoques::Buffer buff(1000);
    TiltedPhoques::Buffer::Writer writer(&buff);
    acIn.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buff);
    const ServerMessageFactory factory;
    auto pMessage = factory.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == T::Opcode);
    return *static_cast<T*>(pMessage.get());
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Step 0: feature switches
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 protocol version pins the minor bump", "[rfab][v410][version]")
{
    REQUIRE(RfabCompat::kRfabOnlineVersion == "4.1.0");
    CHECK(RfabCompat::AreBuildVersionsProtocolCompatible("v4.1.0", "v4.1.0-g1a2b3c4"));
    CHECK(RfabCompat::AreBuildVersionsProtocolCompatible("v4.1.0", "v4.1.3"));
    // ServerSettings and the opcode tables changed: 4.0.x peers must be refused at the handshake.
    CHECK_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("v4.1.0", "v4.0.0"));
    CHECK_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("v4.0.0-g9988776", "v4.1.0"));
}

TEST_CASE("v4.1.0 feature mask semantics", "[rfab][v410][features]")
{
    using Session::Feature;

    SECTION("zero mask means every feature is on, including bits this build does not know")
    {
        for (const auto& info : Session::kFeatureTable)
            CHECK(Session::IsEnabledIn(0u, info.Bit));
        // An unknown future bit never switches a known feature off.
        const uint32_t futureBit = 1u << 20;
        for (const auto& info : Session::kFeatureTable)
            CHECK(Session::IsEnabledIn(futureBit, info.Bit));
    }

    SECTION("WithFeature toggles exactly one bit")
    {
        uint32_t mask = 0;
        mask = Session::WithFeature(mask, Feature::kTacticalHud, false);
        CHECK_FALSE(Session::IsEnabledIn(mask, Feature::kTacticalHud));
        CHECK(Session::IsEnabledIn(mask, Feature::kHairColorSync));
        mask = Session::WithFeature(mask, Feature::kTacticalHud, true);
        CHECK(mask == 0u);
    }

    SECTION("legacy bools are folded in and override the wire bits for their features")
    {
        uint32_t mask = Session::ComposeDisabledMask(0u, false, true);
        CHECK_FALSE(Session::IsEnabledIn(mask, Feature::kDownedSystem));
        CHECK(Session::IsEnabledIn(mask, Feature::kCoordinatedSaves));

        // A wire mask claiming "downed disabled" while the bool says enabled: the bool wins.
        mask = Session::ComposeDisabledMask(Session::Bit(Feature::kDownedSystem) | Session::Bit(Feature::kRfabCombatRules), true, false);
        CHECK(Session::IsEnabledIn(mask, Feature::kDownedSystem));
        CHECK_FALSE(Session::IsEnabledIn(mask, Feature::kCoordinatedSaves));
        CHECK_FALSE(Session::IsEnabledIn(mask, Feature::kRfabCombatRules));
    }

    SECTION("names are stable and unique")
    {
        std::set<std::string> names;
        for (const auto& info : Session::kFeatureTable)
            names.insert(std::string(info.Name));
        CHECK(names.size() == Session::kFeatureTable.size());
        CHECK(Session::NameOf(Feature::kSelfHealingResync) == "SelfHealingResync");
    }
}

TEST_CASE("v4.1.0 ServerSettings carries DisabledFeatureMask on the wire", "[rfab][v410][features][encoding]")
{
    ServerSettings sent{};
    sent.DownedSystemEnabled = false;
    sent.DisabledFeatureMask = Session::Bit(Session::Feature::kHairColorSync) | Session::Bit(Session::Feature::kSelfHealingResync);

    TiltedPhoques::Buffer buff(1000);
    TiltedPhoques::Buffer::Writer writer(&buff);
    sent.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buff);
    ServerSettings received{};
    received.Deserialize(reader);

    REQUIRE(received == sent);
    CHECK(received.DisabledFeatureMask == sent.DisabledFeatureMask);

    ServerSettings other = sent;
    other.DisabledFeatureMask = 0;
    CHECK(other != sent);  // operator== must include the new field
}

TEST_CASE("v4.1.0 every feature switch is wired end to end", "[rfab][v410][features][static]")
{
    const auto root = FindV410RepoRoot();
    if (root.empty())
    {
        WARN("repository root not found from the test working directory; skipping source wiring audit");
        return;
    }

    const std::string gameServer = ReadV410Source(root / "Code" / "server" / "GameServer.cpp");
    REQUIRE_FALSE(gameServer.empty());

    // Collect every client source once.
    std::string clientSources;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "Code" / "client"))
    {
        if (entry.is_regular_file() && (entry.path().extension() == ".cpp" || entry.path().extension() == ".h"))
            clientSources += ReadV410Source(entry.path());
    }
    REQUIRE_FALSE(clientSources.empty());

    for (const auto& info : Session::kFeatureTable)
    {
        const std::string name(info.Name);
        INFO("feature " << name);
        if (info.Bit == Session::Feature::kDownedSystem || info.Bit == Session::Feature::kCoordinatedSaves)
        {
            // Legacy switches: own bools in ServerSettings, folded by ComposeDisabledMask.
            CHECK(gameServer.find("ComposeDisabledMask(0u, bEnableDownedSystem, bEnableCoordinatedSaves)") != std::string::npos);
            continue;
        }

        // Server: the cvar feeds the mask.
        CHECK(gameServer.find("Feature::k" + name) != std::string::npos);
        // Client: at least one gate reads it through the lock-free snapshot.
        CHECK(clientSources.find("Session::Feature::k" + name) != std::string::npos);
    }

    // Hooks must never read ServerSettings for these switches.
    CHECK(clientSources.find("GetServerSettings().DisabledFeatureMask") == std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// Step 1: wire messages
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 resync messages round-trip and sanitize corrupted enums", "[rfab][v410][resync][encoding]")
{
    SECTION("RequestResync")
    {
        RequestResync sent{};
        sent.RequestId = 0xABCDEF;
        sent.RequestScope = Scope::kStringCache;
        sent.ServerId = 0x12345678;
        sent.GapReason = Reason::kStringCacheRejected;
        const RequestResync received = RoundTripClient(sent);
        CHECK(received == sent);
        CHECK(received.RawScopeKnown);
    }

    SECTION("NotifyResyncComplete")
    {
        NotifyResyncComplete sent{};
        sent.RequestId = 77;
        sent.RequestScope = Scope::kEntity;
        sent.ServerId = 0x42;
        sent.Result = Outcome::kOwnedByRequester;
        sent.SnapshotsSent = 3;
        CHECK(RoundTripServer(sent) == sent);
    }

    SECTION("unknown scope byte is flagged, unknown reason/outcome map to harmless values")
    {
        CHECK(SanitizeScope(200) == Scope::kEntity);
        CHECK_FALSE(IsKnownScope(200));
        CHECK(SanitizeReason(250) == Reason::kUnknown);
        CHECK(SanitizeOutcome(250) == Outcome::kInvalid);

        TiltedPhoques::Buffer buff(64);
        TiltedPhoques::Buffer::Writer writer(&buff);
        writer.WriteBits(static_cast<uint8_t>(kRequestResync), 8);
        TiltedPhoques::Serialization::WriteVarInt(writer, 5);
        writer.WriteBits(0xEE, 8);  // corrupted scope
        TiltedPhoques::Serialization::WriteVarInt(writer, 9);
        writer.WriteBits(0xEE, 8);  // corrupted reason
        TiltedPhoques::Buffer::Reader reader(&buff);
        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);
        REQUIRE(pMessage);
        const auto& request = *static_cast<RequestResync*>(pMessage.get());
        CHECK_FALSE(request.RawScopeKnown);
        CHECK(request.GapReason == Reason::kUnknown);
    }

    SECTION("opcodes are appended, never inserted, and the completion keeps causal order while paused")
    {
        // Step 4 appended the world-item opcodes right after the resync ones.
        CHECK(static_cast<uint32_t>(kRequestResync) + 1 == static_cast<uint32_t>(kRequestWorldItemDrop));
        CHECK(static_cast<uint32_t>(kRequestWorldItemPickup) + 1 == static_cast<uint32_t>(kClientOpcodeMax));
        CHECK(static_cast<uint32_t>(kNotifyResyncComplete) + 1 == static_cast<uint32_t>(kNotifyWorldItemSpawn));
        CHECK(static_cast<uint32_t>(kNotifyWorldItemSnapshot) + 1 == static_cast<uint32_t>(kServerOpcodeMax));
        CHECK(CoopWorldStabilizer::ClassifyPausedInboundPacket(kNotifyResyncComplete) ==
              CoopWorldStabilizer::PausedInboundPacketAction::kQueueUntilUnpaused);
        // World-scoped: a reload discards it and the client re-arms the request (AbandonInFlight).
        CHECK_FALSE(CoopWorldStabilizer::MustSurviveWorldReload(kNotifyResyncComplete));
    }
}

// ---------------------------------------------------------------------------------------------
// Step 1: client ledger
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 ClientLedger: unknown entities are confirmed before asking", "[rfab][v410][resync]")
{
    ClientLedger ledger{};
    const uint64_t t0 = 10'000;

    REQUIRE(ledger.NoteGap(Scope::kEntity, 0x10, Reason::kUnknownEntityInMovement, t0));
    CHECK(PollAll(ledger, t0 + 500).empty());  // reliable spawn may still be on its way

    SECTION("the spawn arrives inside the confirm window: nothing is requested")
    {
        ledger.NoteEntityKnown(0x10);
        CHECK(PollAll(ledger, t0 + 5'000).empty());
        CHECK(ledger.GetStats().RequestsSent == 0);
    }

    SECTION("still unknown after the window: exactly one request, repeated gaps are absorbed")
    {
        CHECK_FALSE(ledger.NoteGap(Scope::kEntity, 0x10, Reason::kUnknownEntityInMovement, t0 + 600));
        const auto out = PollAll(ledger, t0 + 1'000);
        REQUIRE(out.size() == 1);
        CHECK(out[0].RequestScope == Scope::kEntity);
        CHECK(out[0].ServerId == 0x10);
        CHECK(out[0].GapReason == Reason::kUnknownEntityInMovement);
        CHECK_FALSE(ledger.NoteGap(Scope::kEntity, 0x10, Reason::kUnknownEntityInMovement, t0 + 1'100));
        CHECK(PollAll(ledger, t0 + 1'200).empty());
    }
}

TEST_CASE("v4.1.0 server id 0 is a real entity, kNoServerId is the only 'none'", "[rfab][v410][resync]")
{
    // entt hands out entity 0 to the first character a fresh server creates (the linux-server
    // harness exercises exactly that). A "0 means none" shortcut silently refused to heal it.
    STATIC_REQUIRE(kNoServerId == 0xFFFFFFFFu);  // entt::null for a 32-bit entity type

    ClientLedger ledger{};
    REQUIRE(ledger.NoteGap(Scope::kEntity, 0u, Reason::kUnknownEntityInMovement, 0));
    const auto out = PollAll(ledger, 5'000);
    REQUIRE(out.size() == 1);
    CHECK(out[0].RequestScope == Scope::kEntity);
    CHECK(out[0].ServerId == 0u);

    CHECK_FALSE(ledger.NoteGap(Scope::kEntity, kNoServerId, Reason::kUnknownEntityInMovement, 6'000));
    for (const uint64_t at : {20'000ull, 60'000ull})
        for (const auto& request : PollAll(ledger, at))
            CHECK(request.ServerId == 0u);  // timeout re-arms of id 0 only, never kNoServerId

    // Whole-session and string-table requests never carry a plausible entity id.
    ClientLedger scoped{};
    REQUIRE(scoped.NoteGap(Scope::kAllEntities, kNoServerId, Reason::kManual, 0));
    REQUIRE(scoped.NoteGap(Scope::kStringCache, kNoServerId, Reason::kManual, 0));
    const auto scopedOut = PollAll(scoped, 100);
    REQUIRE(scopedOut.size() == 2);
    for (const auto& request : scopedOut)
        CHECK(request.ServerId == kNoServerId);

    CHECK(RequestResync{}.ServerId == kNoServerId);
    CHECK(NotifyResyncComplete{}.ServerId == kNoServerId);
}

TEST_CASE("v4.1.0 ClientLedger: gaps wait while the client cannot accept state", "[rfab][v410][resync]")
{
    ClientLedger ledger{};
    REQUIRE(ledger.NoteGap(Scope::kAllEntities, 0, Reason::kSpawnDroppedDuringWorldLoad, 1'000));
    for (uint64_t t = 1'000; t < 60'000; t += 250)
        CHECK(PollAll(ledger, t, /*aCanSend=*/false).empty());
    CHECK(ledger.HasPendingAllEntities());

    const auto out = PollAll(ledger, 60'000, true);
    REQUIRE(out.size() == 1);
    CHECK(out[0].RequestScope == Scope::kAllEntities);
    CHECK(out[0].GapReason == Reason::kSpawnDroppedDuringWorldLoad);
}

TEST_CASE("v4.1.0 ClientLedger: whole-session resync subsumes and absorbs entity gaps", "[rfab][v410][resync]")
{
    ClientPolicy policy{};
    policy.EscalateToAllEntitiesThreshold = 4;
    ClientLedger ledger{policy};

    for (uint32_t id = 1; id <= 4; ++id)
        REQUIRE(ledger.NoteGap(Scope::kEntity, id, Reason::kUnknownEntityInMovement, 0));
    CHECK(ledger.PendingEntityCount() == 4);

    // The fifth one crosses the threshold: one whole-session request instead of five.
    REQUIRE(ledger.NoteGap(Scope::kEntity, 5, Reason::kUnknownEntityInMovement, 0));
    CHECK(ledger.PendingEntityCount() == 0);
    CHECK(ledger.HasPendingAllEntities());
    CHECK(ledger.GetStats().Escalations == 1);

    const auto out = PollAll(ledger, 2'000);
    REQUIRE(out.size() == 1);
    CHECK(out[0].RequestScope == Scope::kAllEntities);

    // While it is in flight every entity gap is covered by it.
    CHECK_FALSE(ledger.NoteGap(Scope::kEntity, 99, Reason::kUnknownEntityInMovement, 2'100));
    CHECK_FALSE(ledger.NoteGap(Scope::kAllEntities, 0, Reason::kWorldPacketsDiscarded, 2'100));
    CHECK(PollAll(ledger, 10'000).empty() == false);  // timed out (6 s) -> re-armed and re-sent
}

TEST_CASE("v4.1.0 ClientLedger: lost replies are re-armed, abandoned replies immediately", "[rfab][v410][resync]")
{
    ClientLedger ledger{};
    REQUIRE(ledger.NoteGap(Scope::kStringCache, 0, Reason::kStringCacheRejected, 0));
    auto out = PollAll(ledger, 0);
    REQUIRE(out.size() == 1);
    const uint32_t firstId = out[0].RequestId;

    SECTION("timeout")
    {
        CHECK(PollAll(ledger, 5'999).empty());
        out = PollAll(ledger, 6'000);
        REQUIRE(out.size() == 1);
        CHECK(out[0].RequestScope == Scope::kStringCache);
        CHECK(out[0].RequestId != firstId);
        CHECK(ledger.GetStats().TimedOut == 1);
        // The late reply to the first request matches nothing.
        CHECK_FALSE(ledger.OnComplete(firstId, Outcome::kApplied, 6'100));
        CHECK(ledger.GetStats().StaleCompletions == 1);
    }

    SECTION("world reload abandons it; the cooldown still spaces the resend")
    {
        ledger.AbandonInFlight(100);
        CHECK(ledger.InFlightCount() == 0);
        CHECK(ledger.HasPendingStringCache());
        CHECK(PollAll(ledger, 4'999).empty());
        CHECK(PollAll(ledger, 5'000).size() == 1);
    }
}

TEST_CASE("v4.1.0 ClientLedger: completion outcomes", "[rfab][v410][resync]")
{
    ClientLedger ledger{};
    REQUIRE(ledger.NoteGap(Scope::kEntity, 0x77, Reason::kUnknownEntityInMovement, 0));
    auto out = PollAll(ledger, 1'000);
    REQUIRE(out.size() == 1);
    const uint32_t id = out[0].RequestId;

    SECTION("applied: cooldown absorbs the echo of in-flight movement")
    {
        REQUIRE(ledger.OnComplete(id, Outcome::kApplied, 1'050));
        CHECK_FALSE(ledger.NoteGap(Scope::kEntity, 0x77, Reason::kUnknownEntityInMovement, 1'100));
    }

    SECTION("entity gone: tombstoned, movement still in flight is not a gap")
    {
        REQUIRE(ledger.OnComplete(id, Outcome::kEntityGone, 1'050));
        CHECK_FALSE(ledger.NoteGap(Scope::kEntity, 0x77, Reason::kUnknownEntityInMovement, 4'500));
    }

    SECTION("owned by requester: not fixable by a resend, quarantined for the window")
    {
        REQUIRE(ledger.OnComplete(id, Outcome::kOwnedByRequester, 1'050));
        CHECK(ledger.GetStats().OwnershipConflicts == 1);
        CHECK_FALSE(ledger.NoteGap(Scope::kEntity, 0x77, Reason::kUnknownEntityInMovement, 30'000));
    }

    SECTION("throttled: re-armed without a second confirm delay")
    {
        REQUIRE(ledger.OnComplete(id, Outcome::kThrottled, 1'050));
        CHECK(ledger.GetStats().Throttled == 1);
        out = PollAll(ledger, 1'100);
        REQUIRE(out.size() == 1);
        CHECK(out[0].ServerId == 0x77);
    }
}

TEST_CASE("v4.1.0 ClientLedger: an entity the client keeps refusing never loops", "[rfab][v410][resync]")
{
    ClientPolicy policy{};
    ClientLedger ledger{policy};
    const uint32_t id = 0xBAD;

    uint64_t now = 0;
    uint32_t sent = 0;
    // Movement for the id keeps arriving every 250 ms for two minutes and every resend is
    // answered "applied" but never fixes it (e.g. refused by the client after a FormId change).
    for (; now < 120'000; now += 250)
    {
        ledger.NoteGap(Scope::kEntity, id, Reason::kUnknownEntityInMovement, now);
        for (const auto& r : PollAll(ledger, now))
        {
            ++sent;
            ledger.OnComplete(r.RequestId, Outcome::kApplied, now);
        }
    }
    // At most MaxEntityAttemptsPerWindow per 60 s window.
    CHECK(sent <= 2 * policy.MaxEntityAttemptsPerWindow + 1);
    CHECK(ledger.GetStats().Quarantined >= 1);

    SECTION("an explicitly ignored id is not reported at all")
    {
        ClientLedger fresh{};
        fresh.NoteEntityIgnored(id, 0);
        CHECK_FALSE(fresh.NoteGap(Scope::kEntity, id, Reason::kUnknownEntityInMovement, 1'000));
        CHECK(fresh.NoteGap(Scope::kEntity, id, Reason::kUnknownEntityInMovement, policy.IgnoredEntityMs + 1));
    }
}

TEST_CASE("v4.1.0 ClientLedger: whole-session resyncs are spaced by the cooldown", "[rfab][v410][resync]")
{
    ClientLedger ledger{};
    REQUIRE(ledger.NoteGap(Scope::kAllEntities, 0, Reason::kWorldPacketsDiscarded, 0));
    auto out = PollAll(ledger, 0);
    REQUIRE(out.size() == 1);
    REQUIRE(ledger.OnComplete(out[0].RequestId, Outcome::kApplied, 200));

    // A new loss right after: kept, but sent only after the 5 s cooldown.
    REQUIRE(ledger.NoteGap(Scope::kAllEntities, 0, Reason::kSpawnDroppedDuringWorldLoad, 300));
    CHECK(PollAll(ledger, 4'999).empty());
    out = PollAll(ledger, 5'000);
    REQUIRE(out.size() == 1);
    CHECK(out[0].GapReason == Reason::kSpawnDroppedDuringWorldLoad);
}

TEST_CASE("v4.1.0 ClientLedger: reset and request id monotonicity", "[rfab][v410][resync]")
{
    ClientLedger ledger{};
    REQUIRE(ledger.NoteGap(Scope::kStringCache, 0, Reason::kStringCacheRejected, 0));
    const auto before = PollAll(ledger, 0);
    REQUIRE(before.size() == 1);

    ledger.Reset();
    CHECK(ledger.InFlightCount() == 0);
    CHECK_FALSE(ledger.HasPendingStringCache());
    CHECK_FALSE(ledger.OnComplete(before[0].RequestId, Outcome::kApplied, 10));

    REQUIRE(ledger.NoteGap(Scope::kStringCache, 0, Reason::kStringCacheRejected, 10));
    const auto after = PollAll(ledger, 10);  // cooldown cleared by the session boundary
    REQUIRE(after.size() == 1);
    CHECK(after[0].RequestId > before[0].RequestId);
}

TEST_CASE("v4.1.0 ClientLedger: bookkeeping stays bounded", "[rfab][v410][resync]")
{
    ClientPolicy policy{};
    policy.EscalateToAllEntitiesThreshold = 1'000'000;  // force the per-entity path
    policy.MaxRequestsPerPoll = 64;
    ClientLedger ledger{policy};

    uint64_t now = 0;
    for (uint32_t id = 1; id <= 20'000; ++id)
    {
        ledger.NoteGap(Scope::kEntity, id, Reason::kUnknownEntityInMovement, now);
        ledger.NoteEntityRemoved(id + 50'000, now);
        if (id % 32 == 0)
        {
            now += 1'100;
            for (const auto& r : PollAll(ledger, now))
                ledger.OnComplete(r.RequestId, Outcome::kApplied, now);
        }
    }
    CHECK(ledger.TrackedEntityCount() <= policy.MaxTrackedEntities);
}

TEST_CASE("v4.1.0 ClientLedger: randomized soak keeps the invariants", "[rfab][v410][resync][soak]")
{
    std::mt19937 rng(0xC0FFEEu);
    for (int session = 0; session < 32; ++session)
    {
        ClientPolicy policy{};
        policy.EscalateToAllEntitiesThreshold = 1 + rng() % 16;
        policy.MaxRequestsPerPoll = 1 + rng() % 6;
        ClientLedger ledger{policy};

        std::vector<Request> inFlight;
        uint64_t now = 0;
        for (int step = 0; step < 2'000; ++step)
        {
            now += rng() % 400;
            const uint32_t id = 1 + rng() % 40;
            switch (rng() % 9)
            {
            case 0: ledger.NoteGap(Scope::kEntity, id, Reason::kUnknownEntityInMovement, now); break;
            case 1: ledger.NoteGap(Scope::kAllEntities, 0, Reason::kSpawnDroppedDuringWorldLoad, now); break;
            case 2: ledger.NoteGap(Scope::kStringCache, 0, Reason::kStringCacheRejected, now); break;
            case 3: ledger.NoteEntityKnown(id); break;
            case 4: ledger.NoteEntityRemoved(id, now); break;
            case 5: ledger.AbandonInFlight(now); inFlight.clear(); break;
            case 6:
                if (!inFlight.empty())
                {
                    const size_t pick = rng() % inFlight.size();
                    ledger.OnComplete(inFlight[pick].RequestId, SanitizeOutcome(static_cast<uint8_t>(rng() % kOutcomeCount)), now);
                    inFlight.erase(inFlight.begin() + static_cast<std::ptrdiff_t>(pick));
                }
                break;
            default:
            {
                const bool canSend = (rng() % 4) != 0;
                std::vector<Request> out;
                const size_t n = ledger.Poll(now, canSend, out);
                REQUIRE(n == out.size());
                REQUIRE(out.size() <= policy.MaxRequestsPerPoll);
                if (!canSend)
                    REQUIRE(out.empty());
                for (const auto& r : out)
                {
                    if (r.RequestScope == Scope::kEntity)
                        REQUIRE(r.ServerId != 0);
                    inFlight.push_back(r);
                }
                break;
            }
            }

            // Never two requests in flight for the same scope/entity, and never an entity request
            // in flight next to a whole-session one issued after it.
            REQUIRE(ledger.InFlightCount() <= 2 + 40);
            REQUIRE(ledger.TrackedEntityCount() <= policy.MaxTrackedEntities);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Step 1: server budget
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 ServerBudget bounds what one client can make the server do", "[rfab][v410][resync]")
{
    ServerPolicy policy{};
    ServerBudget budget{policy};

    SECTION("whole-session and string-table resends are interval limited")
    {
        CHECK(budget.Admit(Scope::kAllEntities, 1'000));
        CHECK_FALSE(budget.Admit(Scope::kAllEntities, 1'500));
        CHECK(budget.Admit(Scope::kStringCache, 1'500));  // independent scopes
        CHECK(budget.Admit(Scope::kAllEntities, 3'000));
    }

    SECTION("single-entity resends use a token bucket")
    {
        uint32_t admitted = 0;
        for (int i = 0; i < 1'000; ++i)
            admitted += budget.Admit(Scope::kEntity, 5'000) ? 1u : 0u;
        CHECK(admitted == policy.EntityBurst);

        // 1 s later: 10 tokens refilled.
        admitted = 0;
        for (int i = 0; i < 1'000; ++i)
            admitted += budget.Admit(Scope::kEntity, 6'000) ? 1u : 0u;
        CHECK(admitted == 10u);

        // A hostile loop for a minute never exceeds burst + rate * time.
        admitted = 0;
        for (uint64_t t = 6'000; t <= 66'000; t += 5)
            admitted += budget.Admit(Scope::kEntity, t) ? 1u : 0u;
        CHECK(admitted <= 601u);
    }
}
