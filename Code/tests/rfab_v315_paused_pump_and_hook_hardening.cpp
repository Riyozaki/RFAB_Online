#include <TiltedCore/Stl.hpp>

#include <catch2/catch.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <common/CoopWorldStabilizer.h>

namespace
{
std::filesystem::path FindV315RepoRoot()
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

std::string LoadV315RepoFile(const std::filesystem::path& aRoot, const char* apRelPath)
{
    std::ifstream in(aRoot / apRelPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("v3.1.5 MainLoop update mode & paused-menu inbound packet classification", "[rfab][v315][stabilizer]")
{
    using CoopWorldStabilizer::ClassifyPausedInboundPacket;
    using CoopWorldStabilizer::MainLoopUpdateMode;
    using CoopWorldStabilizer::PausedInboundPacketAction;
    using CoopWorldStabilizer::SelectMainLoopUpdateMode;

    // 1. MainLoopUpdateMode selection across Main Menu, Loading Menu, active gameplay, and RFAB paused menus
    CHECK(SelectMainLoopUpdateMode(true, false, false, false, false, true, true, true) == MainLoopUpdateMode::kSkip);
    CHECK(SelectMainLoopUpdateMode(false, true, false, false, false, true, true, true) == MainLoopUpdateMode::kSkip);
    CHECK(SelectMainLoopUpdateMode(false, false, true, false, false, true, true, true) == MainLoopUpdateMode::kSkip);
    CHECK(SelectMainLoopUpdateMode(false, false, false, true, false, true, true, true) == MainLoopUpdateMode::kSkip);
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, true, true, true, true) == MainLoopUpdateMode::kSkip);

    // Normal unpaused gameplay when SkyrimVM ticked: full simulation update
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, true, true, true) ==
          MainLoopUpdateMode::kFullSimulationUpdate);

    // Paused in StatsMenu (RFAB Perk Tree), MapMenu, ESC Journal Menu, or Console in a loaded world:
    // keep GNS heartbeats, party/chat UI, and save/load barriers alive without mutating 3D world actors!
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, false, false, true) ==
          MainLoopUpdateMode::kPausedNetworkAndSavePump);
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, true, false, true) ==
          MainLoopUpdateMode::kPausedNetworkAndSavePump);

    // Before a world/save is loaded, never run the paused pump
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, false, false, false) ==
          MainLoopUpdateMode::kSkip);

    // 2. PausedInboundPacketAction classification
    CHECK(ClassifyPausedInboundPacket(static_cast<uint32_t>(kServerOpcodeMax)) ==
          PausedInboundPacketAction::kRejectOutOfRange);
    CHECK(ClassifyPausedInboundPacket(0xFFu) == PausedInboundPacketAction::kRejectOutOfRange);

    // 30 Hz unreliable position frames are dropped while frozen so multi-minute StatsMenu sessions never overflow
    CHECK(ClassifyPausedInboundPacket(kServerReferencesMoveRequest) ==
          PausedInboundPacketAction::kDropStaleUnreliable);

    // Handshake, party, chat, string-cache, settings, and save/load barrier control packets run immediately
    CHECK(ClassifyPausedInboundPacket(kAuthenticationResponse) == PausedInboundPacketAction::kDispatchImmediately);
    CHECK(ClassifyPausedInboundPacket(kNotifyPartyInvite) == PausedInboundPacketAction::kDispatchImmediately);
    CHECK(ClassifyPausedInboundPacket(kNotifyPartyJoined) == PausedInboundPacketAction::kDispatchImmediately);
    CHECK(ClassifyPausedInboundPacket(kNotifyChatMessageBroadcast) == PausedInboundPacketAction::kDispatchImmediately);
    CHECK(ClassifyPausedInboundPacket(kNotifyPrepareSave) == PausedInboundPacketAction::kDispatchImmediately);
    CHECK(ClassifyPausedInboundPacket(kNotifyCommitSave) == PausedInboundPacketAction::kDispatchImmediately);
    CHECK(ClassifyPausedInboundPacket(kNotifyExecuteLoad) == PausedInboundPacketAction::kDispatchImmediately);

    // Reliable world-mutating packets queue in FIFO order until simulation unpauses
    CHECK(ClassifyPausedInboundPacket(kAssignCharacterResponse) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifySpawnData) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifyPlayerLeft) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifyInventoryChanges) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifySpellCast) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifyActivate) == PausedInboundPacketAction::kQueueUntilUnpaused);
}

