#include <catch2/catch.hpp>

#include <CoopDamagePipeline.h>
#include <CoopLootLedger.h>
#include <CoopWorldView.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::filesystem::path LocateRepoRootForV203Audit()
{
    std::error_code ec;
    auto cursor = std::filesystem::current_path(ec);
    for (int depth = 0; depth < 8 && !cursor.empty(); ++depth)
    {
        if (std::filesystem::exists(cursor / "Code" / "client" / "World.h", ec) &&
            std::filesystem::exists(cursor / "docs" / "V2.0.2_RELEASE_BACKUP_MANIFEST.json", ec))
        {
            return cursor;
        }
        cursor = cursor.parent_path();
    }
    return {};
}

std::string LoadV203RepoFile(const std::filesystem::path& acRoot, const char* apRelPath)
{
    std::ifstream ifs(acRoot / apRelPath, std::ios::binary);
    REQUIRE(ifs.good());
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}
} // namespace

TEST_CASE("v2.0.3 Phase 0: Immutable v2.0.2 release backup manifest, UTF-8 BOM, and fail-closed telemetry", "[rfab][v203][phase0]")
{
    const auto root = LocateRepoRootForV203Audit();
    REQUIRE_FALSE(root.empty());

    const std::string manifest = LoadV203RepoFile(root, "docs/V2.0.2_RELEASE_BACKUP_MANIFEST.json");
    REQUIRE(manifest.find("\"v2.0.2\"") != std::string::npos);
    REQUIRE(manifest.find("\"v2.0.2-backup\"") != std::string::npos);
    REQUIRE(manifest.find("424319337") != std::string::npos);
    REQUIRE(manifest.find("56215f4288916a62909dc3b934ef5e59273cc1b35e0e24f1ffc966834c22afdf") != std::string::npos);
    REQUIRE(manifest.find("7ac59039ae4146a9fb3c897e893518eab172134bfb5771893799a1211274dce4") != std::string::npos);
    REQUIRE(manifest.find("3110c5876758b2004a8d2b35fc99e2a5fa90548ddd3028895d2eb7c24ea21645") != std::string::npos);
    REQUIRE(manifest.find("a8b603cfbacd19d93a5053b3510eda1e01aad894a68f5a66c3b0cbe5d95dbbde") != std::string::npos);

    for (const char* scriptRel : {"Tools/rfab/Build-CloudRelease.ps1", "Tools/rfab/Package-Release.ps1"})
    {
        const std::string ps1 = LoadV203RepoFile(root, scriptRel);
        REQUIRE(ps1.size() > 3);
        REQUIRE(static_cast<unsigned char>(ps1[0]) == 0xEF);
        REQUIRE(static_cast<unsigned char>(ps1[1]) == 0xBB);
        REQUIRE(static_cast<unsigned char>(ps1[2]) == 0xBF);
    }

    const std::string buildPs1 = LoadV203RepoFile(root, "Tools/rfab/Build-CloudRelease.ps1");
    REQUIRE(buildPs1.find("[string]$VersionTag = \"v3.1.3\"") != std::string::npos);
    REQUIRE(buildPs1.find("Ensure-V202BackupRelease") != std::string::npos);
    REQUIRE(buildPs1.find("$script:V202BackupReady") != std::string::npos);

    const std::string pkgPs1 = LoadV203RepoFile(root, "Tools/rfab/Package-Release.ps1");
    REQUIRE(pkgPs1.find("[string]$VersionTag = \"v3.1.3\"") != std::string::npos);
}

