#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <common/CoopTeleportGuard.h>
#include <common/CoopWorldStabilizer.h>

#include <entt/entt.hpp>
#include <Server.hpp>

#include <Services/RfabProgressionService.h>

namespace
{
std::filesystem::path FindV311RepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "CoopWorldStabilizer.h") &&
            fs::exists(cur / "Code" / "client" / "Games" / "Skyrim" / "Actor.cpp"))
        {
            return cur;
        }
        if (!cur.has_parent_path())
            break;
        fs::path parent = cur.parent_path();
        if (parent == cur)
            break;
        cur = std::move(parent);
    }
    return {};
}

std::string LoadV311RepoFile(const std::filesystem::path& aRoot, const char* acRelPath)
{
    std::ifstream ifs(aRoot / acRelPath, std::ios::binary);
    REQUIRE(ifs.is_open());
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}
} // namespace

TEST_CASE("v3.1.1 ActorDemotionHoldTracker eliminates High <-> Middle-High boundary thrash while removing unloaded/deleted actors in 0ms", "[rfab][v311][discovery]")
{
    CoopWorldStabilizer::ActorDemotionHoldTracker tracker{};
    constexpr uint32_t kBoundaryBandit = 0x0008433Cu;
    constexpr uint32_t kDeletedTempNpc = 0xFF001234u;

    tracker.NotifyPresentInHigh(kBoundaryBandit);

    // Dipping into Middle-High while still alive & enabled in an attached cell is retained for up to 3500ms:
    CHECK(tracker.ShouldRetainMissingFromHigh(kBoundaryBandit, true, false, 1000ULL));
    CHECK(tracker.ShouldRetainMissingFromHigh(kBoundaryBandit, true, false, 4499ULL));
    // Stepping back into High resets the demotion timer cleanly:
    tracker.NotifyPresentInHigh(kBoundaryBandit);
    CHECK(tracker.ShouldRetainMissingFromHigh(kBoundaryBandit, true, false, 6000ULL));
    // Remaining out of High continuously past 3500ms finally releases ownership cleanly:
    CHECK_FALSE(tracker.ShouldRetainMissingFromHigh(kBoundaryBandit, true, false, 9501ULL));

    // Unloaded cell or disabled/deleted actors are NEVER held (0ms immediate removal):
    tracker.NotifyPresentInHigh(kDeletedTempNpc);
    CHECK_FALSE(tracker.ShouldRetainMissingFromHigh(kDeletedTempNpc, false, false, 10000ULL));
    CHECK_FALSE(tracker.ShouldRetainMissingFromHigh(kDeletedTempNpc, true, true, 10000ULL));

    // Explicit post-3D species-swap hold retains through QueueUpdate() NiNode rebuild:
    tracker.ArmExplicitHold(0x00105A3Cu, 20000ULL, 3500ULL);
    CHECK(tracker.ShouldRetainMissingFromHigh(0x00105A3Cu, true, false, 23000ULL));
    CHECK_FALSE(tracker.ShouldRetainMissingFromHigh(0x00105A3Cu, true, false, 23501ULL));
}

