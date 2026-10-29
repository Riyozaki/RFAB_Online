#include <TiltedCore/Stl.hpp>

#include <catch2/catch.hpp>

#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <common/CoopWorldStabilizer.h>

namespace
{
std::filesystem::path FindV316RepoRoot()
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

std::string LoadV316RepoFile(const std::filesystem::path& aRoot, const char* apRelPath)
{
    std::ifstream in(aRoot / apRelPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("v3.1.6 Causal FIFO ordering during paused peer reconnect & high-Hz MainLoop mode", "[rfab][v316][audit]")
{
    using CoopWorldStabilizer::ClassifyPausedInboundPacket;
    using CoopWorldStabilizer::MainLoopUpdateMode;
    using CoopWorldStabilizer::PausedInboundPacketAction;
    using CoopWorldStabilizer::SelectMainLoopUpdateMode;

    // 1. Simulate a peer disconnecting and reconnecting while local player is paused in RFAB StatsMenu:
    //    `NotifyPlayerLeft` MUST NEVER be overtaken by `NotifyPlayerJoined` / `NotifyPartyInfo` / `NotifySpawnData`!
    const std::vector<ServerOpcode> arrivalSequence{
        kNotifyPlayerLeft,
        kServerReferencesMoveRequest,
        kNotifyChatMessageBroadcast,
        kNotifyPlayerJoined,
        kNotifyPartyInfo,
        kNotifySpawnData,
    };

    std::vector<ServerOpcode> immediateDispatched;
    std::deque<ServerOpcode> pausedQueue;
    for (const ServerOpcode op : arrivalSequence)
    {
        switch (ClassifyPausedInboundPacket(static_cast<uint32_t>(op)))
        {
        case PausedInboundPacketAction::kDispatchImmediately:
            immediateDispatched.push_back(op);
            break;
        case PausedInboundPacketAction::kQueueUntilUnpaused:
            pausedQueue.push_back(op);
            break;
        case PausedInboundPacketAction::kDropStaleUnreliable:
        case PausedInboundPacketAction::kRejectOutOfRange:
            break;
        }
    }

    // Only independent chat dispatched while paused; unreliable move dropped; causal sequence stays 100% intact!
    REQUIRE(immediateDispatched.size() == 1);
    CHECK(immediateDispatched[0] == kNotifyChatMessageBroadcast);

    REQUIRE(pausedQueue.size() == 4);
    CHECK(pausedQueue[0] == kNotifyPlayerLeft);
    CHECK(pausedQueue[1] == kNotifyPlayerJoined);
    CHECK(pausedQueue[2] == kNotifyPartyInfo);
    CHECK(pausedQueue[3] == kNotifySpawnData);

    // Exhaustive check across ALL valid ServerOpcodes: only Chat + 3 Save/Load barrier opcodes bypass the queue,
    // and only kServerReferencesMoveRequest is dropped as stale unreliable.
    for (uint32_t op = 0; op < static_cast<uint32_t>(kServerOpcodeMax); ++op)
    {
        const auto action = ClassifyPausedInboundPacket(op);
        if (op == static_cast<uint32_t>(kServerReferencesMoveRequest))
        {
            CHECK(action == PausedInboundPacketAction::kDropStaleUnreliable);
        }
        else if (op == static_cast<uint32_t>(kNotifyChatMessageBroadcast) ||
                 op == static_cast<uint32_t>(kNotifyPrepareSave) ||
                 op == static_cast<uint32_t>(kNotifyCommitSave) ||
                 op == static_cast<uint32_t>(kNotifyExecuteLoad))
        {
            CHECK(action == PausedInboundPacketAction::kDispatchImmediately);
        }
        else
        {
            CHECK(action == PausedInboundPacketAction::kQueueUntilUnpaused);
        }
    }

    // 2. High-Hz unpaused frame (`aSimulationUnpaused == true && aVmWasActive == false`) MUST return `kSkip`,
    //    never `kPausedNetworkAndSavePump` (which would drop live movement packets while unpaused!).
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, true, false, true) ==
          MainLoopUpdateMode::kSkip);
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, false, false, true) ==
          MainLoopUpdateMode::kPausedNetworkAndSavePump);
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, true, true, true) ==
          MainLoopUpdateMode::kFullSimulationUpdate);
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, true, true, false) ==
          MainLoopUpdateMode::kSkip);
}

