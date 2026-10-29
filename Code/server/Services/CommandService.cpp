#include <Services/CommandService.h>

#include <Components.h>
#include <GameServer.h>
#include <World.h>
#include <common/CoopPacketSafety.h>

#include <Messages/SetTimeCommandRequest.h>
#include <Messages/NotifySetTimeResult.h>
#include <Messages/TeleportCommandRequest.h>
#include <Messages/TeleportCommandResponse.h>

CommandService::CommandService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_setTimeConnection = aDispatcher.sink<PacketEvent<SetTimeCommandRequest>>().connect<&CommandService::OnSetTimeCommand>(this);
    m_teleportConnection = aDispatcher.sink<PacketEvent<TeleportCommandRequest>>().connect<&CommandService::OnTeleportCommandRequest>(this);
}

void CommandService::OnSetTimeCommand(const PacketEvent<SetTimeCommandRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    NotifySetTimeResult response{};

    const auto cPlayerId = acMessage.pPlayer->GetId();
    bool isAuthorized = m_world.GetSaveSnapshotService().IsPlayerAuthorizedHost(acMessage.pPlayer);

    if (!isAuthorized)
    {
        for (const auto session : GameServer::Get()->GetAdminSessions())
        {
            if (const auto* pAdminPlayer = PlayerManager::Get()->GetByConnectionId(session))
            {
                if (pAdminPlayer->GetId() == cPlayerId)
                {
                    isAuthorized = true;
                    break;
                }
            }
        }
    }

    if (isAuthorized)
    {
        const auto cHours = static_cast<int>(acMessage.Packet.Hours);
        const auto cMinutes = static_cast<int>(acMessage.Packet.Minutes);

        if (!m_world.GetCalendarService().SetTime(cHours, cMinutes, m_world.GetCalendarService().GetTimeScale()))
            return;

        response.Result = NotifySetTimeResult::SetTimeResult::kSuccess;
        acMessage.pPlayer->Send(response);
        return;
    }

    response.Result = NotifySetTimeResult::SetTimeResult::kNoPermission;
    acMessage.pPlayer->Send(response);
}

void CommandService::OnTeleportCommandRequest(const PacketEvent<TeleportCommandRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    Player* pTargetPlayer = nullptr;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer && pPlayer->GetUsername() == acMessage.Packet.TargetPlayer)
            pTargetPlayer = pPlayer;
    }

    TeleportCommandResponse response{};
    if (pTargetPlayer)
    {
        auto character = pTargetPlayer->GetCharacter();
        if (character && m_world.valid(*character))
        {
            const auto* pMovementComponent = m_world.try_get<MovementComponent>(*character);
            const auto& cellComponent = pTargetPlayer->GetCellComponent();
            if (pMovementComponent &&
                CoopPacketSafety::IsFiniteVec3(pMovementComponent->Position) &&
                (cellComponent.Cell || cellComponent.WorldSpaceId))
            {
                response.CellId = cellComponent.Cell;
                response.Position = pMovementComponent->Position;
                response.WorldSpaceId = cellComponent.WorldSpaceId;
            }
        }
    }

    acMessage.pPlayer->Send(response);
}
