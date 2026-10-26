#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <entt/entt.hpp>
#include <Server.hpp>

#include <CoopCheckpointStore.h>
#include <Messages/NotifyExecuteLoad.h>
#include <Services/SaveSnapshotService.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

TEST_CASE("Stage 3: Atomic RFABCKP2 checkpoint persistence & corruption detection", "[rfab][stage3][checkpoint]")
{
    using namespace CoopCheckpoint;

    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / std::filesystem::path(u8"rfab_stage3_Лисица_ckpt_test");
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    const std::filesystem::path ckptPath = tempRoot / "Data" / "rfab_checkpoints.bin";

    std::vector<uint8_t> samplePayload = {0x10, 0x20, 0x30, 0x40, 0x55, 0xAA, 0xFE, 0x01, 0x99};

    SECTION("Atomic save and load round-trips header metadata and payload on Unicode paths")
    {
        REQUIRE(WriteAtomicCheckpointFile(ckptPath, samplePayload.data(), samplePayload.size()));
        REQUIRE(std::filesystem::exists(ckptPath, ec));

        std::filesystem::path tmpFile = ckptPath;
        tmpFile += ".tmp";
        REQUIRE_FALSE(std::filesystem::exists(tmpFile, ec));

        std::vector<uint8_t> loaded{};
        REQUIRE(ReadValidatedCheckpointFile(ckptPath, loaded));
        CHECK(loaded == samplePayload);
    }

    SECTION("Corrupted payload byte is rejected by 64-bit FNV-1a checksum")
    {
        REQUIRE(WriteAtomicCheckpointFile(ckptPath, samplePayload.data(), samplePayload.size()));

        {
            std::fstream fs(ckptPath, std::ios::in | std::ios::out | std::ios::binary);
            REQUIRE(fs.is_open());
            fs.seekp(static_cast<std::streamoff>(sizeof(EnvelopeHeader) + 2));
            const char corrupted = static_cast<char>(0xFF);
            fs.write(&corrupted, 1);
            fs.flush();
        }

        std::vector<uint8_t> loaded{};
        REQUIRE_FALSE(ReadValidatedCheckpointFile(ckptPath, loaded));
        CHECK(loaded.empty());
    }

    SECTION("Truncated file and invalid magic header are rejected safely")
    {
        std::filesystem::create_directories(ckptPath.parent_path(), ec);
        {
            std::ofstream out(ckptPath, std::ios::binary | std::ios::trunc);
            const char badBytes[8] = {'B', 'A', 'D', 'M', 'A', 'G', 'I', 'C'};
            out.write(badBytes, sizeof(badBytes));
        }

        std::vector<uint8_t> loaded{};
        REQUIRE_FALSE(ReadValidatedCheckpointFile(ckptPath, loaded));
        CHECK(loaded.empty());
    }

    std::filesystem::remove_all(tempRoot, ec);
}

TEST_CASE("Stage 3: Seamless in-session rollback policy prevents stale PreJoin .ess reloads", "[rfab][stage3][rollback]")
{
    using namespace CoopCheckpoint;

    // Party wipes with player state on server ALWAYS prefer seamless in-session rollback (zero .ess reload!)
    CHECK(ShouldServerPreferInSessionRollback(true, true, "RFAB_MP_Epoch_0005"));
    CHECK(ShouldServerPreferInSessionRollback(true, true, ""));
    CHECK(ShouldServerPreferInSessionRollback(false, true, ""));

    // Explicit manual slot load with valid non-empty slot name allows coordinated .ess load if desired
    CHECK_FALSE(ShouldServerPreferInSessionRollback(false, true, "RFAB_MP_Epoch_0005"));
    CHECK_FALSE(ShouldServerPreferInSessionRollback(true, false, "RFAB_MP_Epoch_0005"));

    // Client NEVER falls back to RFAB_MP_PreJoin_Backup.ess when forbidden, from a previous session, or when server sent transform!
    CHECK_FALSE(ShouldAllowPreJoinDiskFallback(false, true, false));
    CHECK_FALSE(ShouldAllowPreJoinDiskFallback(true, false, false));
    CHECK_FALSE(ShouldAllowPreJoinDiskFallback(true, true, true));

    // Only explicit non-wipe disk fallback when backup was written in THIS session and no server transform exists
    CHECK(ShouldAllowPreJoinDiskFallback(true, true, false));
}