TEST_CASE("v2.0.3 Phase 1: EntityIndexTable overlapping handoffs, reconnect identity, and WorldView parity", "[rfab][v203][phase1]")
{
    SECTION("EntityIndexTable preserves ServerId across overlapping Local<->Remote ownership/mount handoffs and updates replaced keys")
    {
        using Index = CoopWorldView::EntityIndexTable<uint32_t>;
        Index idx;

        constexpr uint32_t kPlacedDraugrEntity = 11u;
        constexpr uint32_t kPlacedDraugrFormId = 0x0003A123u;
        constexpr uint32_t kServerId = 7001u;

        idx.UpsertFormId(kPlacedDraugrEntity, kPlacedDraugrFormId, /*aIsObject=*/false);
        idx.BindServerId(kPlacedDraugrEntity, Index::kRemote, kServerId);
        REQUIRE(idx.FindActorByFormId(kPlacedDraugrFormId).value() == kPlacedDraugrEntity);
        REQUIRE(idx.FindByServerId(kServerId).value() == kPlacedDraugrEntity);

        // `TakeOwnership` emplaces `LocalComponent(kServerId)` BEFORE removing `RemoteComponent(kServerId)`:
        idx.BindServerId(kPlacedDraugrEntity, Index::kLocal, kServerId);
        idx.UnbindServerId(kPlacedDraugrEntity, Index::kRemote);
        REQUIRE(idx.FindByServerId(kServerId).value() == kPlacedDraugrEntity);

        // `OnNotifyMount` / `RelinquishOwnership` emplaces `RemoteComponent(kServerId)` BEFORE removing `LocalComponent(kServerId)`:
        idx.BindServerId(kPlacedDraugrEntity, Index::kRemote, kServerId);
        idx.UnbindServerId(kPlacedDraugrEntity, Index::kLocal);
        REQUIRE(idx.FindByServerId(kServerId).value() == kPlacedDraugrEntity);

        // Live `/disconnect` unbinds `ServerId` while keeping placed `0x00..0xFE` `FormIdComponent` alive for `/connect`:
        idx.UnbindServerId(kPlacedDraugrEntity, Index::kRemote);
        REQUIRE_FALSE(idx.FindByServerId(kServerId).has_value());
        REQUIRE(idx.FindActorByFormId(kPlacedDraugrFormId).value() == kPlacedDraugrEntity);

        // Object entity replacing its `ServerId` in-place (`42 -> 99`) immediately evicts stale `42`:
        constexpr uint32_t kChestEntity = 22u;
        constexpr uint32_t kChestFormId = 0x0009ABCDu;
        idx.UpsertFormId(kChestEntity, kChestFormId, /*aIsObject=*/false);
        idx.PromoteFormToObject(kChestEntity, kChestFormId);
        REQUIRE_FALSE(idx.FindActorByFormId(kChestFormId).has_value());
        REQUIRE(idx.FindObjectByFormId(kChestFormId).value() == kChestEntity);

        idx.BindServerId(kChestEntity, Index::kObject, 42u);
        REQUIRE(idx.FindByServerId(42u).value() == kChestEntity);
        idx.BindServerId(kChestEntity, Index::kObject, 99u);
        REQUIRE_FALSE(idx.FindByServerId(42u).has_value());
        REQUIRE(idx.FindByServerId(99u).value() == kChestEntity);
    }

    SECTION("BuildPlayerIdentityKey stays invariant across reconnect PlayerId shifts and case/whitespace variations")
    {
        const std::string k1 = CoopWorldView::BuildPlayerIdentityKey(0, "  Riyozaki  ", 1);
        const std::string k2 = CoopWorldView::BuildPlayerIdentityKey(0, "riyozaki", 7);
        REQUIRE(k1 == "name:riyozaki");
        REQUIRE(k1 == k2);

        const std::string d1 = CoopWorldView::BuildPlayerIdentityKey(123456789012345678ULL, "Alice", 1);
        const std::string d2 = CoopWorldView::BuildPlayerIdentityKey(123456789012345678ULL, "AliceRenamed", 9);
        REQUIRE(d1 == "discord:123456789012345678");
        REQUIRE(d1 == d2);

        REQUIRE(CoopWorldView::BuildPlayerIdentityKey(0, "   ", 42) == "pid:42");
    }

    SECTION("Phase 1 source invariants verify two-phase ownership, WaitingFor3D cache sync, and dormant server wake-up")
    {
        const auto root = LocateRepoRootForV203Audit();
        REQUIRE_FALSE(root.empty());

        const std::string waitAssignH = LoadV203RepoFile(root, "Code/client/Components/WaitingForAssignmentComponent.h");
        REQUIRE(waitAssignH.find("bool DirtyInventory{false}") != std::string::npos);
        REQUIRE(waitAssignH.find("float PendingHealthDelta{0.0f}") != std::string::npos);

        const std::string clientCharCpp = LoadV203RepoFile(root, "Code/client/Services/Generic/CharacterService.cpp");
        REQUIRE(clientCharCpp.find("PendingOwnershipEpoch") != std::string::npos);
        REQUIRE(clientCharCpp.find("SyncRemoteCachedInventory") != std::string::npos);
        REQUIRE(clientCharCpp.find("SyncRemoteCachedEquipment") != std::string::npos);
        REQUIRE(clientCharCpp.find("SyncRemoteCachedActorValues") != std::string::npos);
        REQUIRE(clientCharCpp.find("SyncRemoteCachedHealthDelta") != std::string::npos);
        REQUIRE(clientCharCpp.find("SyncRemoteCachedDeathState") != std::string::npos);
        REQUIRE(clientCharCpp.find("SyncRemoteCachedWeaponDraw") != std::string::npos);

        const std::string serverCharCpp = LoadV203RepoFile(root, "Code/server/Services/CharacterService.cpp");
        REQUIRE(serverCharCpp.find("FindPersistentCharacterByFormId") != std::string::npos);
        REQUIRE(serverCharCpp.find("WakeDormantCharactersInActiveRanges") != std::string::npos);
        REQUIRE(serverCharCpp.find("TiltedPhoques::kUnreliableNoNagle") != std::string::npos);

        const std::string serverInvCpp = LoadV203RepoFile(root, "Code/server/Services/InventoryService.cpp");
        REQUIRE(serverInvCpp.find("NotifyDrawWeapon notify{}") != std::string::npos);
    }
}