TEST_CASE("v3.1.1 Wildlife variant & duplicate encounter stabilization across client and server", "[rfab][v311][wildlife]")
{
    SECTION("AuthoritativePlacedActorRegistry stores static placed ref -> static base mappings and rejects 0x14 / 0xFF00xxxx")
    {
        CoopWorldStabilizer::AuthoritativePlacedActorRegistry reg{};
        reg.Record(0x0009A110u, 0x00023ABEu);
        reg.Record(0x14u, 0x00023ABEu);
        reg.Record(0xFF000111u, 0x00023ABEu);
        reg.Record(0x0009A111u, 0xFF000222u);

        CHECK(reg.LookupBaseFormId(0x0009A110u) == 0x00023ABEu);
        CHECK(reg.LookupBaseFormId(0x14u) == 0u);
        CHECK(reg.LookupBaseFormId(0xFF000111u) == 0u);
        CHECK(reg.LookupBaseFormId(0x0009A111u) == 0u);

        reg.Clear();
        CHECK(reg.LookupBaseFormId(0x0009A110u) == 0u);
    }

    SECTION("Post-3D species swap triggers only for remote non-combat cross-species mismatches (e.g. Wolf vs Mudcrab)")
    {
        constexpr uint32_t kWolfRace = 0x0001320Au;
        constexpr uint32_t kMudcrabRace = 0x000131FBu;
        constexpr uint32_t kNordRace = 0x00013746u;

        CHECK(CoopWorldStabilizer::ShouldForcePost3DSpeciesSwap(
            false, false, false, false, false, kWolfRace, kMudcrabRace));
        // Same race (e.g. two Nord bandits) must NOT rebuild 3D post-stream:
        CHECK_FALSE(CoopWorldStabilizer::ShouldForcePost3DSpeciesSwap(
            false, false, false, false, false, kNordRace, kNordRace));
        // Local owner or mid-combat actor must NEVER be re-templated:
        CHECK_FALSE(CoopWorldStabilizer::ShouldForcePost3DSpeciesSwap(
            false, false, true, false, false, kWolfRace, kMudcrabRace));
        CHECK_FALSE(CoopWorldStabilizer::ShouldForcePost3DSpeciesSwap(
            false, false, false, false, true, kWolfRace, kMudcrabRace));
    }

    SECTION("Non-leader co-located 0xFF wild spawns are suppressed while summons, mounts, and solo exploration remain intact")
    {
        CHECK(CoopWorldStabilizer::ShouldSuppressNonLeaderTempWildSpawn(
            true, false, true, false, false, false, false, true));
        // Party leader keeps authoritative wild encounter spawns:
        CHECK_FALSE(CoopWorldStabilizer::ShouldSuppressNonLeaderTempWildSpawn(
            true, false, true, true, false, false, false, true));
        // Guest's own conjuration summons & mounts are NEVER suppressed:
        CHECK_FALSE(CoopWorldStabilizer::ShouldSuppressNonLeaderTempWildSpawn(
            true, false, true, false, false, true, false, true));
        CHECK_FALSE(CoopWorldStabilizer::ShouldSuppressNonLeaderTempWildSpawn(
            true, false, true, false, false, false, true, true));
        // Guest exploring far away from party leader keeps local encounters:
        CHECK_FALSE(CoopWorldStabilizer::ShouldSuppressNonLeaderTempWildSpawn(
            true, false, true, false, false, false, false, false));
    }

    SECTION("RfabProgressionLedger deduplicates 12s exterior cross-seam & leveled-variant ambushes while preserving multi-wolf packs")
    {
        RfabProgressionLedger ledger{};
        const GameId tamrielWs{0, 0x3C};
        const GameId cellA{0, 0x9001};
        const GameId cellB{0, 0x9002}; // Adjacent exterior cell across 4096 seam!
        const GameId wolfBase{0, 0x23ABE};
        const GameId foxBase{0, 0x23AB9};

        // Host spawns a 2-wolf pack on cellA side of seam:
        ledger.RegisterAmbushSpawn(701, 1, cellA, wolfBase, 4090.0f, 1000.0f, 50.0f, 1000ULL, 12000ULL, tamrielWs);
        ledger.RegisterAmbushSpawn(702, 1, cellA, wolfBase, 4094.0f, 1020.0f, 50.0f, 1020ULL, 12000ULL, tamrielWs);

        // 7.5 seconds later, Guest's trigger fires on cellB side of seam and rolls 1 Wolf + 1 Fox:
        CHECK(ledger.FindMatchingRecentAmbush(
                  2, cellB, wolfBase, 4102.0f, 1004.0f, 52.0f, 8500ULL, 12000ULL, 1400.0f, tamrielWs, true) == 701u);
        CHECK(ledger.FindMatchingRecentAmbush(
                  2, cellB, foxBase, 4105.0f, 1025.0f, 52.0f, 8520ULL, 12000ULL, 1400.0f, tamrielWs, true) == 702u);
        // A 3rd spawn has no unmatched Host counterpart -> returns 0:
        CHECK(ledger.FindMatchingRecentAmbush(
                  2, cellB, foxBase, 4108.0f, 1030.0f, 52.0f, 8530ULL, 12000ULL, 1400.0f, tamrielWs, true) == 0u);
    }
}

