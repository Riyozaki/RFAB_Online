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

