#include <TiltedCore/Stl.hpp>

#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <common/CoopWorldStabilizer.h>

namespace
{
std::filesystem::path FindV314RepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "CoopWorldStabilizer.h") &&
            fs::exists(cur / "Code" / "client" / "SkyrimVM64.cpp"))
        {
            return cur;
        }
        if (!cur.has_parent_path())
            break;
        fs::path parent = cur.parent_path();
        if (parent == cur)
            break;
        cur = parent;
    }
    return {};
}

std::string LoadV314RepoFile(const std::filesystem::path& aRoot, const char* apRelPath)
{
    std::ifstream in(aRoot / apRelPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("v3.1.4 Unloading-cell graph isolation & exterior border relinquish-claim cooldown", "[rfab][v314][stabilizer]")
{
    // 1. CanTouchActorGraphDuringDetach blocks SendAnimationEvent / EvaluatePackage on unloading or 3D-less actors
    CHECK(CoopWorldStabilizer::CanTouchActorGraphDuringDetach(true, true, false, false, false));
    CHECK_FALSE(CoopWorldStabilizer::CanTouchActorGraphDuringDetach(false, true, false, false, false)); // cell detached/unloaded
    CHECK_FALSE(CoopWorldStabilizer::CanTouchActorGraphDuringDetach(true, false, false, false, false)); // GetNiNode() == nullptr
    CHECK_FALSE(CoopWorldStabilizer::CanTouchActorGraphDuringDetach(true, true, true, false, false));   // IsStillLoading()
    CHECK_FALSE(CoopWorldStabilizer::CanTouchActorGraphDuringDetach(true, true, false, true, false));   // IsDisabled()
    CHECK_FALSE(CoopWorldStabilizer::CanTouchActorGraphDuringDetach(true, true, false, false, true));   // IsDeleted()

    // 2. RelinquishClaimCooldownTracker stops 100ms Wolf (DC568) / Rabbit (105A3C) 5x5 seam ping-pong
    CoopWorldStabilizer::RelinquishClaimCooldownTracker tracker{};
    constexpr uint32_t kWolfFormId = 0x000DC568u;
    constexpr uint64_t kT0 = 50'000ULL;

    CHECK_FALSE(tracker.IsInCooldown(kWolfFormId, kT0));
    tracker.RecordRelinquished(kWolfFormId, kT0);

    // 90ms and 1500ms later: still suppressed so leader does not immediately re-steal across a detached cell seam
    CHECK(tracker.IsInCooldown(kWolfFormId, kT0 + 90ULL));
    CHECK(tracker.IsInCooldown(kWolfFormId, kT0 + 1500ULL));

    // Local player (0x14) and null FormIDs are never placed in cooldown
    tracker.RecordRelinquished(0x14u, kT0);
    CHECK_FALSE(tracker.IsInCooldown(0x14u, kT0 + 10ULL));

    // Once the 3000ms window elapses, normal re-entry into the 5x5 margin can claim ownership again
    CHECK_FALSE(tracker.IsInCooldown(kWolfFormId, kT0 + CoopWorldStabilizer::RelinquishClaimCooldownTracker::kDefaultCooldownMs));
}

TEST_CASE("v3.1.4 Single-thread WinMain frame execution, worker-hook safety & v3.1.3 backup contracts", "[rfab][v314][contracts]")
{
    const auto root = FindV314RepoRoot();
    REQUIRE_FALSE(root.empty());

    const std::string buildPs1 = LoadV314RepoFile(root, "Tools/rfab/Build-CloudRelease.ps1");
    const std::string pkgPs1 = LoadV314RepoFile(root, "Tools/rfab/Package-Release.ps1");
    REQUIRE(buildPs1.size() > 3);
    REQUIRE(pkgPs1.size() > 3);
    CHECK(static_cast<unsigned char>(buildPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(buildPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(buildPs1[2]) == 0xBF);
    CHECK(static_cast<unsigned char>(pkgPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(pkgPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(pkgPs1[2]) == 0xBF);
    CHECK(buildPs1.find("[string]$VersionTag = \"v3.1.6\"") != std::string::npos);
    CHECK(pkgPs1.find("[string]$VersionTag = \"v3.1.6\"") != std::string::npos);
    CHECK(buildPs1.find("Ensure-V313BackupRelease") != std::string::npos);
    CHECK(buildPs1.find("\"v3.1.3-backup\"") != std::string::npos);

    const std::string v313Manifest = LoadV314RepoFile(root, "docs/V3.1.3_RELEASE_BACKUP_MANIFEST.json");
    CHECK(v313Manifest.find("\"v3.1.3-backup\"") != std::string::npos);
    CHECK(v313Manifest.find("3e0510af3008f1dd66822dc81659efba90ce6dc43121ab51ce5f7b2fa9a7b0e8") != std::string::npos);

    // 1. HookVMUpdate (BSJobs worker) NEVER invokes g_appInstance->Update(); HookMainLoop (WinMain) owns World::Update()
    const std::string skyrimVmCpp = LoadV314RepoFile(root, "Code/client/SkyrimVM64.cpp");
    const auto vmHookPos = skyrimVmCpp.find("int TP_MAKE_THISCALL(HookVMUpdate");
    const auto mainHookPos = skyrimVmCpp.find("short TP_MAKE_THISCALL(HookMainLoop");
    const auto dtorHookPos = skyrimVmCpp.find("uintptr_t TP_MAKE_THISCALL(HookVMDestructor");
    REQUIRE(vmHookPos != std::string::npos);
    REQUIRE(mainHookPos != std::string::npos);
    REQUIRE(dtorHookPos != std::string::npos);
    const std::string vmHookBody = skyrimVmCpp.substr(vmHookPos, mainHookPos - vmHookPos);
    const std::string mainHookBody = skyrimVmCpp.substr(mainHookPos, dtorHookPos - mainHookPos);
    CHECK(vmHookBody.find("g_appInstance->Update()") == std::string::npos);
    CHECK(vmHookBody.find("s_vmActiveSinceLastMainLoop.store") != std::string::npos);
    CHECK(mainHookBody.find("g_appInstance->Update()") != std::string::npos);
    CHECK(mainHookBody.find("!apThis->freezeTime") != std::string::npos);

    // 2. Unified TESObjectCELL::IsAttachedAndLoaded() & unloading-actor graph isolation
    const std::string cellH = LoadV314RepoFile(root, "Code/client/Games/Skyrim/Forms/TESObjectCELL.h");
    CHECK(cellH.find("IsAttachedAndLoaded()") != std::string::npos);

    const std::string charSvcCpp = LoadV314RepoFile(root, "Code/client/Services/Generic/CharacterService.cpp");
    CHECK(charSvcCpp.find("IsActorLiveInAttachedCellWith3D") != std::string::npos);
    CHECK(charSvcCpp.find("m_relinquishClaimCooldowns.RecordRelinquished") != std::string::npos);

    const std::string assignSubCpp =
        LoadV314RepoFile(root, "Code/client/Services/Generic/CharacterMovementAndAssignmentSubservice.cpp");
    CHECK(assignSubCpp.find("m_relinquishClaimCooldowns.IsInCooldown") != std::string::npos);
    CHECK(assignSubCpp.find("IsInsideExteriorEntryMargin") != std::string::npos);

    // 3. Worker-hook atomic safety & main-thread marshaling
    const std::string magicTargetCpp = LoadV314RepoFile(root, "Code/client/Games/Skyrim/Magic/MagicTarget.cpp");
    CHECK(magicTargetCpp.find("AtomicOrActorFlags1") != std::string::npos);
    CHECK(magicTargetCpp.find("s_scopedTeammateRefCounts") != std::string::npos);
    CHECK(magicTargetCpp.find("GetServerSettings().PvpEnabled") == std::string::npos);

    const std::string downedCpp = LoadV314RepoFile(root, "Code/client/Services/DownedStateService.cpp");
    CHECK(downedCpp.find("if (!World::IsMainThread())") != std::string::npos);

    const std::string rfabAdapterCpp = LoadV314RepoFile(root, "Code/client/Services/RfabAdapterService.cpp");
    CHECK(rfabAdapterCpp.find("if (!World::IsMainThread())") != std::string::npos);

    // 4. Crash logger deduplication, TiltedTrampoline attribution, Launcher MO2 exe root & NPC ammo coalescing
    const std::string mainCpp = LoadV314RepoFile(root, "Code/client/main.cpp");
    CHECK(mainCpp.find("TiltedTrampoline") != std::string::npos);
    CHECK(mainCpp.find("s_crashDedupMutex") != std::string::npos);

    const std::string launcherCpp = LoadV314RepoFile(root, "Code/immersive_launcher/Launcher.cpp");
    CHECK(launcherCpp.find("mo2FromExe") != std::string::npos);

    const std::string invSvcCpp = LoadV314RepoFile(root, "Code/client/Services/Generic/InventoryService.cpp");
    CHECK(invSvcCpp.find("FlushPendingNpcAmmoDeltas") != std::string::npos);
}
