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
    // keep GNS heartbeats, chat UI, and save/load barriers alive without mutating 3D world actors!
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, false, false, true) ==
          MainLoopUpdateMode::kPausedNetworkAndSavePump);
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, false, true, true) ==
          MainLoopUpdateMode::kPausedNetworkAndSavePump);

    // Unpaused high-Hz frame between SkyrimVM ticks (or unpause transition frame): NEVER drop live movement!
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, true, false, true) ==
          MainLoopUpdateMode::kSkip);

    // Before a world/save is loaded, never run either simulation update or paused pump
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, false, false, false) ==
          MainLoopUpdateMode::kSkip);
    CHECK(SelectMainLoopUpdateMode(false, false, false, false, false, true, true, false) ==
          MainLoopUpdateMode::kSkip);

    // 2. PausedInboundPacketAction classification
    CHECK(ClassifyPausedInboundPacket(static_cast<uint32_t>(kServerOpcodeMax)) ==
          PausedInboundPacketAction::kRejectOutOfRange);
    CHECK(ClassifyPausedInboundPacket(0xFFu) == PausedInboundPacketAction::kRejectOutOfRange);

    // Movement frames are folded into one pending request while frozen (never dropped: the ones carrying
    // animation ActionEvents are reliable) - see PausedInboundCoalescer.h / registry N2.
    CHECK(ClassifyPausedInboundPacket(kServerReferencesMoveRequest) ==
          PausedInboundPacketAction::kCoalesceUntilUnpaused);

    // Independent chat & coordinated save/load barrier control packets run immediately while paused
    CHECK(ClassifyPausedInboundPacket(kNotifyChatMessageBroadcast) == PausedInboundPacketAction::kDispatchImmediately);
    CHECK(ClassifyPausedInboundPacket(kNotifyPrepareSave) == PausedInboundPacketAction::kDispatchImmediately);
    CHECK(ClassifyPausedInboundPacket(kNotifyCommitSave) == PausedInboundPacketAction::kDispatchImmediately);
    CHECK(ClassifyPausedInboundPacket(kNotifyExecuteLoad) == PausedInboundPacketAction::kDispatchImmediately);

    // ALL reliable handshake, party, join/leave, and world-state packets queue in strict single-stream causal FIFO
    // order until simulation unpauses (so NotifyPlayerJoined can never overtake NotifyPlayerLeft!)
    CHECK(ClassifyPausedInboundPacket(kAuthenticationResponse) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifyPartyInvite) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifyPartyJoined) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifyPlayerJoined) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kAssignCharacterResponse) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifySpawnData) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifyPlayerLeft) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifyInventoryChanges) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifySpellCast) == PausedInboundPacketAction::kQueueUntilUnpaused);
    CHECK(ClassifyPausedInboundPacket(kNotifyActivate) == PausedInboundPacketAction::kQueueUntilUnpaused);
}

