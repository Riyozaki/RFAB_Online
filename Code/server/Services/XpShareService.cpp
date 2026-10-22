#include <chrono>

#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>

#include <Messages/NotifySyncRfabExperience.h>
#include <Messages/SyncRfabExperienceRequest.h>

#include <Services/XpShareService.h>

namespace
{
Console::Setting bEnableRfabXpSync{"Gameplay:bEnableRfabXpSync", "Share RFAB experience between players", true};

Console::Setting bXpShareWholeServer{"Gameplay:bXpShareWholeServer",
                                     "Share experience with every player on the server (false: only the party)",
                                     true};

Console::Setting uXpShareDedupWindowMs{"Gameplay:uXpShareDedupWindowMs",
                                       "Collapse reports of the same experience amount coming from different players within this many "
                                       "milliseconds (0 disables, 5000 is a good default)",
                                       5000u};

uint64_t NowMs() noexcept
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
} // namespace

XpShareService::XpShareService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_shareConnection = aDispatcher.sink<PacketEvent<SyncRfabExperienceRequest>>().connect<&XpShareService::OnShareRequest>(this);
    m_playerJoinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&XpShareService::OnPlayerJoin>(this);
    m_playerLeaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&XpShareService::OnPlayerLeave>(this);
}

void XpShareService::OnShareRequest(const PacketEvent<SyncRfabExperienceRequest>& acMessage) noexcept
{
    if (!bEnableRfabXpSync.value_as<bool>())
        return;

    Player* pSender = acMessage.GetSender();
    if (pSender == nullptr)
        return;

    const uint32_t amount = Gameplay::XpShare::SanitiseAmount(acMessage.Packet.Experience);
    if (amount == 0)
        return;

    // Layer 1: exact replay protection. A client that reconnects restarts its
    // counter at 0, which is accepted, so a lost connection never blocks XP.
    auto& tracker = m_trackers[pSender->GetConnectionId()];
    if (!tracker.Accept(acMessage.Packet.Sequence))
    {
        ++m_stats.ReportsRejected;
        spdlog::debug("XP share: rejected replayed report #{} ({} XP) from {:X}",
                      acMessage.Packet.Sequence,
                      amount,
                      pSender->GetConnectionId());
        return;
    }

    ++m_stats.ReportsAccepted;
    m_stats.TotalAwarded += amount;

    // Layer 2: heuristic cross-player dedup. When the game itself synchronises a
    // quest/kill, every client runs its own RFAB handler and reports the same
    // amount almost simultaneously. Only the first report is fanned out.
    m_window.SetWindow(uXpShareDedupWindowMs.value_as<uint32_t>());

    if (uXpShareDedupWindowMs.value_as<uint32_t>() > 0 && !m_window.ShouldBroadcast(amount, NowMs()))
    {
        ++m_stats.ReportsSuppressed;
        spdlog::debug("XP share: suppressed duplicate amount {} from {:X}", amount, pSender->GetConnectionId());
        return;
    }

    NotifySyncRfabExperience notify{};
    notify.Experience = amount;

    if (bXpShareWholeServer.value_as<bool>())
    {
        GameServer::Get()->SendToPlayers(notify, pSender);
        return;
    }

    const auto& partyComponent = pSender->GetParty();
    GameServer::Get()->SendToParty(notify, partyComponent, pSender);
}

void XpShareService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    if (acEvent.pPlayer != nullptr)
        m_trackers[acEvent.pPlayer->GetConnectionId()].Reset();
}

void XpShareService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (acEvent.pPlayer != nullptr)
        m_trackers.erase(acEvent.pPlayer->GetConnectionId());
}