TEST_CASE("v3.1.6 Adversarial audit architectural contracts across client & release pipeline", "[rfab][v316][contracts]")
{
    const auto root = FindV316RepoRoot();
    REQUIRE_FALSE(root.empty());

    // 1. Release script protection & BOM integrity (`v3.1.5` preserved in $ProtectedTags, bumped to `v3.1.6`)
    const std::string buildPs1 = LoadV316RepoFile(root, "Tools/rfab/Build-CloudRelease.ps1");
    const std::string pkgPs1 = LoadV316RepoFile(root, "Tools/rfab/Package-Release.ps1");
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
    CHECK(buildPs1.find("\"v3.1.5\"") != std::string::npos);

    // 2. TransportService::FlushPausedInboundPackets checks underlying GNS `IsConnected()`, not `m_connected`
    const std::string transportCpp = LoadV316RepoFile(root, "Code/client/Services/Generic/TransportService.cpp");
    CHECK(transportCpp.find("if (!m_connected)") == std::string::npos);
    CHECK(transportCpp.find("if (!IsConnected())") != std::string::npos);

    // 3. World::UpdatePausedPump uses dedicated `m_lastPausedPumpTime` + `IsPausedPumpActive()`
    const std::string worldH = LoadV316RepoFile(root, "Code/client/World.h");
    const std::string worldCpp = LoadV316RepoFile(root, "Code/client/World.cpp");
    CHECK(worldH.find("m_lastPausedPumpTime") != std::string::npos);
    CHECK(worldH.find("IsPausedPumpActive()") != std::string::npos);
    CHECK(worldH.find("GetInventoryService()") != std::string::npos);
    CHECK(worldCpp.find("cNow - m_lastPausedPumpTime") != std::string::npos);

    // 4. MagicTarget::HookFindTargets uses virtual `GetCasterAsActor()` (safe for 0x48 NonActorMagicCaster shrines/traps)
    const std::string magicTargetCpp = LoadV316RepoFile(root, "Code/client/Games/Skyrim/Magic/MagicTarget.cpp");
    CHECK(magicTargetCpp.find("static_cast<const ActorMagicCaster*>(apThis)") == std::string::npos);
    CHECK(magicTargetCpp.find("apThis->GetCasterAsActor()") != std::string::npos);

    // 5. PlayerCharacter::HookPickUpObject checks IsCreated + IsConnectedCached + !ScopedInventoryOverride::IsOverriden
    const std::string playerCharCpp = LoadV316RepoFile(root, "Code/client/Games/Skyrim/PlayerCharacter.cpp");
    const auto pickUpPos = playerCharCpp.find("HookPickUpObject");
    REQUIRE(pickUpPos != std::string::npos);
    const std::string pickUpSlice = playerCharCpp.substr(pickUpPos, 420);
    CHECK(pickUpSlice.find("World::IsCreated()") != std::string::npos);
    CHECK(pickUpSlice.find("World::IsConnectedCached()") != std::string::npos);
    CHECK(pickUpSlice.find("!ScopedInventoryOverride::IsOverriden()") != std::string::npos);

    // 6. TESObjectREFR::HookActivate never touches raw `apThis` after `RealActivate` on non-main threads
    const std::string tesRefrCpp = LoadV316RepoFile(root, "Code/client/Games/Skyrim/TESObjectREFR.cpp");
    CHECK(tesRefrCpp.find("Cast<TESObjectREFR>(TESForm::GetById(objectFormId)) : apThis") == std::string::npos);
    CHECK(tesRefrCpp.find("checkUnlockedAndEmit") != std::string::npos);

    // 7. CoordinatedSaveService + InventoryService reload barrier & paused-menu polish
    const std::string coordSaveCpp = LoadV316RepoFile(root, "Code/client/Services/CoordinatedSaveService.cpp");
    const std::string invCpp = LoadV316RepoFile(root, "Code/client/Services/Generic/InventoryService.cpp");
    CHECK(invCpp.find("void InventoryService::ResetForWorldReload() noexcept") != std::string::npos);
    CHECK(coordSaveCpp.find("m_world.GetInventoryService().ResetForWorldReload();") != std::string::npos);
    CHECK(coordSaveCpp.find("statsOpen || m_world.IsPausedPumpActive()") != std::string::npos);
    CHECK(coordSaveCpp.find("m_deferredLoadMenuCloseFrames") != std::string::npos);
}