TEST_CASE("v2.0.3 Phase 2: Multi-hop SKSE->EngineFixes trampoline walker and Zero-CTD engine invariants", "[rfab][v203][phase2]")
{
    SECTION("DecodeHookJumpTargetLive walks 1-hop, 2-hop SKSE->EngineFixes RIP-indirect, 3-hop chains, and self-loop cycles")
    {
        // 1. Unhooked MSVC x64 prologue `mov [rsp+8], rbx` (`48 89 5C 24 08`) -> 0
        alignas(16) uint8_t unhooked[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x90, 0x90, 0x90};
        REQUIRE(CoopDamagePipeline::DecodeHookJumpTargetLive(unhooked) == 0);

        // 2. 2-hop chain: `FormAllocate` (`E9 rel32`) -> SKSE branch trampoline (`FF 25 00 00 00 00 <u64>`) -> `EngineFixes`
        alignas(16) uint8_t arena[128]{};
        uint8_t* pFormAllocate = arena + 0;
        uint8_t* pSkseTrampoline = arena + 32;
        uint8_t* pEngineFixesAlloc = arena + 96;
        // Put non-jump body at final `EngineFixes` function so hop-walk lands cleanly on `pEngineFixesAlloc`:
        pEngineFixesAlloc[0] = 0x48;
        pEngineFixesAlloc[1] = 0x83;
        pEngineFixesAlloc[2] = 0xEC;
        pEngineFixesAlloc[3] = 0x28;

        // `pFormAllocate`: `E9 rel32` -> `pSkseTrampoline`
        pFormAllocate[0] = 0xE9;
        const int32_t relToTramp = static_cast<int32_t>(pSkseTrampoline - (pFormAllocate + 5));
        std::memcpy(pFormAllocate + 1, &relToTramp, sizeof(relToTramp));

        // `pSkseTrampoline`: `FF 25 00 00 00 00` + 8-byte absolute target `pEngineFixesAlloc`
        pSkseTrampoline[0] = 0xFF;
        pSkseTrampoline[1] = 0x25;
        const int32_t ripDispZero = 0;
        std::memcpy(pSkseTrampoline + 2, &ripDispZero, sizeof(ripDispZero));
        const uint64_t finalAddr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pEngineFixesAlloc));
        std::memcpy(pSkseTrampoline + 6, &finalAddr, sizeof(finalAddr));

        REQUIRE(
            CoopDamagePipeline::DecodeHookJumpTargetLive(pFormAllocate) ==
            reinterpret_cast<uintptr_t>(pEngineFixesAlloc));

        // 3. Pathological `EB FE` infinite self-jump terminates deterministically without hanging:
        alignas(16) uint8_t selfLoop[8] = {0xEB, 0xFE, 0x90, 0x90};
        const uintptr_t loopRes = CoopDamagePipeline::DecodeHookJumpTargetLive(selfLoop);
        REQUIRE(loopRes == reinterpret_cast<uintptr_t>(selfLoop));
    }

    SECTION("Phase 2 ABI & Zero-CTD source invariants verify GameArray ABI, GamePtr<Actor> refcounting, and main-thread EquipManager")
    {
        const auto root = LocateRepoRootForV203Audit();
        REQUIRE_FALSE(root.empty());

        const std::string primH = LoadV203RepoFile(root, "Code/client/Games/Primitives.h");
        REQUIRE(primH.find("~GameArray() = default;") != std::string::npos);

        const std::string enchCpp = LoadV203RepoFile(root, "Code/client/Games/Skyrim/Forms/EnchantmentItem.cpp");
        REQUIRE(enchCpp.find("Memory::Free(effects.data)") != std::string::npos);

        const std::string rfabH = LoadV203RepoFile(root, "Code/client/Services/RfabAdapterService.h");
        REQUIRE(rfabH.find("[[nodiscard]] GamePtr<Actor> FindBestStandingPlayerTargetFor") != std::string::npos);

        const std::string equipCpp = LoadV203RepoFile(root, "Code/client/Games/Skyrim/EquipManager.cpp");
        REQUIRE(equipCpp.find("World::IsCreated() && World::IsConnectedCached()") != std::string::npos);

        const std::string refrCpp = LoadV203RepoFile(root, "Code/client/Games/Skyrim/TESObjectREFR.cpp");
        REQUIRE(refrCpp.find("pExtraEnchantment->pEnchantment->IsTemporary()") != std::string::npos);
        REQUIRE(refrCpp.find("!arEntry.EnchantData.Effects.empty()") != std::string::npos);
    }
}