TEST_CASE("v3.1.1 Combat proximity wakeup, lethal overkill, corpse freeze, quickload masks, and teleport guards", "[rfab][v311][gameplay]")
{
    SECTION("Symmetric peer proximity aggro wakes hostile AI while skipping furniture Draugr, ghosts, sneaking, and floor separation")
    {
        CHECK(CoopWorldStabilizer::ShouldWakeLocalHostileNpcForPeer(
            false, false, false, false, false, false, false, true, false, false, false, 400.0f * 400.0f, 40.0f));
        // Sleeping Draugr in sarcophagus (`isNpcInFurniture = true`) must NOT wake prematurely:
        CHECK_FALSE(CoopWorldStabilizer::ShouldWakeLocalHostileNpcForPeer(
            false, false, false, false, false, true, false, true, false, false, false, 200.0f * 200.0f, 10.0f));
        // Sneaking peer must NOT bypass Skyrim stealth:
        CHECK_FALSE(CoopWorldStabilizer::ShouldWakeLocalHostileNpcForPeer(
            false, false, false, false, false, false, false, true, false, true, false, 200.0f * 200.0f, 10.0f));
        // Different dungeon floor (`|dz| > 220`) must NOT pull through ceilings:
        CHECK_FALSE(CoopWorldStabilizer::ShouldWakeLocalHostileNpcForPeer(
            false, false, false, false, false, false, false, true, false, false, false, 200.0f * 200.0f, 260.0f));
    }

    SECTION("Corpse ragdoll freeze and object/load-door activation filters")
    {
        CHECK(CoopWorldStabilizer::ShouldFreezeCorpseInterpolation(true));
        CHECK_FALSE(CoopWorldStabilizer::ShouldFreezeCorpseInterpolation(false));

        // Regular static chest/lever broadcasts & executes cleanly:
        CHECK(CoopWorldStabilizer::ShouldBroadcastObjectActivation(false, false, false));
        CHECK(CoopWorldStabilizer::ShouldExecuteRemoteObjectActivate(false, false));

        // Corpse/Actor, 0xFF temp object, and ExtraTeleport load doors are strictly blocked:
        CHECK_FALSE(CoopWorldStabilizer::ShouldBroadcastObjectActivation(true, false, false));
        CHECK_FALSE(CoopWorldStabilizer::ShouldBroadcastObjectActivation(false, true, false));
        CHECK_FALSE(CoopWorldStabilizer::ShouldBroadcastObjectActivation(false, false, true));
        CHECK_FALSE(CoopWorldStabilizer::ShouldExecuteRemoteObjectActivate(true, false));
        CHECK_FALSE(CoopWorldStabilizer::ShouldExecuteRemoteObjectActivate(false, true));
    }

    SECTION("Lethal overkill margin and premature local remote death recovery eliminate dead-vs-sliver-HP desync")
    {
        // 30 HP before hit -> 20% = 6.0f regen safety margin added to -14.2f => -20.2f:
        CHECK(CoopWorldStabilizer::EnsureLethalOverkillDelta(-14.2f, -15.0f, 30.0f, true) == Approx(-20.2f));
        // Larger intended raw blow (-28.0f) wins over -20.2f:
        CHECK(CoopWorldStabilizer::EnsureLethalOverkillDelta(-14.2f, -28.0f, 30.0f, true) == Approx(-28.0f));
        // Surviving hits (`aTargetDiedOrReachedZeroHp = false`) are unchanged:
        CHECK(CoopWorldStabilizer::EnsureLethalOverkillDelta(-14.2f, -28.0f, 30.0f, false) == Approx(-14.2f));

        // Premature local death on remote NPC resurrects once prediction window (>=3000ms) has elapsed and server HP > 5:
        CHECK(CoopWorldStabilizer::ShouldResurrectPrematureLocalRemoteDeath(true, true, false, 42.0f, 6500ULL, 3000ULL));
        CHECK_FALSE(CoopWorldStabilizer::ShouldResurrectPrematureLocalRemoteDeath(true, true, false, 42.0f, 4500ULL, 3000ULL));
        CHECK_FALSE(CoopWorldStabilizer::ShouldResurrectPrematureLocalRemoteDeath(true, true, true, 42.0f, 6500ULL, 3000ULL));
    }

    SECTION("Quickload task masks and late-session 4.5s RemoteQuestMoveToGuard with local E-press clear")
    {
        CHECK(CoopWorldStabilizer::IsQuickloadTaskMask(0x20u));
        CHECK(CoopWorldStabilizer::IsQuickloadTaskMask(0x80u));
        CHECK(CoopWorldStabilizer::IsQuickloadTaskMask(0xD0000100u));
        CHECK(CoopWorldStabilizer::IsQuickloadTaskMask(0xD0000200u));
        CHECK_FALSE(CoopWorldStabilizer::IsQuickloadTaskMask(0xF0000040u));

        CoopTeleportGuard::RemoteQuestMoveToGuard::ResetForTesting();
        {
            CoopTeleportGuard::ScopedRemoteQuestMoveToSuppression guard(
                1'000'000'000ULL,
                CoopTeleportGuard::RemoteQuestMoveToGuard::kExtendedLateSessionPapyrusHoldNs);
            CHECK(CoopTeleportGuard::RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(1'100'000'000ULL));
        }
        // Lagged Papyrus VM fragment 3.2s later is STILL blocked in late-session play:
        CHECK(CoopTeleportGuard::RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(4'200'000'000ULL));
        // Direct local player E-press clears the async hold immediately so local portal activation works:
        CoopTeleportGuard::RemoteQuestMoveToGuard::ClearAsyncHoldForLocalInteraction();
        CHECK_FALSE(CoopTeleportGuard::RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo(4'200'000'000ULL));
    }
}

TEST_CASE("v3.1.1 Source-level regression contracts across all 10 playtest fixes and 7 CTD guards", "[rfab][v311][contracts]")
{
    const auto root = FindV311RepoRoot();
    REQUIRE_FALSE(root.empty());

    const std::string buildPs1 = LoadV311RepoFile(root, "Tools/rfab/Build-CloudRelease.ps1");
    REQUIRE(buildPs1.size() > 3);
    CHECK(static_cast<unsigned char>(buildPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(buildPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(buildPs1[2]) == 0xBF);
    CHECK(buildPs1.find("Resolve-ReleaseVersionTag") != std::string::npos);
    CHECK(buildPs1.find("\"v3.1.0-backup\"") != std::string::npos);
    CHECK(buildPs1.find("Ensure-V310BackupRelease") != std::string::npos);
    CHECK(buildPs1.find("\"v3.1.1-backup\"") != std::string::npos);
    CHECK(buildPs1.find("Ensure-V311BackupRelease") != std::string::npos);

    const std::string backupManifest = LoadV311RepoFile(root, "docs/V3.1.0_RELEASE_BACKUP_MANIFEST.json");
    CHECK(backupManifest.find("\"v3.1.0-backup\"") != std::string::npos);
    CHECK(backupManifest.find("f9a3aeffc1d3bf06eddf248f379a1f1a91666af77476b563943ad130565dff9c") != std::string::npos);

    const std::string worldCpp = LoadV311RepoFile(root, "Code/client/World.cpp");
    CHECK(worldCpp.find("World::IsMainThread()") != std::string::npos);

    const std::string actorCpp = LoadV311RepoFile(root, "Code/client/Games/Skyrim/Actor.cpp");
    CHECK(actorCpp.find("LookupAuthoritativePlacedBaseFormId") != std::string::npos);
    CHECK(actorCpp.find("EnsureLethalOverkillDelta") != std::string::npos);
    CHECK(actorCpp.find("IsHostileToActor") != std::string::npos);
    CHECK(actorCpp.find("0x00000DB1u") != std::string::npos);

    const std::string tesRefrCpp = LoadV311RepoFile(root, "Code/client/Games/Skyrim/TESObjectREFR.cpp");
    CHECK(tesRefrCpp.find("containerEntry.ExtraWorn = false;") != std::string::npos);
    CHECK(tesRefrCpp.find("ShouldBroadcastObjectActivation") != std::string::npos);

    const std::string interpCpp = LoadV311RepoFile(root, "Code/client/Systems/InterpolationSystem.cpp");
    CHECK(interpCpp.find("ShouldFreezeCorpseInterpolation") != std::string::npos);

    const std::string objCpp = LoadV311RepoFile(root, "Code/client/Services/Generic/ObjectService.cpp");
    CHECK(objCpp.find("ShouldExecuteRemoteObjectActivate") != std::string::npos);
    CHECK(objCpp.find("ExtraDataType::Teleport") != std::string::npos);

    const std::string discCpp = LoadV311RepoFile(root, "Code/client/Services/Generic/DiscoveryService.cpp");
    CHECK(discCpp.find("m_demotionHoldTracker.ShouldRetainMissingFromHigh") != std::string::npos);

    const std::string coordSaveCpp = LoadV311RepoFile(root, "Code/client/Services/CoordinatedSaveService.cpp");
    CHECK(coordSaveCpp.find("m_pendingSaveRetriesLeft = 24") != std::string::npos);
    CHECK(coordSaveCpp.find("normName != normPreJoin") != std::string::npos);
    CHECK(coordSaveCpp.find("SetActorInventory(") == std::string::npos);
}
