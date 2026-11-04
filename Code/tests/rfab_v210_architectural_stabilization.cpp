#include <catch2/catch.hpp>

#include <CoopAnimationState.h>
#include <CoopCheckpointStore.h>
#include <Authority/Hits.h>
#include <CoopWorldView.h>
#include <RfabModpackCompat.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::filesystem::path LocateRepoRootForV210Audit()
{
    std::error_code ec;
    auto searchUpwards = [&](std::filesystem::path cursor) -> std::filesystem::path
    {
        for (int depth = 0; depth < 8 && !cursor.empty(); ++depth)
        {
            if (std::filesystem::exists(cursor / "Code" / "client" / "World.h", ec) &&
                std::filesystem::exists(cursor / "docs" / "V2.0.3_RELEASE_BACKUP_MANIFEST.json", ec))
            {
                return cursor;
            }
            const auto parent = cursor.parent_path();
            if (parent == cursor)
                break;
            cursor = parent;
        }
        return {};
    };

    if (const auto fromCwd = searchUpwards(std::filesystem::current_path(ec)); !fromCwd.empty())
        return fromCwd;

    return searchUpwards(std::filesystem::path(__FILE__).parent_path());
}

std::string LoadV210RepoFile(const std::filesystem::path& acRoot, const char* apRelPath)
{
    std::ifstream ifs(acRoot / apRelPath, std::ios::binary);
    REQUIRE(ifs.good());
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}
} // namespace

TEST_CASE("v2.1.0 Phase 0: Immutable v2.0.3 backup manifest, release pipeline protection, and v2.0/v2.1 wire compatibility", "[rfab][v210][phase0]")
{
    const auto root = LocateRepoRootForV210Audit();


    REQUIRE(RfabCompat::AreBuildVersionsProtocolCompatible("v2.1.0", "v2.1.0"));
    REQUIRE(RfabCompat::AreBuildVersionsProtocolCompatible("v2.1.0", "v2.0.3"));
    REQUIRE(RfabCompat::AreBuildVersionsProtocolCompatible("v2.0.3", "v2.1.0"));
    REQUIRE_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("v2.1.0", "v1.2.1"));
}


TEST_CASE("v2.1.0 Phase 2: Bidirectional and cell-bucketed ServerInterestRegistry", "[rfab][v210][phase2]")
{
    CoopWorldView::ServerInterestRegistry registry;

    constexpr uint32_t kPlayerA = 1u;
    constexpr uint32_t kPlayerB = 2u;
    constexpr uint32_t kEntityDraugr1 = 101u;
    constexpr uint32_t kEntityDraugr2 = 102u;
    constexpr uint64_t kBleakFallsCell = 0x00015217ULL;
    constexpr uint64_t kWhiterunCell = 0x0001A26FULL;

    registry.BindEntityCell(kEntityDraugr1, kBleakFallsCell);
    registry.BindEntityCell(kEntityDraugr2, kBleakFallsCell);
    REQUIRE(registry.EntitiesInCell(kBleakFallsCell).size() == 2);
    REQUIRE(registry.GetEntityCell(kEntityDraugr1) == kBleakFallsCell);

    REQUIRE(registry.AddObserver(kPlayerA, kEntityDraugr1));
    REQUIRE(registry.AddObserver(kPlayerB, kEntityDraugr1));
    REQUIRE(registry.ObserversOfEntity(kEntityDraugr1).size() == 2);

    // Moving Draugr1 to another cell atomically rebinds its cell bucket:
    registry.BindEntityCell(kEntityDraugr1, kWhiterunCell);
    REQUIRE(registry.EntitiesInCell(kBleakFallsCell).size() == 1);
    REQUIRE(registry.EntitiesInCell(kWhiterunCell).size() == 1);
    REQUIRE(registry.GetEntityCell(kEntityDraugr1) == kWhiterunCell);

    // Removing observer A updates reverse index in O(1):
    REQUIRE(registry.RemoveObserver(kPlayerA, kEntityDraugr1));
    REQUIRE(registry.ObserversOfEntity(kEntityDraugr1).size() == 1);
    REQUIRE(registry.ObserversOfEntity(kEntityDraugr1).contains(kPlayerB));

    // Forgetting Draugr1 clears both observer indices and cell buckets:
    registry.ForgetEntity(kEntityDraugr1);
    REQUIRE(registry.ObserversOfEntity(kEntityDraugr1).empty());
    REQUIRE(registry.EntitiesInCell(kWhiterunCell).empty());
    REQUIRE_FALSE(registry.IsObserving(kPlayerB, kEntityDraugr1));
}

