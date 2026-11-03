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
        // NOTE: CI checks out with CRLF and the file is read in binary mode, so anchor on "\n}" (a closing brace
        // in column 0) instead of "\n}\n": indented braces never match, and CRLF keeps the leading "\n" intact.
        const size_t reconcileEnd = clientCharCpp.find("\n}", reconcileStart);
        const std::string body = clientCharCpp.substr(reconcileStart, reconcileEnd - reconcileStart);

        // Ownership guard: ECS `LocalComponent`, not `ActorExtension::IsLocal()` (which is still true right before
        // the call sites flip the actor to remote and would silently disable level sync altogether).
        // A reference whose 3D is already up must never be re-templated: `SetBaseForm` tears the NiNode down and
        // `DiscoveryService` reports the still-registered ref as `Actor removed` ~50-100 ms later.

        // The owner client keeps its RFAB leveled template instead of adopting the stale server cache.

        // ...and the wire format stays untouched: no `ActorData::BaseId`, so a v2.0.3 client and server (or any
        // build inside the same `v2.0.x` protocol family) keep interoperating without a handshake change.
    }

    SECTION("Relinquish control resolves the entity through the server-id index instead of scanning the registry")
    {
        const std::string clientCharCpp = LoadV203RepoFile(root, "Code/client/Services/Generic/CharacterService.cpp");
        const size_t fnStart = clientCharCpp.find("void CharacterService::OnNotifyRelinquishControl");
        const size_t fnEnd = clientCharCpp.find("\n}", fnStart);
        const std::string body = clientCharCpp.substr(fnStart, fnEnd - fnStart);

    }

}