TEST_CASE("Stage 3: NotifyExecuteLoad serializes seamless in-session transform, inventory & health", "[rfab][stage3][wire]")
{
    NotifyExecuteLoad outgoing{};
    outgoing.EpochId = 17ULL;
    outgoing.SlotName = "RFAB_MP_S0001_Manual_1";
    outgoing.IsPartyWipe = true;
    outgoing.AllowPreJoinFallback = false;
    outgoing.Message = "Группа пала в бою — возврат к чекпоинту";
    outgoing.UseInSessionRollback = true;
    outgoing.HasRestoreTransform = true;
    outgoing.TargetCellId = GameId(0x00, 0x000165A8);
    outgoing.TargetWorldSpaceId = GameId(0x00, 0x0000003C);
    outgoing.PositionX = 18452.5f;
    outgoing.PositionY = -7421.25f;
    outgoing.PositionZ = 3120.75f;
    outgoing.RotationZ = 1.57079f;
    outgoing.HasRestoreInventory = true;
    outgoing.RestoreHealthFraction = 1.0f;

    Inventory::Entry potion{};
    potion.BaseId = GameId(0x06, 0x0003EAE3);
    potion.Count = 5;
    outgoing.RestoreInventory.Entries.push_back(potion);

    uint8_t scratch[4096]{};
    TiltedPhoques::Buffer buffer(scratch, sizeof(scratch));
    TiltedPhoques::Buffer::Writer writer(&buffer);
    outgoing.SerializeRaw(writer);

    TiltedPhoques::Buffer::Reader reader(&buffer);
    NotifyExecuteLoad incoming{};
    incoming.DeserializeRaw(reader);

    CHECK(incoming == outgoing);
    CHECK(incoming.EpochId == 17ULL);
    CHECK(incoming.SlotName == "RFAB_MP_S0001_Manual_1");
    CHECK(incoming.IsPartyWipe);
    CHECK_FALSE(incoming.AllowPreJoinFallback);
    CHECK(incoming.UseInSessionRollback);
    CHECK(incoming.HasRestoreTransform);
    CHECK(incoming.TargetCellId == GameId(0x00, 0x000165A8));
    CHECK(incoming.TargetWorldSpaceId == GameId(0x00, 0x0000003C));
    CHECK(incoming.PositionX == Approx(18452.5f));
    CHECK(incoming.PositionY == Approx(-7421.25f));
    CHECK(incoming.PositionZ == Approx(3120.75f));
    CHECK(incoming.RotationZ == Approx(1.57079f));
    CHECK(incoming.HasRestoreInventory);
    REQUIRE(incoming.RestoreInventory.Entries.size() == 1);
    CHECK(incoming.RestoreInventory.Entries[0].BaseId == GameId(0x06, 0x0003EAE3));
    CHECK(incoming.RestoreInventory.Entries[0].Count == 5);
    CHECK(incoming.RestoreHealthFraction == Approx(1.0f));
}

TEST_CASE("Stage 3: SaveSnapshotCoordinator restores persisted epoch counter monotonically", "[rfab][stage3][coordinator]")
{
    SaveSnapshotCoordinator coordinator{};
    coordinator.RestorePersistedState("S7777", 24ULL, 5ULL, 1U, 2U, 24ULL, "RFAB_MP_S7777_Manual_0");

    CHECK(coordinator.GetSessionTag() == "S7777");
    CHECK(coordinator.GetLastCommittedEpoch() == 24ULL);
    CHECK(coordinator.GetLastCommittedSlot() == "RFAB_MP_S7777_Manual_0");
    CHECK(coordinator.GetCurrentWorldEpoch() == 5ULL);

    const auto barrier = coordinator.BeginBarrier({1U, 2U}, false, 10'000ULL);
    REQUIRE(barrier.has_value());
    CHECK(barrier->EpochId == 25ULL);
    CHECK(barrier->SlotName == "RFAB_MP_S7777_Manual_1");
}
