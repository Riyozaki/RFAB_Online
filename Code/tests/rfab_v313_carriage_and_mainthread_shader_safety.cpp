#include <TiltedCore/Stl.hpp>

#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <common/CoopHairColorSync.h>
#include <common/CoopTeleportGuard.h>

namespace
{
std::filesystem::path FindV313RepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "CoopTeleportGuard.h") &&
            fs::exists(cur / "Code" / "client" / "Systems" / "FaceGenSystem.cpp"))
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

std::string LoadV313RepoFile(const std::filesystem::path& aRoot, const char* apRelPath)
{
    std::ifstream in(aRoot / apRelPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("v3.1.3 Carriage travel grace protects delayed CarriageSystemScript MoveTo from peer async holds", "[rfab][v313][carriage]")
{
    using CoopTeleportGuard::RemoteQuestMoveToGuard;
    RemoteQuestMoveToGuard::ResetForTesting();

    constexpr uint64_t kT0 = 10'000'000'000ULL;

    // 1. Player boards carriage bench at t = 10.0s (CarriageSystemScript fades out for ~2.5s before FastTravel/MoveTo)
    RemoteQuestMoveToGuard::NoteLocalDirectPlayerTravelInteraction(kT0);
    CHECK_FALSE(RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(kT0 + 100'000'000ULL));

    // 2. At t = 11.0s (mid-carriage fade-out!), a co-op peer activates a chest/lever or advances a quest stage
    {
        CoopTeleportGuard::ScopedRemoteQuestMoveToSuppression peerAsyncScope(
            kT0 + 1'000'000'000ULL,
            RemoteQuestMoveToGuard::kExtendedLateSessionPapyrusHoldNs);
        // Synchronous in-scope peer SetStage/Activate MoveTo is STILL blocked while activeDepth > 0
        CHECK(RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(kT0 + 1'010'000'000ULL));
    }
    RemoteQuestMoveToGuard::ExitScope(
        kT0 + 1'020'000'000ULL,
        RemoteQuestMoveToGuard::kExtendedLateSessionPapyrusHoldNs);

    // 3. At t = 12.6s, local player's CarriageSystemScript finishes fade-out and executes MoveTo -> MUST SUCCEED!
    CHECK_FALSE(RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(kT0 + 2'600'000'000ULL));

    // 4. After the 8.0s carriage grace expires (t = 19.0s), any new remote quest async hold blocks unauthorized MoveTo again
    RemoteQuestMoveToGuard::EnterScope(kT0 + 9'000'000'000ULL, RemoteQuestMoveToGuard::kExtendedLateSessionPapyrusHoldNs);
    RemoteQuestMoveToGuard::ExitScope(kT0 + 9'010'000'000ULL, RemoteQuestMoveToGuard::kExtendedLateSessionPapyrusHoldNs);
    CHECK(RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(kT0 + 10'000'000'000ULL));

    RemoteQuestMoveToGuard::ResetForTesting();
}

TEST_CASE("v3.1.3 Main-thread shader isolation & carriage crash-fix architectural contracts", "[rfab][v313][contracts]")
{
    const auto root = FindV313RepoRoot();
    REQUIRE_FALSE(root.empty());

    const std::string buildPs1 = LoadV313RepoFile(root, "Tools/rfab/Build-CloudRelease.ps1");
    const std::string pkgPs1 = LoadV313RepoFile(root, "Tools/rfab/Package-Release.ps1");
    REQUIRE(buildPs1.size() > 3);
    REQUIRE(pkgPs1.size() > 3);
    CHECK(static_cast<unsigned char>(buildPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(buildPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(buildPs1[2]) == 0xBF);
    CHECK(static_cast<unsigned char>(pkgPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(pkgPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(pkgPs1[2]) == 0xBF);
    CHECK(buildPs1.find("Resolve-ReleaseVersionTag") != std::string::npos);
    CHECK(pkgPs1.find("Resolve-ReleaseVersionTag") != std::string::npos);
    CHECK(buildPs1.find("Ensure-V312BackupRelease") != std::string::npos);
    CHECK(buildPs1.find("\"v3.1.2-backup\"") != std::string::npos);

    const std::string v312Manifest = LoadV313RepoFile(root, "docs/V3.1.2_RELEASE_BACKUP_MANIFEST.json");
    CHECK(v312Manifest.find("\"v3.1.2-backup\"") != std::string::npos);
    CHECK(v312Manifest.find("52e4ed5edf47750fea06144a0c274466acbe82bafa40b1b9aa35c6a739190e31") != std::string::npos);

    // 1. SkyrimVM64 HookMainLoop (SE 35565) drives World::OnMainThreadUpdate on real WinMain thread;
    //    World::Update (HookVMUpdate SE 53115 BSJobs worker pool) must NEVER overwrite s_mainThreadId!
    const std::string skyrimVmCpp = LoadV313RepoFile(root, "Code/client/SkyrimVM64.cpp");
    CHECK(skyrimVmCpp.find("World::Get().OnMainThreadUpdate()") != std::string::npos);

    const std::string worldCpp = LoadV313RepoFile(root, "Code/client/World.cpp");
    CHECK(worldCpp.find("void World::OnMainThreadUpdate() noexcept") != std::string::npos);
    CHECK(worldCpp.find("void World::QueueMainThreadTask(") != std::string::npos);
    const auto updatePos = worldCpp.find("void World::Update() noexcept");
    const auto runnerPos = worldCpp.find("RunnerService& World::GetRunner()");
    REQUIRE(updatePos != std::string::npos);
    REQUIRE(runnerPos != std::string::npos);
    const std::string worldUpdateBody = worldCpp.substr(updatePos, runnerPos - updatePos);
    CHECK(worldUpdateBody.find("s_mainThreadId.store") == std::string::npos);

    // 2. FaceGenSystem never touches shaders off WinMain, while offline, or during Fader/Loading transitions,
    //    and restores 0x14 hair tint RGB in-place via 3rd-person GetNiNode3rdPerson(false) without SetShaderMaterial!
    const std::string faceGenCpp = LoadV313RepoFile(root, "Code/client/Systems/FaceGenSystem.cpp");
    CHECK(faceGenCpp.find("IsSafeMainThreadSceneGraphWindow()") != std::string::npos);
    CHECK(faceGenCpp.find("Fader Menu") != std::string::npos);
    CHECK(faceGenCpp.find("GetNiNode3rdPerson(false)") != std::string::npos);
    CHECK(faceGenCpp.find("RestoreLocalHairTintDriftRecursive") != std::string::npos);
    CHECK(faceGenCpp.find("ExecuteMainThreadPass") != std::string::npos);

    // 3. CharacterService schedules FaceGen onto WinMain via QueueMainThreadTask and never calls it on BSJobs workers
    const std::string charSvcCpp = LoadV313RepoFile(root, "Code/client/Services/Generic/CharacterService.cpp");
    CHECK(charSvcCpp.find("ScheduleMainThreadFaceGenPass()") != std::string::npos);
    CHECK(charSvcCpp.find("FaceGenSystem::Update(") == std::string::npos);
    CHECK(charSvcCpp.find("World::Get().QueueMainThreadTask") != std::string::npos);

    const std::string assignSubCpp =
        LoadV313RepoFile(root, "Code/client/Services/Generic/CharacterMovementAndAssignmentSubservice.cpp");
    const auto pollPos = assignSubCpp.find("void CharacterService::PollLocalPlayerAppearanceSync() noexcept");
    const auto schedPos = assignSubCpp.find("void CharacterService::ScheduleMainThreadFaceGenPass() noexcept");
    REQUIRE(pollPos != std::string::npos);
    REQUIRE(schedPos != std::string::npos);
    const std::string pollBody = assignSubCpp.substr(pollPos, schedPos - pollPos);
    CHECK(pollBody.find("UpdateLocalPlayerHairTintIsolation") == std::string::npos);
}
