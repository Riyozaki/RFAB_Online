#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <entt/entt.hpp>
#include <Server.hpp>

#include <CoopCheckpointStore.h>
#include <Messages/CoordinatedSaveAck.h>
#include <Messages/NotifyCommitSave.h>
#include <Messages/NotifyExecuteLoad.h>
#include <Services/SaveSnapshotService.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
std::filesystem::path FindRepoRootPhase3()
{
    std::error_code ec;
    auto cur = std::filesystem::current_path(ec);
    for (int depth = 0; depth < 8 && !cur.empty(); ++depth)
    {
        if (std::filesystem::exists(cur / "Code" / "client" / "Services" / "CoordinatedSaveService.cpp", ec))
            return cur;
        cur = cur.parent_path();
    }
    return {};
}

std::string LoadPhase3File(const std::filesystem::path& acRoot, const char* acRelPath)
{
    std::ifstream in(acRoot / acRelPath, std::ios::binary);
    REQUIRE(in.is_open());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("v3.0 Phase 3: ReloadBarrierEpochBit multiplexes CoordinatedSaveAck & NotifyCommitSave cleanly", "[rfab][v300][phase3][wire]")
{
    using namespace CoopCheckpoint;

    for (const uint64_t rawEpoch : {0ULL, 1ULL, 7ULL, 42ULL, 999'999ULL})
    {
        CHECK_FALSE(IsReloadBarrierEpoch(rawEpoch));

        const uint64_t encoded = EncodeReloadBarrierEpoch(rawEpoch);
        CHECK(IsReloadBarrierEpoch(encoded));
        CHECK(DecodeReloadBarrierEpoch(encoded) == rawEpoch);
    }

    // Wire round-trip on CoordinatedSaveAck & NotifyCommitSave
    CoordinatedSaveAck ackOut{};
    ackOut.EpochId = EncodeReloadBarrierEpoch(19ULL);
    ackOut.SlotName = "RFAB_MP_S0001_Auto_2";
    ackOut.Success = true;

    uint8_t scratch[1024]{};
    TiltedPhoques::Buffer buf(scratch, sizeof(scratch));
    TiltedPhoques::Buffer::Writer writer(&buf);
    ackOut.SerializeRaw(writer);

    TiltedPhoques::Buffer::Reader reader(&buf);
    CoordinatedSaveAck ackIn{};
    ackIn.DeserializeRaw(reader);

    CHECK(IsReloadBarrierEpoch(ackIn.EpochId));
    CHECK(DecodeReloadBarrierEpoch(ackIn.EpochId) == 19ULL);
    CHECK(ackIn.SlotName == "RFAB_MP_S0001_Auto_2");
    CHECK(ackIn.Success);
}

TEST_CASE("v3.0 Phase 3: WorldReloadBarrier holds fast Guest NPC claims until Host completes .ess load", "[rfab][v300][phase3][barrier]")
{
    SaveSnapshotCoordinator coordinator("S3000", 3);

    SECTION("Guest SSD finishes before Host: Guest claims are held until Host reports ready")
    {
        coordinator.NotifyRolledBackTo(12ULL, "RFAB_MP_S3000_Manual_0", 10'000ULL);
        const auto barrier = coordinator.BeginReloadBarrier({1U, 2U}, /*aHostPlayerId=*/1U, 12ULL, "RFAB_MP_S3000_Manual_0", 10'000ULL);

        CHECK(coordinator.HasActiveReloadBarrier());
        CHECK(barrier.HostPlayerId == 1U);
        CHECK_FALSE(barrier.HostReady);
        CHECK_FALSE(barrier.HostGateReleased);

        // Host is never held; Guest IS held while Host is still loading .ess
        CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(1U));
        CHECK(coordinator.ShouldHoldNonHostWorldClaims(2U));

        // Guest (pid 2) finishes loading .ess first
        CHECK_FALSE(coordinator.RecordReloadPlayerReady(2U, 12ULL));
        CHECK(coordinator.HasActiveReloadBarrier());
        CHECK(coordinator.ShouldHoldNonHostWorldClaims(2U));

        // Host (pid 1) finishes loading .ess -> releases Host gate AND completes full barrier
        CHECK(coordinator.RecordReloadPlayerReady(1U, 12ULL));
        CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(2U));
        CHECK_FALSE(coordinator.HasActiveReloadBarrier());
    }

    SECTION("Host finishes before Guest: Host gate releases immediately and barrier closes when Guest finishes")
    {
        coordinator.NotifyRolledBackTo(13ULL, "RFAB_MP_S3000_Auto_1", 20'000ULL);
        (void)coordinator.BeginReloadBarrier({1U, 2U}, /*aHostPlayerId=*/1U, 13ULL, "RFAB_MP_S3000_Auto_1", 20'000ULL);

        CHECK(coordinator.ShouldHoldNonHostWorldClaims(2U));

        // Host finishes first -> immediately releases gate for Guest
        CHECK(coordinator.RecordReloadPlayerReady(1U, 13ULL));
        CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(2U));
        CHECK(coordinator.HasActiveReloadBarrier());

        // Guest finishes second -> barrier closes cleanly
        CHECK_FALSE(coordinator.RecordReloadPlayerReady(2U, 13ULL));
        CHECK_FALSE(coordinator.HasActiveReloadBarrier());
    }

    SECTION("Host loading stall unblocks Guests at 15s hold timeout and clears barrier at 45s")
    {
        coordinator.NotifyRolledBackTo(14ULL, "RFAB_MP_S3000_Manual_1", 50'000ULL);
        (void)coordinator.BeginReloadBarrier({1U, 2U}, /*aHostPlayerId=*/1U, 14ULL, "RFAB_MP_S3000_Manual_1", 50'000ULL);

        (void)coordinator.RecordReloadPlayerReady(2U, 14ULL);
        CHECK(coordinator.ShouldHoldNonHostWorldClaims(2U));

        // At +10s: still within Host hold window
        CHECK_FALSE(coordinator.TickReloadBarrier(60'000ULL));
        CHECK(coordinator.ShouldHoldNonHostWorldClaims(2U));

        // At +15s: Host hold expires -> releases Guest gate!
        CHECK(coordinator.TickReloadBarrier(65'000ULL));
        CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(2U));
        CHECK(coordinator.HasActiveReloadBarrier());

        // At +45s: full reload barrier expires cleanly
        CHECK_FALSE(coordinator.TickReloadBarrier(95'000ULL));
        CHECK_FALSE(coordinator.HasActiveReloadBarrier());
    }

    SECTION("Mid-reload Host disconnect immediately releases Guest world-claim gate")
    {
        coordinator.NotifyRolledBackTo(15ULL, "RFAB_MP_S3000_Auto_2", 70'000ULL);
        (void)coordinator.BeginReloadBarrier({1U, 2U}, /*aHostPlayerId=*/1U, 15ULL, "RFAB_MP_S3000_Auto_2", 70'000ULL);

        CHECK(coordinator.ShouldHoldNonHostWorldClaims(2U));
        CHECK(coordinator.HandlePlayerDisconnectedFromReload(1U));
        CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(2U));
        CHECK(coordinator.HasActiveReloadBarrier());

        CHECK_FALSE(coordinator.RecordReloadPlayerReady(2U, 15ULL));
        CHECK_FALSE(coordinator.HasActiveReloadBarrier());
    }
}

TEST_CASE("v3.0 Phase 3: Source-level invariants enforce live-socket .ess reload, zero SetActorInventory, and 0x14 entity reuse", "[rfab][v300][phase3][audit]")
{
    const auto root = FindRepoRootPhase3();
    REQUIRE_FALSE(root.empty());

    const std::string coordSaveCpp = LoadPhase3File(root, "Code/client/Services/CoordinatedSaveService.cpp");
    const std::string discoveryCpp = LoadPhase3File(root, "Code/client/Services/Generic/DiscoveryService.cpp");
    const std::string playerSvcCpp = LoadPhase3File(root, "Code/client/Services/Generic/PlayerService.cpp");
    const std::string transportCpp = LoadPhase3File(root, "Code/client/Services/Generic/TransportService.cpp");
    const std::string gameServerCpp = LoadPhase3File(root, "Code/server/GameServer.cpp");
    const std::string serverCharCpp = LoadPhase3File(root, "Code/server/Services/CharacterService.cpp");
    const std::string saveSnapCpp = LoadPhase3File(root, "Code/server/Services/SaveSnapshotService.cpp");

    // 1. Live-socket reload keeps m_transport open on m_liveSessionWorldReloadPending and restores MP globals after load
    CHECK(coordSaveCpp.find("m_liveSessionWorldReloadPending") != std::string::npos);
    CHECK(coordSaveCpp.find("m_liveSessionWorldReloadInFlight") != std::string::npos);
    CHECK(coordSaveCpp.find("CompleteLiveWorldReloadAfterEngineReady") != std::string::npos);
    CHECK(coordSaveCpp.find("RestoreMultiplayerGlobalsAfterWorldReload()") != std::string::npos);
    CHECK(coordSaveCpp.find("m_world.GetObjectService().ResetForWorldReload()") != std::string::npos);
    CHECK(coordSaveCpp.find("m_world.GetMagicService().ResetForWorldReload()") != std::string::npos);
    CHECK(coordSaveCpp.find("m_world.GetActorValueService().ResetForWorldReload()") != std::string::npos);

    // 2. NEVER call destructive `pPlayer->SetActorInventory(...)` on 0x14 (protects SkyUI hotkeys & RFAB.dll ExtraData!)
    CHECK(coordSaveCpp.find("SetActorInventory(") == std::string::npos);

    // 3. DiscoveryService gates Guest NPC VisitForms() behind IsHoldingWorldDiscoveryForHostBarrier()
    CHECK(discoveryCpp.find("IsHoldingWorldDiscoveryForHostBarrier()") != std::string::npos);
    CHECK(playerSvcCpp.find("void PlayerService::RestoreMultiplayerGlobalsAfterWorldReload()") != std::string::npos);

    // 4. Both Client TransportService and Server GameServer configure 60s GNS connected timeout & 4MB send buffer
    CHECK(transportCpp.find("k_ESteamNetworkingConfig_TimeoutConnected") != std::string::npos);
    CHECK(transportCpp.find("k_ESteamNetworkingConfig_SendBufferSize") != std::string::npos);
    CHECK(gameServerCpp.find("k_ESteamNetworkingConfig_TimeoutConnected") != std::string::npos);
    CHECK(gameServerCpp.find("k_ESteamNetworkingConfig_SendBufferSize") != std::string::npos);

    // 5. Server CharacterService reuses existing 0x14 player entity on live reload, purges post-checkpoint entities,
    //    and enforces ShouldHoldNonHostWorldClaims across all dormant-wake & claim paths
    CHECK(serverCharCpp.find("[RFAB WorldReloadBarrier] Reused player") != std::string::npos);
    CHECK(serverCharCpp.find("void CharacterService::ResetWorldEntitiesForCheckpointReload") != std::string::npos);
    CHECK(serverCharCpp.find("ShouldHoldNonHostWorldClaims") != std::string::npos);

    // 6. SaveSnapshotService arms BeginWorldReloadBarrier on both committed snapshots and pre-checkpoint PreJoin reloads,
    //    and delivers post-reload globals via SyncPostReloadStateToPlayer AFTER client .ess load finishes
    CHECK(saveSnapCpp.find("BeginWorldReloadBarrier(") != std::string::npos);
    CHECK(saveSnapCpp.find("SyncPostReloadStateToPlayer(") != std::string::npos);
    CHECK(saveSnapCpp.find("ReleaseHostReloadGateToGuests(") != std::string::npos);
}
