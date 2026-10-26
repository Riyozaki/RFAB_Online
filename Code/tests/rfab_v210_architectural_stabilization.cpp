#include <catch2/catch.hpp>

#include <CoopCheckpointStore.h>
#include <CoopDamagePipeline.h>
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
    auto cursor = std::filesystem::current_path(ec);
    for (int depth = 0; depth < 8 && !cursor.empty(); ++depth)
    {
        if (std::filesystem::exists(cursor / "Code" / "client" / "World.h", ec) &&
            std::filesystem::exists(cursor / "docs" / "V2.0.3_RELEASE_BACKUP_MANIFEST.json", ec))
        {
            return cursor;
        }
        cursor = cursor.parent_path();
    }
    return {};
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
    REQUIRE_FALSE(root.empty());

    const std::string manifest = LoadV210RepoFile(root, "docs/V2.0.3_RELEASE_BACKUP_MANIFEST.json");
    REQUIRE(manifest.find("\"v2.0.3\"") != std::string::npos);
    REQUIRE(manifest.find("\"v2.0.3-backup\"") != std::string::npos);
    REQUIRE(manifest.find("424566518") != std::string::npos);
    REQUIRE(manifest.find("e7b96ef70276d29a916012c3489359a9bd736ce2") != std::string::npos);
    REQUIRE(manifest.find("0b03101a09bf4beba42a707561a5a39d1f9abba7edd2f939354092bfb6870689") != std::string::npos);
    REQUIRE(manifest.find("1fe98cfb3f02715b59e48b1798fa7ef3b7fd366ac79653b4d4afae5554e84a53") != std::string::npos);
    REQUIRE(manifest.find("db297e2113c56416db6bcec2eb25d3b0cfa7489d93d4838074ff960721d20844") != std::string::npos);
    REQUIRE(manifest.find("c20d0734032ad0ea8a1a3e1bfc0620729bdbbc40a3c3767e82c74fc62ecb1f1f") != std::string::npos);

    const std::string buildPs1 = LoadV210RepoFile(root, "Tools/rfab/Build-CloudRelease.ps1");
    REQUIRE(buildPs1.find("[string]$VersionTag = \"v2.1.0\"") != std::string::npos);
    REQUIRE(buildPs1.find("Ensure-V203BackupRelease") != std::string::npos);
    REQUIRE(buildPs1.find("$script:V203BackupReady") != std::string::npos);
    REQUIRE(buildPs1.find("\"v2.0.3-backup\"") != std::string::npos);

    REQUIRE(RfabCompat::AreBuildVersionsProtocolCompatible("v2.1.0", "v2.1.0"));
    REQUIRE(RfabCompat::AreBuildVersionsProtocolCompatible("v2.1.0", "v2.0.3"));
    REQUIRE(RfabCompat::AreBuildVersionsProtocolCompatible("v2.0.3", "v2.1.0"));
    REQUIRE_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("v2.1.0", "v1.2.1"));
}

TEST_CASE("v2.1.0 Phase 1: Single-Source-of-Truth RemoteEntityModel and O(1) ClientWorldView entity lookups", "[rfab][v210][phase1]")
{
    const auto root = LocateRepoRootForV210Audit();
    REQUIRE_FALSE(root.empty());

    const std::string charSvcH = LoadV210RepoFile(root, "Code/client/Services/CharacterService.h");
    REQUIRE(charSvcH.find("bool HasAuthoritativeSpawnPacket{false};") != std::string::npos);
    REQUIRE(charSvcH.find("ResolveAuthoritativeSpawnState") != std::string::npos);

    const std::string charSvcCpp = LoadV210RepoFile(root, "Code/client/Services/Generic/CharacterService.cpp");
    REQUIRE(charSvcCpp.find("ResolveAuthoritativeSpawnState(entity, waitingFor3D.SpawnRequest)") != std::string::npos);
    REQUIRE(charSvcCpp.find("model.HasAuthoritativeSpawnPacket = true;") != std::string::npos);
    REQUIRE(charSvcCpp.find("if (model.SpawnRequest.BaseId == GameId{} && pActor->baseForm)") != std::string::npos);
    REQUIRE(charSvcCpp.find("if (!pActor->GetNiNode())") != std::string::npos);
    REQUIRE(charSvcCpp.find("std::find_if") == std::string::npos);

    const std::string magicSvcCpp = LoadV210RepoFile(root, "Code/client/Services/Generic/MagicService.cpp");
    REQUIRE(magicSvcCpp.find("std::find_if") == std::string::npos);
    REQUIRE(magicSvcCpp.find("FindActorEntityByFormId") != std::string::npos);
    REQUIRE(magicSvcCpp.find("FindEntityByServerId") != std::string::npos);

    const std::string combatSvcCpp = LoadV210RepoFile(root, "Code/client/Services/Generic/CombatService.cpp");
    REQUIRE(combatSvcCpp.find("std::find_if") == std::string::npos);

    const std::string objectSvcCpp = LoadV210RepoFile(root, "Code/client/Services/Generic/ObjectService.cpp");
    REQUIRE(objectSvcCpp.find("std::find_if") == std::string::npos);
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

TEST_CASE("v2.1.0 Phase 3: ServerCombatAuthorityLedger non-owner damage, post-downed grace, and ally protection", "[rfab][v210][phase3]")
{
    CoopDamagePipeline::ServerCombatAuthorityLedger<32> ledger;

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
        kPeerId, 0u, CoopDamagePipeline::ServerHitClaimKind::kProjectileLaunch, 2000ULL);
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

    const auto root = LocateRepoRootForV210Audit();
    REQUIRE_FALSE(root.empty());
    const std::string serverCharCpp = LoadV210RepoFile(root, "Code/server/Services/CharacterService.cpp");
    REQUIRE(serverCharCpp.find("EnsureDormantPersistentCharacter") != std::string::npos);

    const std::string saveSnapCpp = LoadV210RepoFile(root, "Code/server/Services/SaveSnapshotService.cpp");
    REQUIRE(saveSnapCpp.find("PersistentActorsByKey") != std::string::npos);
    REQUIRE(saveSnapCpp.find("CoopCheckpoint::TlvTag::kV210PersistentActors") != std::string::npos);
}
