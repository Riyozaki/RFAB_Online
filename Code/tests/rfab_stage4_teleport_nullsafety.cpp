#include <catch2/catch.hpp>

#include <CoopTeleportGuard.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace
{
std::filesystem::path FindRepoRootStage4()
{
    auto cur = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (std::filesystem::exists(cur / "Code" / "client" / "Games" / "Skyrim" / "TESObjectREFR.cpp"))
            return cur;
        if (!cur.has_parent_path())
            break;
        cur = cur.parent_path();
    }
    return {};
}

std::string ReadFileUtf8(const std::filesystem::path& aPath)
{
    std::ifstream in(aPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("Stage 4: LatchedDownedRespawnGuard suppresses spontaneous GetCOCPlacementInfo teleports", "[rfab][stage4][teleport]")
{
    using namespace CoopTeleportGuard;

    LatchedDownedRespawnGuard guard{};

    SECTION("Suppressed by default before connection and before ServerSettings arrive")
    {
        CHECK(guard.ShouldSuppressLegacyRespawnTeleport(1'000ULL));
        guard.NotifyConnected(2'000ULL);
        CHECK(guard.ShouldSuppressLegacyRespawnTeleport(2'100ULL));
    }

    SECTION("DownedSystemEnabled latches suppression across transient disconnects and reconnect windows")
    {
        guard.NotifyConnected(1'000ULL);
        guard.NotifyServerSettings(/*aDeathSystemEnabled=*/true, /*aDownedSystemEnabled=*/true);
        CHECK(guard.IsDownedLatched());
        CHECK(guard.ShouldSuppressLegacyRespawnTeleport(5'000ULL));

        // Transient network drop: must NEVER allow legacy RespawnPlayer() COC teleport!
        guard.NotifyDisconnected(6'000ULL);
        CHECK(guard.ShouldSuppressLegacyRespawnTeleport(6'050ULL));
        CHECK(guard.ShouldSuppressLegacyRespawnTeleport(30'000ULL));

        // Immediate auto-reconnect before NotifySettings arrives: stays latched!
        guard.NotifyConnected(31'000ULL);
        CHECK(guard.ShouldSuppressLegacyRespawnTeleport(31'050ULL));
    }

    SECTION("Legacy non-downed death system still suppresses offline and during 15s post-disconnect cooldown")
    {
        guard.NotifyConnected(1'000ULL);
        guard.NotifyServerSettings(/*aDeathSystemEnabled=*/true, /*aDownedSystemEnabled=*/false);
        CHECK_FALSE(guard.IsDownedLatched());
        CHECK_FALSE(guard.ShouldSuppressLegacyRespawnTeleport(2'000ULL));

        guard.NotifyDisconnected(10'000ULL);
        CHECK(guard.ShouldSuppressLegacyRespawnTeleport(10'500ULL));

        guard.NotifyConnected(12'000ULL);
        // Within 15s of disconnect at 10'000ms -> still suppressed!
        CHECK(guard.ShouldSuppressLegacyRespawnTeleport(24'999ULL));
        // After 15s cooldown expires -> allowed if server explicitly disabled DownedSystem
        CHECK_FALSE(guard.ShouldSuppressLegacyRespawnTeleport(25'500ULL));
    }
}

TEST_CASE("Stage 4: RemoteQuestMoveToGuard blocks cross-thread async Papyrus quest-fragment teleports", "[rfab][stage4][teleport]")
{
    using namespace CoopTeleportGuard;

    RemoteQuestMoveToGuard::ResetForTesting();
    REQUIRE_FALSE(RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(1'000'000ULL));

    constexpr uint64_t kT0 = 10'000'000'000ULL;
    constexpr uint64_t kHoldNs = RemoteQuestMoveToGuard::kDefaultAsyncPapyrusHoldNs;

    RemoteQuestMoveToGuard::EnterScope(kT0, kHoldNs);
    CHECK(RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(kT0));

    // Simulate Papyrus VM worker thread checking the guard concurrently
    bool blockedOnWorkerThread = false;
    std::thread vmWorker([&]() {
        blockedOnWorkerThread = RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(kT0 + 50'000'000ULL);
    });
    vmWorker.join();
    CHECK(blockedOnWorkerThread);

    // Nested remote quest stage update
    RemoteQuestMoveToGuard::EnterScope(kT0 + 100'000'000ULL, kHoldNs);
    RemoteQuestMoveToGuard::ExitScope(kT0 + 120'000'000ULL, kHoldNs);
    CHECK(RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(kT0 + 150'000'000ULL));

    // Outer scope exits at T0 + 200ms -> hold window remains active until T0 + 200ms + 750ms
    const uint64_t exitNs = kT0 + 200'000'000ULL;
    RemoteQuestMoveToGuard::ExitScope(exitNs, kHoldNs);
    CHECK(RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(exitNs + 749'000'000ULL));
    CHECK_FALSE(RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(exitNs + 751'000'000ULL));

    RemoteQuestMoveToGuard::ResetForTesting();
}

TEST_CASE("Stage 4: Structural null-safety & teleport-guard audit across client/server sources", "[rfab][stage4][audit]")
{
    const auto root = FindRepoRootStage4();
    if (root.empty())
        return;

    const auto tesRefr = ReadFileUtf8(root / "Code/client/Games/Skyrim/TESObjectREFR.cpp");
    CHECK(tesRefr.find("HookInternalMoveTo") != std::string::npos);
    CHECK(tesRefr.find("ShouldBlockUnauthorizedPlayerMoveTo") != std::string::npos);

    const auto questSvc = ReadFileUtf8(root / "Code/client/Services/Generic/QuestService.cpp");
    CHECK(questSvc.find("ScopedRemoteQuestMoveToSuppression") != std::string::npos);

    const auto playerSvc = ReadFileUtf8(root / "Code/client/Services/Generic/PlayerService.cpp");
    CHECK(playerSvc.find("ShouldSuppressLegacyRespawnTeleport") != std::string::npos);

    const auto playerChar = ReadFileUtf8(root / "Code/client/Games/Skyrim/PlayerCharacter.cpp");
    CHECK(playerChar.find("if (!pCell)") != std::string::npos);
    CHECK(playerChar.find("!pSkills || !pSkills->data") != std::string::npos);

    const auto equipMgr = ReadFileUtf8(root / "Code/client/Games/Skyrim/EquipManager.cpp");
    CHECK(equipMgr.find("if (!apActor)") != std::string::npos);
    CHECK(equipMgr.find("if (!pExtension)") != std::string::npos);

    const auto coordSave = ReadFileUtf8(root / "Code/client/Services/CoordinatedSaveService.cpp");
    CHECK(coordSave.find("ApplyInSessionRollback") != std::string::npos);
    CHECK(coordSave.find("m_preJoinBackupWrittenThisSession") != std::string::npos);
    CHECK(coordSave.find("CloseAllMenus") != std::string::npos);
}
