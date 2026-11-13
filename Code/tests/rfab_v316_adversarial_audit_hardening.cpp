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
        case PausedInboundPacketAction::kCoalesceUntilUnpaused:
        case PausedInboundPacketAction::kDropStaleUnreliable:
        case PausedInboundPacketAction::kRejectOutOfRange:
            break;
        }
    }

    // Only independent chat dispatched while paused; move folded into the coalesced request; causal sequence intact.
    REQUIRE(immediateDispatched.size() == 1);
    CHECK(immediateDispatched[0] == kNotifyChatMessageBroadcast);

    REQUIRE(pausedQueue.size() == 4);
    CHECK(pausedQueue[0] == kNotifyPlayerLeft);
    CHECK(pausedQueue[1] == kNotifyPlayerJoined);
    CHECK(pausedQueue[2] == kNotifyPartyInfo);
    CHECK(pausedQueue[3] == kNotifySpawnData);

    // Exhaustive check across ALL valid ServerOpcodes: only Chat + 3 Save/Load barrier opcodes bypass the queue,
    // and only kServerReferencesMoveRequest is coalesced (never dropped).
    for (uint32_t op = 0; op < static_cast<uint32_t>(kServerOpcodeMax); ++op)
    {
        const auto action = ClassifyPausedInboundPacket(op);
        if (op == static_cast<uint32_t>(kServerReferencesMoveRequest))
        {
            CHECK(action == PausedInboundPacketAction::kCoalesceUntilUnpaused);
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

