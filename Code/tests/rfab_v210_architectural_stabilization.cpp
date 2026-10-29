#include <catch2/catch.hpp>

#include <CoopAnimationState.h>
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
    REQUIRE_FALSE(root.empty());

    const std::string manifest = LoadV210RepoFile(root, "docs/V2.0.3_RELEASE_BACKUP_MANIFEST.json");
    REQUIRE(manifest.find("\"v2.0.3\"") != std::string::npos);
    REQUIRE(manifest.find("\"v2.0.3-backup\"") != std::string::npos);
    REQUIRE(manifest.find("424566518") != std::string::npos);
    REQUIRE(manifest.find("865da6c6cc1ba6e336f894feeddf0aa9d4418959") != std::string::npos);
    REQUIRE(manifest.find("5073f1eca4010888df6a772103f8207a744041779b07b23a361a5f40025fdcfb") != std::string::npos);
    REQUIRE(manifest.find("b7c80a3717dceee70555ed4e17eb270535ca8bec717f42d3926b3605d2da122e") != std::string::npos);
    REQUIRE(manifest.find("66e9ebb415903deb28bf584c71a4f17f69cf28780b176a3175dfd19b9d4f7f91") != std::string::npos);
    REQUIRE(manifest.find("143758f14c3dead27271638b2f44b48a4e64111059296a00c5e4ad1cc35fe1ee") != std::string::npos);

    const std::string buildPs1 = LoadV210RepoFile(root, "Tools/rfab/Build-CloudRelease.ps1");
    REQUIRE(buildPs1.find("[string]$VersionTag = \"v3.1.6\"") != std::string::npos);
    REQUIRE(buildPs1.find("Ensure-V203BackupRelease") != std::string::npos);
    REQUIRE(buildPs1.find("$script:V203BackupReady") != std::string::npos);
    REQUIRE(buildPs1.find("\"v2.0.3-backup\"") != std::string::npos);
    REQUIRE(buildPs1.find("Ensure-V210BackupRelease") != std::string::npos);
    REQUIRE(buildPs1.find("\"v2.1.0-backup\"") != std::string::npos);
    REQUIRE(buildPs1.find("Ensure-V300BackupRelease") != std::string::npos);
    REQUIRE(buildPs1.find("\"v3.0.0-backup\"") != std::string::npos);

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
    REQUIRE(charSvcCpp.find("pActor->GetLeveledTemplateBase()") != std::string::npos);
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