TEST_CASE("v2.0.3 Phase 3: RFAB ScriptDamageAuthLedger, ConcentrationSyncLimiter, and ResurrectInPlace", "[rfab][v203][phase3]")
{
    SECTION("ScriptDamageAuthLedger authorizes Papyrus DamageActorValue after local player hits while blocking ambient peer bleed")
    {
        CoopDamagePipeline::ScriptDamageAuthLedger ledger;
        constexpr uint32_t kRemoteBossFormId = 0x0704BEEFu;
        constexpr uint32_t kUnrelatedBanditFormId = 0x00019999u;

        REQUIRE_FALSE(ledger.IsAuthorized(kRemoteBossFormId, 1000));

        ledger.ArmWindow(kRemoteBossFormId, /*aNowMs=*/1000, /*aDurationMs=*/3000);
        REQUIRE(ledger.IsAuthorized(kRemoteBossFormId, 1250));
        REQUIRE(ledger.IsAuthorized(kRemoteBossFormId, 3999));
        REQUIRE_FALSE(ledger.IsAuthorized(kUnrelatedBanditFormId, 1250));

        // Expires cleanly at/past `ExpiresAtMs` (`1000 + 3000 == 4000`):
        REQUIRE_FALSE(ledger.IsAuthorized(kRemoteBossFormId, 4000));
    }

    SECTION("ConcentrationSyncLimiter throttles 60Hz concentration ticks while passing new effects/targets immediately")
    {
        CoopDamagePipeline::ConcentrationSyncLimiter limiter;

        // First cast tick sends immediately:
        REQUIRE(limiter.ShouldAllowSend(/*caster=*/0x14u, /*target=*/500u, /*effect=*/0x12FD0u, /*nowMs=*/1000));
        // 16ms later (next 60 FPS frame): suppressed!
        REQUIRE_FALSE(limiter.ShouldAllowSend(0x14u, 500u, 0x12FD0u, 1016));
        REQUIRE_FALSE(limiter.ShouldAllowSend(0x14u, 500u, 0x12FD0u, 1179));
        // 180ms later (~5.5 Hz cadence): allowed!
        REQUIRE(limiter.ShouldAllowSend(0x14u, 500u, 0x12FD0u, 1180));
        // Switching spell/effect or target passes immediately without waiting on previous slot's cooldown:
        REQUIRE(limiter.ShouldAllowSend(0x14u, 500u, /*effect=*/0x23456u, 1190));
        // Clearing on disconnect/reset sends immediately:
        limiter.Clear();
        REQUIRE(limiter.ShouldAllowSend(0x14u, 500u, 0x12FD0u, 1200));
    }

    SECTION("Phase 3 source invariants verify ResurrectInPlace and clean boss combat continuity without heal-rollback ping-pong")
    {
        const auto root = LocateRepoRootForV203Audit();
        REQUIRE_FALSE(root.empty());

        const std::string actorCpp = LoadV203RepoFile(root, "Code/client/Games/Skyrim/Actor.cpp");
        REQUIRE(actorCpp.find("void Actor::ResurrectInPlace() noexcept") != std::string::npos);
        REQUIRE(actorCpp.find("s_scriptDamageAuthLedger") != std::string::npos);

        const std::string magicTargetCpp = LoadV203RepoFile(root, "Code/client/Games/Skyrim/Magic/MagicTarget.cpp");
        REQUIRE(magicTargetCpp.find("ArmLocalPlayerScriptDamageWindow") != std::string::npos);

        const std::string serverAvCpp = LoadV203RepoFile(root, "Code/server/Services/ActorValueService.cpp");
        REQUIRE(serverAvCpp.find("ShouldBlockFalseBossResetHeal") == std::string::npos);
    }
}