TEST_CASE("v2.1.0 Phase 3: HitLedger non-owner damage, post-downed grace, and ally protection", "[rfab][v210][phase3]")
{
    Authority::HitLedger<32> ledger;

    constexpr uint32_t kPeerId = 2u;
    constexpr uint32_t kBossServerId = 500u;

    // 1. Positive health delta from non-owner is rejected (prevents remote passive regen healing boss):
    REQUIRE_FALSE(ledger.ValidateNonOwnerHealthDelta(
        kPeerId, kBossServerId, +25.0f, /*inRange=*/true, /*downed=*/false, /*dead=*/false, /*partyAlly=*/false, /*pvp=*/false, 1000ULL));

    // 2. Active in-range non-owner damage on enemy NPC is accepted:
    REQUIRE(ledger.ValidateNonOwnerHealthDelta(
        kPeerId, kBossServerId, -80.0f, /*inRange=*/true, /*downed=*/false, /*dead=*/false, /*partyAlly=*/false, /*pvp=*/false, 1000ULL));

    // 3. Friendly fire on party ally / summon is rejected when PvP is disabled:
    REQUIRE_FALSE(ledger.ValidateNonOwnerHealthDelta(
        kPeerId, 501u, -40.0f, /*inRange=*/true, /*downed=*/false, /*dead=*/false, /*partyAlly=*/true, /*pvp=*/false, 1050ULL));
    REQUIRE(ledger.ValidateNonOwnerHealthDelta(
        kPeerId, 501u, -40.0f, /*inRange=*/true, /*downed=*/false, /*dead=*/false, /*partyAlly=*/true, /*pvp=*/true, 1050ULL));

    // 4. Pre-downed projectile launch claim permits one in-flight hit within 1500ms after entering downed state:
    ledger.RecordCombatClaim(
        kPeerId, 0u, Authority::HitKind::kProjectileLaunch, 2000ULL);
    ledger.RecordPlayerDownedState(kPeerId, /*aIsDowned=*/true, 2200ULL);
    REQUIRE(ledger.ValidateNonOwnerHealthDelta(
        kPeerId, kBossServerId, -120.0f, /*inRange=*/true, /*downed=*/true, /*dead=*/false, /*partyAlly=*/false, /*pvp=*/false, 2900ULL));

    // 5. Post-downed damage after the 1500ms in-flight grace window is blocked:
    REQUIRE_FALSE(ledger.ValidateNonOwnerHealthDelta(
        kPeerId, kBossServerId, -120.0f, /*inRange=*/true, /*downed=*/true, /*dead=*/false, /*partyAlly=*/false, /*pvp=*/false, 4000ULL));
}

TEST_CASE("v2.1.0 Phase 4: Reconnect-stable PartySessionLedger and party quest stage deduplication", "[rfab][v210][phase4]")
{
    CoopWorldView::PartySessionLedger partyLedger;
    const uint64_t hostKey = CoopWorldView::PartySessionLedger::HashIdentity("HostDragonborn");
    const uint64_t peerKey = CoopWorldView::PartySessionLedger::HashIdentity("PeerCompanion");

    partyLedger.RecordPartyJoin(hostKey, 1u, /*aIsLeader=*/true, 1000ULL);
    partyLedger.RecordPartyJoin(peerKey, 1u, /*aIsLeader=*/false, 1100ULL);

    // Transient disconnect of host preserves party binding and leader restoration flag:
    partyLedger.RecordDisconnect(hostKey, 5000ULL);
    const auto hostReconnect = partyLedger.FindReconnectParty(hostKey, 12000ULL);
    REQUIRE(hostReconnect.Found);
    REQUIRE(hostReconnect.PartyId == 1u);
    REQUIRE(hostReconnect.RestoreAsLeader);

    // Explicit party leave clears reconnect restoration:
    partyLedger.RecordPartyLeave(peerKey);
    const auto peerReconnect = partyLedger.FindReconnectParty(peerKey, 12000ULL);
    REQUIRE_FALSE(peerReconnect.Found);
}

TEST_CASE("v2.1.0 Phase 5: Self-describing TLV checkpoint extension container and persistent actor hydration", "[rfab][v210][phase5]")
{
    CoopCheckpoint::TlvContainerBuilder builder;
    const uint8_t sampleActorChunk[] = {0x01, 0x02, 0x10, 0x20, 0xFF};
    const uint8_t futureUnknownChunk[] = {0xAA, 0xBB, 0xCC};

    builder.AppendChunk(CoopCheckpoint::TlvTag::kV210PersistentActors, sampleActorChunk, sizeof(sampleActorChunk));
    builder.AppendChunk(static_cast<CoopCheckpoint::TlvTag>(0x9999u), futureUnknownChunk, sizeof(futureUnknownChunk));

    std::vector<CoopCheckpoint::TlvChunkView> chunks;
    REQUIRE(CoopCheckpoint::ParseTlvContainer(builder.Bytes().data(), builder.Bytes().size(), chunks));
    // The views above borrow the builder's buffer; reading them has to be safe.
    REQUIRE(chunks.size() == 2);
    REQUIRE(chunks[0].Tag == static_cast<uint32_t>(CoopCheckpoint::TlvTag::kV210PersistentActors));
    REQUIRE(chunks[0].ByteLength == sizeof(sampleActorChunk));
    REQUIRE(std::memcmp(chunks[0].Data, sampleActorChunk, sizeof(sampleActorChunk)) == 0);
    REQUIRE(chunks[1].Tag == 0x9999u);
    REQUIRE(chunks[1].ByteLength == sizeof(futureUnknownChunk));

    // Corrupted CRC-32C in TLV chunk fails closed:
    std::vector<uint8_t> corrupted = builder.Bytes();
    corrupted.back() ^= 0xFFu;
    std::vector<CoopCheckpoint::TlvChunkView> corruptedChunks;
    REQUIRE_FALSE(CoopCheckpoint::ParseTlvContainer(corrupted.data(), corrupted.size(), corruptedChunks));


}