TEST_CASE("v3.1.5 Paused-pump wiring, scalar ActivateEvent & hook hardening contracts", "[rfab][v315][contracts]")
{
    const auto root = FindV315RepoRoot();
    REQUIRE_FALSE(root.empty());

    const std::string buildPs1 = LoadV315RepoFile(root, "Tools/rfab/Build-CloudRelease.ps1");
    const std::string pkgPs1 = LoadV315RepoFile(root, "Tools/rfab/Package-Release.ps1");
    REQUIRE(buildPs1.size() > 3);
    REQUIRE(pkgPs1.size() > 3);
    CHECK(static_cast<unsigned char>(buildPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(buildPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(buildPs1[2]) == 0xBF);
    CHECK(static_cast<unsigned char>(pkgPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(pkgPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(pkgPs1[2]) == 0xBF);
    CHECK(buildPs1.find("[string]$VersionTag = \"v3.1.5\"") != std::string::npos);
    CHECK(pkgPs1.find("[string]$VersionTag = \"v3.1.5\"") != std::string::npos);
    CHECK(buildPs1.find("\"v3.1.4\"") != std::string::npos);

    // 1. Paused-menu pump & opcode bounds safety
    const std::string skyrimVmCpp = LoadV315RepoFile(root, "Code/client/SkyrimVM64.cpp");
    CHECK(skyrimVmCpp.find("SelectMainLoopUpdateMode") != std::string::npos);
    CHECK(skyrimVmCpp.find("UpdatePausedPump()") != std::string::npos);

    const std::string transportCpp = LoadV315RepoFile(root, "Code/client/Services/Generic/TransportService.cpp");
    CHECK(transportCpp.find("opcode >= static_cast<uint32_t>(kServerOpcodeMax) || !m_messageHandlers[opcode]") != std::string::npos);
    CHECK(transportCpp.find("PollWhileSimulationPaused()") != std::string::npos);
    CHECK(transportCpp.find("FlushPausedInboundPackets()") != std::string::npos);
    CHECK(transportCpp.find("ClearPausedInboundPackets()") != std::string::npos);

    const std::string overlayClientCpp = LoadV315RepoFile(root, "Code/client/Services/Generic/OverlayClient.cpp");
    CHECK(overlayClientCpp.find("QueueMainThreadTask") != std::string::npos);

    const std::string coordSaveCpp = LoadV315RepoFile(root, "Code/client/Services/CoordinatedSaveService.cpp");
    CHECK(coordSaveCpp.find("BSFixedString(\"StatsMenu\")") != std::string::npos);
    CHECK(coordSaveCpp.find("BSFixedString(\"Console\")") != std::string::npos);
    CHECK(coordSaveCpp.find("TickWhilePaused") != std::string::npos);
    CHECK(coordSaveCpp.find("m_deferredInSessionRollback") != std::string::npos);

    // 2. Scalar ActivateEvent (zero raw engine pointers in deferred RunnerService events)
    const std::string activateEventH = LoadV315RepoFile(root, "Code/client/Events/ActivateEvent.h");
    CHECK(activateEventH.find("TESObjectREFR* pObject") == std::string::npos);
    CHECK(activateEventH.find("Actor* pActivator") == std::string::npos);
    CHECK(activateEventH.find("TESBoundObject* pObjectToGet") == std::string::npos);
    CHECK(activateEventH.find("uint32_t ObjectId{0};") != std::string::npos);
    CHECK(activateEventH.find("uint32_t ActivatorId{0};") != std::string::npos);

    // 3. Hook null/detaching-cell/thread-safety guards
    const std::string invisCpp = LoadV315RepoFile(root, "Code/client/Games/Skyrim/Effects/InvisibilityEffect.cpp");
    CHECK(invisCpp.find("IsAttachedAndLoaded()") != std::string::npos);
    CHECK(invisCpp.find("World::IsConnectedCached()") != std::string::npos);

    const std::string refsCpp = LoadV315RepoFile(root, "Code/client/Games/References.cpp");
    CHECK(refsCpp.find("s_cachedRfabActive") != std::string::npos);
    CHECK(refsCpp.find("ResolveRfabPluginMod") == std::string::npos);

    const std::string tesRefrCpp = LoadV315RepoFile(root, "Code/client/Games/Skyrim/TESObjectREFR.cpp");
    CHECK(tesRefrCpp.find("if (!World::IsMainThread())") != std::string::npos);
}