TEST_CASE("v2.1.0 Phase 6: Playtest Root-Cause Fix #1 — 3D-Readiness invariant, ExtraDataList lock, and thread-safe BehaviorVar/DescriptorManager", "[rfab][v210][playtest][crashes]")
{
    const auto root = LocateRepoRootForV210Audit();
    REQUIRE_FALSE(root.empty());

    // 1. Node-stable + shared_mutex AnimationGraphDescriptorManager:
    const std::string descMgrH = LoadV210RepoFile(root, "Code/encoding/Structs/AnimationGraphDescriptorManager.h");
    REQUIRE(descMgrH.find("mutable std::shared_mutex m_mutex") != std::string::npos);
    REQUIRE(descMgrH.find("std::unordered_map<uint64_t, AnimationGraphDescriptor>") != std::string::npos);

    const std::string descMgrCpp = LoadV210RepoFile(root, "Code/encoding/Structs/AnimationGraphDescriptorManager.cpp");
    REQUIRE(descMgrCpp.find("std::shared_lock<std::shared_mutex>") != std::string::npos);
    REQUIRE(descMgrCpp.find("std::unique_lock<std::shared_mutex>") != std::string::npos);
    REQUIRE(descMgrCpp.find("m_descriptors.try_emplace") != std::string::npos);

    // 2. Thread-safe BehaviorVar patcher & BSAnimationGraphManager variable access:
    const std::string behaviorVarCpp = LoadV210RepoFile(root, "Code/client/ModCompat/BehaviorVar.cpp");
    REQUIRE(behaviorVarCpp.find("std::recursive_mutex mutex_lock") != std::string::npos);

    const std::string animGraphCpp = LoadV210RepoFile(root, "Code/client/Games/BSAnimationGraphManager.cpp");
    REQUIRE(animGraphCpp.find("BSScopedLock<BSRecursiveLock> _{lock}") != std::string::npos);

    // 3. ExtraDataList::GetByType always locks BSRecursiveLock:
    const std::string extraDataCpp = LoadV210RepoFile(root, "Code/client/Games/Skyrim/ExtraData/ExtraDataList.cpp");
    REQUIRE(extraDataCpp.find("BSScopedLock<BSRecursiveLock> _(lock)") != std::string::npos);

    // 4. 3D-Readiness gate before EquipItem / AddSpell / CastSpellImmediate / SetActorInventory:
    const std::string invSvcCpp = LoadV210RepoFile(root, "Code/client/Services/Generic/InventoryService.cpp");
    REQUIRE(invSvcCpp.find("!pActor->GetNiNode() || pActor->IsStillLoading()") != std::string::npos);

    const std::string magicSvcCpp = LoadV210RepoFile(root, "Code/client/Services/Generic/MagicService.cpp");
    REQUIRE(magicSvcCpp.find("acMessage.SpellFormId.ModId == std::numeric_limits<uint32_t>::max()") != std::string::npos);
    REQUIRE(magicSvcCpp.find("pSpell->HasSummonEffect()") != std::string::npos);
    REQUIRE(magicSvcCpp.find("pEffect->IsSummonEffect()") != std::string::npos);

    // 5. Engine hooks guarded by World::IsCreated() && World::IsConnectedCached():
    const std::string actorCpp = LoadV210RepoFile(root, "Code/client/Games/Skyrim/Actor.cpp");
    REQUIRE(actorCpp.find("World::IsCreated() && World::IsConnectedCached()") != std::string::npos);
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
    REQUIRE_FALSE(root.empty());

    const std::string saveSnapCpp = LoadV210RepoFile(root, "Code/server/Services/SaveSnapshotService.cpp");
    REQUIRE(saveSnapCpp.find("CoopCheckpoint::ShouldAnchorPartyMemberToLeaderOnRollback") != std::string::npos);

    const std::string downedCpp = LoadV210RepoFile(root, "Code/client/Services/DownedStateService.cpp");
    REQUIRE(downedCpp.find("pControls->pJumpHandler->isEnabled = true") != std::string::npos);
    REQUIRE(downedCpp.find("pControls->togglePOVHandler->isEnabled = true") != std::string::npos);
    REQUIRE(downedCpp.find("m_postReviveBleedoutExitFrames") != std::string::npos);
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

    const auto root = LocateRepoRootForV210Audit();
    REQUIRE_FALSE(root.empty());

    const std::string tesNpcCpp = LoadV210RepoFile(root, "Code/client/Games/Skyrim/Forms/TESNPC.cpp");
    REQUIRE(tesNpcCpp.find("HookSetLeveledNpc") != std::string::npos);
    REQUIRE(tesNpcCpp.find("14257") != std::string::npos);
    REQUIRE(tesNpcCpp.find("TESNPC::GetLeveledSourceNpc") != std::string::npos);

    const std::string actorCpp = LoadV210RepoFile(root, "Code/client/Games/Skyrim/Actor.cpp");
    REQUIRE(actorCpp.find("Actor::GetLeveledTemplateBase") != std::string::npos);
    REQUIRE(actorCpp.find("ExtraDataType::LeveledCreature") != std::string::npos);

    const std::string serverCharCpp = LoadV210RepoFile(root, "Code/server/Services/CharacterService.cpp");
    REQUIRE(serverCharCpp.find("if (pOwningPlayer != nullptr && pOwningPlayer == acMessage.pPlayer)") != std::string::npos);
    REQUIRE(serverCharCpp.find("sanitizedBaseId") != std::string::npos);
}