TEST_CASE("v2.1.0 Phase 7: Playtest Root-Cause Fix #2 — Party-wipe rollback leader anchoring, PlayerControls restoration, and post-revive bleedout enforcer", "[rfab][v210][playtest][rollback]")
{
    // 1. Party-wipe rollback anchors party members to their party leader's checkpoint transform when in different cities/worldspaces:
    REQUIRE(CoopCheckpoint::ShouldAnchorPartyMemberToLeaderOnRollback(
        /*aHasLeaderSnapshot=*/true,
        /*aHasMemberSnapshot=*/true,
        /*aLeaderWorldSpaceBaseId=*/0x3C,
        /*aLeaderCellBaseId=*/0x9BC3,
        /*aLeaderX=*/-36250.0f, /*aLeaderY=*/-2150.0f, /*aLeaderZ=*/-240.0f,
        /*aMemberWorldSpaceBaseId=*/0x37EDF, // SolitudeWorld vs Tamriel!
        /*aMemberCellBaseId=*/0x37EE5,
        /*aMemberX=*/-72716.0f, /*aMemberY=*/105396.0f, /*aMemberZ=*/-8928.0f));
    REQUIRE_FALSE(CoopCheckpoint::ShouldAnchorPartyMemberToLeaderOnRollback(
        /*aHasLeaderSnapshot=*/true,
        /*aHasMemberSnapshot=*/true,
        /*aLeaderWorldSpaceBaseId=*/0x3C,
        /*aLeaderCellBaseId=*/0x9BC3,
        /*aLeaderX=*/-36250.0f, /*aLeaderY=*/-2150.0f, /*aLeaderZ=*/-240.0f,
        /*aMemberWorldSpaceBaseId=*/0x3C,
        /*aMemberCellBaseId=*/0x9BC3,
        /*aMemberX=*/-36100.0f, /*aMemberY=*/-2100.0f, /*aMemberZ=*/-240.0f));
    REQUIRE_FALSE(CoopCheckpoint::ShouldAnchorPartyMemberToLeaderOnRollback(
        /*aHasLeaderSnapshot=*/false,
        /*aHasMemberSnapshot=*/true,
        /*aLeaderWorldSpaceBaseId=*/0x3C,
        /*aLeaderCellBaseId=*/0x9BC3,
        /*aLeaderX=*/0.0f, /*aLeaderY=*/0.0f, /*aLeaderZ=*/0.0f,
        /*aMemberWorldSpaceBaseId=*/0x37EDF,
        /*aMemberCellBaseId=*/0x37EE5,
        /*aMemberX=*/0.0f, /*aMemberY=*/0.0f, /*aMemberZ=*/0.0f));

    const auto root = LocateRepoRootForV210Audit();


}

TEST_CASE("v2.1.0 Phase 8: Playtest Root-Cause Fix #3 & #4 — Leveled NPC static template resolution, voice gender guard, and pre-assigned ownership confirmation", "[rfab][v210][playtest][npcs]")
{
    // Male bandit (aIsActorFemale = false) receiving FemaleEvenToned voice line is flagged as a mismatch:
    REQUIRE(CoopAnimationState::HasVoicePathGenderMismatch(
        "Data\\Sound\\Voice\\Skyrim.esm\\FemaleEvenToned\\combat_taunt_01.fuz",
        /*aIsActorFemale=*/false));
    REQUIRE_FALSE(CoopAnimationState::HasVoicePathGenderMismatch(
        "Data\\Sound\\Voice\\Skyrim.esm\\MaleBandit\\combat_taunt_01.fuz",
        /*aIsActorFemale=*/false));
    REQUIRE_FALSE(CoopAnimationState::HasVoicePathGenderMismatch(
        "Data\\Sound\\Voice\\Skyrim.esm\\CrDraugrVoice\\combat_taunt_01.fuz",
        /*aIsActorFemale=*/false));
    REQUIRE(CoopAnimationState::HasVoicePathGenderMismatch(
        "Data\\Sound\\Voice\\Skyrim.esm\\MaleBandit\\combat_taunt_01.fuz",
        /*aIsActorFemale=*/true));


}

