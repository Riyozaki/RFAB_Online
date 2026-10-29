#include <Services/MapService.h>
#include <GameServer.h>
#include <common/CoopPacketSafety.h>

#include <Messages/NotifyRemoveWaypoint.h>
#include <Messages/NotifySetWaypoint.h>
#include <Messages/RequestRemoveWaypoint.h>
#include <Messages/RequestSetWaypoint.h>

MapService::MapService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
      m_playerSetWaypointConnection = 
          aDispatcher.sink<PacketEvent<RequestSetWaypoint>>().connect<&MapService::OnSetWaypointRequest>(this);
      m_playerRemoveWaypointConnection =
          aDispatcher.sink<PacketEvent<RequestRemoveWaypoint>>().connect<&MapService::OnRemoveWaypointRequest>(this);
}

void MapService::OnSetWaypointRequest(const PacketEvent<RequestSetWaypoint>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    auto& message = acMessage.Packet;
    if (!message.WorldSpaceFormID || message.WorldSpaceFormID.ModId == 0xFFFFFFFFu ||
        !CoopPacketSafety::AreWorldCoordinatesValid(message.Position.x, message.Position.y, message.Position.z))
    {
        return;
    }

    NotifySetWaypoint notify{};
    notify.Position = message.Position;
    notify.WorldSpaceFormID = message.WorldSpaceFormID;

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    if (!partyComponent.JoinedPartyId.has_value())
        return;

    GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
}

void MapService::OnRemoveWaypointRequest(const PacketEvent<RequestRemoveWaypoint>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    NotifyRemoveWaypoint notify{};

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    if (!partyComponent.JoinedPartyId.has_value())
        return;

    GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
}