TEST_CASE("v2.0.3 Phase 4: Persistent ContainerLootLedger (>10 min anti-dupe + snapshot rollback) & checkpoint identity", "[rfab][v203][phase4]")
{
    SECTION("ContainerLootLedger blocks delayed second-player boss chest / OnDeath loot even 10+ minutes later and rolls back cleanly")
    {
        CoopLootLedger::ContainerLootLedger ledger;
        constexpr uint32_t kBossChestKey = 0x80123456u;
        constexpr uint32_t kRfabModId = 9u;
        constexpr uint32_t kArtifactId = 0x0022AA11u;

        // Player 1 loots 1x dynamic RFAB artifact at t = 10s:
        const auto p1 = ledger.EvaluateWithdrawal(kBossChestKey, kRfabModId, kArtifactId, /*player=*/1, /*avail=*/0, /*req=*/1, /*quest=*/false, /*nowMs=*/10'000ULL);
        REQUIRE(p1.GrantedTake == 1);
        REQUIRE(p1.OverdrawnRollback == 0);

        // Snapshot taken after Boss Room 1 (`Epoch = 1`):
        const auto room1Snapshot = ledger.CaptureSnapshot();

        // Player 2 opens Boss Room 1 chest 10 MINUTES later (`t = 610,000 ms`, well past old 15s window!):
        const auto p2Delayed = ledger.EvaluateWithdrawal(kBossChestKey, kRfabModId, kArtifactId, /*player=*/2, /*avail=*/0, /*req=*/1, /*quest=*/false, /*nowMs=*/610'000ULL);
        REQUIRE(p2Delayed.GrantedTake == 0);
        REQUIRE(p2Delayed.OverdrawnRollback == 1);

        // Players advance into Room 2 and loot Chest 2, then party-wipe back to `room1Snapshot`:
        constexpr uint32_t kRoom2ChestKey = 0x80998877u;
        constexpr uint32_t kRubyId = 0x00063B43u;
        const auto room2Take = ledger.EvaluateWithdrawal(kRoom2ChestKey, 0, kRubyId, 1, 0, 1, false, 650'000ULL);
        REQUIRE(room2Take.GrantedTake == 1);

        ledger.RestoreSnapshot(room1Snapshot);
        // After rollback to `Epoch = 1`:
        // - Boss Room 1 chest is STILL depleted (cannot be re-looted after wipe)!
        const auto bossAfterWipe = ledger.EvaluateWithdrawal(kBossChestKey, kRfabModId, kArtifactId, 2, 0, 1, false, 700'000ULL);
        REQUIRE(bossAfterWipe.GrantedTake == 0);
        REQUIRE(bossAfterWipe.OverdrawnRollback == 1);

        // - Room 2 chest (looted AFTER `Epoch = 1`) is cleanly lootable again!
        const auto room2AfterWipe = ledger.EvaluateWithdrawal(kRoom2ChestKey, 0, kRubyId, 2, 0, 1, false, 700'100ULL);
        REQUIRE(room2AfterWipe.GrantedTake == 1);
        REQUIRE(room2AfterWipe.OverdrawnRollback == 0);

        // Natural NPC respawn (`ForgetContainer`) resets only that corpse's loot slots:
        ledger.ForgetContainer(kRoom2ChestKey);
        const auto afterRespawn = ledger.EvaluateWithdrawal(kRoom2ChestKey, 0, kRubyId, 1, 0, 1, false, 800'000ULL);
        REQUIRE(afterRespawn.GrantedTake == 1);
        REQUIRE(afterRespawn.OverdrawnRollback == 0);
    }

    SECTION("Phase 4 source invariants verify kV203IdentityAndLedgerExtMagic, HydratePersistedWorldState, and client object rollback")
    {
        const auto root = LocateRepoRootForV203Audit();
        REQUIRE_FALSE(root.empty());

        const std::string saveSnapCpp = LoadV203RepoFile(root, "Code/server/Services/SaveSnapshotService.cpp");
        REQUIRE(saveSnapCpp.find("kV203IdentityAndLedgerExtMagic") != std::string::npos);
        REQUIRE(saveSnapCpp.find("HydratePersistedWorldState") != std::string::npos);
        REQUIRE(saveSnapCpp.find("PlayersByIdentityKey") != std::string::npos);
        REQUIRE(saveSnapCpp.find("RestoreLootLedgerSnapshot(acSnapshot.LootLedger)") != std::string::npos);

        const std::string coordSaveCpp = LoadV203RepoFile(root, "Code/client/Services/CoordinatedSaveService.cpp");
        REQUIRE(coordSaveCpp.find("RefreshCurrentCellObjectsAfterRollback()") != std::string::npos);
    }
}

TEST_CASE("v2.0.3 playtest hardening: level-sync must never demolish a live actor", "[rfab][v203][playtest]")
{
    const auto root = LocateRepoRootForV203Audit();
    REQUIRE_FALSE(root.empty());

    SECTION("Placed actor base-form reconciliation never detaches the reference from the world")
    {
        const std::string clientCharCpp = LoadV203RepoFile(root, "Code/client/Services/Generic/CharacterService.cpp");

        // Root cause found in `tp_client.log` (v2.0.3 playtest): 25 of 26 `[RFAB LeveledSync] Reconciling placed
        // actor` lines were followed within 1..104 log lines by `CancelServerAssignment` + `Actor removed` for the
        // SAME form id. `DisableImpl()` dropped the NiNode, `DiscoveryService` reported `ActorRemovedEvent`, the
        // client handed ownership back to the server, the NPC lost local AI ("ignored the player") and its
        // equipment was rebuilt from stale spawn data ("attacked with a non-primary weapon").
        const size_t reconcileStart = clientCharCpp.find("bool CharacterService::ReconcilePlacedActorBaseFormInPlace");
        REQUIRE(reconcileStart != std::string::npos);
        // NOTE: CI checks out with CRLF and the file is read in binary mode, so anchor on "\n}" (a closing brace
        // in column 0) instead of "\n}\n": indented braces never match, and CRLF keeps the leading "\n" intact.
        const size_t reconcileEnd = clientCharCpp.find("\n}", reconcileStart);
        REQUIRE(reconcileEnd != std::string::npos);
        const std::string body = clientCharCpp.substr(reconcileStart, reconcileEnd - reconcileStart);

        REQUIRE(body.find("apActor->DisableImpl()") == std::string::npos);
        REQUIRE(body.find("apActor->EnableImpl()") == std::string::npos);
        REQUIRE(body.find("IsInCombat()") != std::string::npos);
        // Ownership guard: ECS `LocalComponent`, not `ActorExtension::IsLocal()` (which is still true right before
        // the call sites flip the actor to remote and would silently disable level sync altogether).
        REQUIRE(body.find("m_world.all_of<LocalComponent>(ownerEntity)") != std::string::npos);
        // A reference whose 3D is already up must never be re-templated: `SetBaseForm` tears the NiNode down and
        // `DiscoveryService` reports the still-registered ref as `Actor removed` ~50-100 ms later.
        REQUIRE(body.find("if (apActor->GetNiNode())") != std::string::npos);
        REQUIRE(body.find("SetBaseForm") != std::string::npos);
        REQUIRE(body.find("QueueUpdate()") != std::string::npos);

        // The owner client keeps its RFAB leveled template instead of adopting the stale server cache.
        REQUIRE(clientCharCpp.find("keeping owner-authoritative base form") != std::string::npos);

        // ...and the wire format stays untouched: no `ActorData::BaseId`, so a v2.0.3 client and server (or any
        // build inside the same `v2.0.x` protocol family) keep interoperating without a handshake change.
        const std::string actorDataH = LoadV203RepoFile(root, "Code/encoding/Structs/ActorData.h");
        REQUIRE(actorDataH.find("GameId BaseId") == std::string::npos);
        const std::string actorDataCpp = LoadV203RepoFile(root, "Code/encoding/Structs/ActorData.cpp");
        REQUIRE(actorDataCpp.find("BaseId.Serialize") == std::string::npos);
        REQUIRE(clientCharCpp.find("actorData.BaseId") == std::string::npos);
    }

    SECTION("Relinquish control resolves the entity through the server-id index instead of scanning the registry")
    {
        const std::string clientCharCpp = LoadV203RepoFile(root, "Code/client/Services/Generic/CharacterService.cpp");
        const size_t fnStart = clientCharCpp.find("void CharacterService::OnNotifyRelinquishControl");
        REQUIRE(fnStart != std::string::npos);
        const size_t fnEnd = clientCharCpp.find("\n}", fnStart);
        REQUIRE(fnEnd != std::string::npos);
        const std::string body = clientCharCpp.substr(fnStart, fnEnd - fnStart);

        REQUIRE(body.find("FindEntityByServerId(acMessage.ServerId)") != std::string::npos);
        REQUIRE(body.find("spdlog::error(__FUNCTION__ \": failed to find server id") == std::string::npos);
        REQUIRE(body.find("for (auto entity : entities)") == std::string::npos);
    }

    SECTION("Naked-outfit repair never re-dresses a fighting or armed actor")
    {
        const std::string inventoryCpp = LoadV203RepoFile(root, "Code/client/Services/Generic/InventoryService.cpp");
        REQUIRE(inventoryCpp.find("holdsRealWeapon") != std::string::npos);
        REQUIRE(inventoryCpp.find("Cast<TESObjectWEAP>(pActor->GetEquippedWeapon(0))") != std::string::npos);
        REQUIRE(inventoryCpp.find("pActor->IsInCombat() || holdsRealWeapon") != std::string::npos);
    }

    SECTION("Downed/revive transitions are observable in the client log")
    {
        const std::string downedCpp = LoadV203RepoFile(root, "Code/client/Services/DownedStateService.cpp");
        REQUIRE(downedCpp.find("Local player entered downed state") != std::string::npos);
        REQUIRE(downedCpp.find("Local player revived") != std::string::npos);
        REQUIRE(downedCpp.find("Peer {} downed state -> {}") != std::string::npos);
    }
}
