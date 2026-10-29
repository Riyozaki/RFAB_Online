#include <GameServer.h>

#include <Services/OverlayService.h>

#include <ChatMessageTypes.h>

#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/SendChatMessageRequest.h>
#include <Messages/PlayerDialogueRequest.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/TeleportRequest.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerHealthUpdate.h>

#include "Game/Player.h"
#include <common/CoopPacketSafety.h>

#include <algorithm>
#include <cmath>

namespace
{
constexpr size_t kMaxSanitizedChatLength = 1024;

String StripHtmlTagsNoThrow(const String& acInput) noexcept
{
    String out;
    const size_t maxBytes = std::min(acInput.size(), kMaxSanitizedChatLength);
    out.reserve(maxBytes);

    size_t i = 0;
    while (i < maxBytes)
    {
        if (acInput[i] == '<')
        {
            const size_t closePos = acInput.find('>', i + 1);
            if (closePos != String::npos && closePos < maxBytes)
            {
                i = closePos + 1;
                continue;
            }
            // Unclosed '<': skip literal '<' to prevent HTML injection without regex backtracking.
            ++i;
            continue;
        }
        out.push_back(acInput[i]);
        ++i;
    }

    return out;
}
} // namespace


OverlayService::OverlayService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_chatMessageConnection = aDispatcher.sink<PacketEvent<SendChatMessageRequest>>().connect<&OverlayService::HandleChatMessage>(this);
    m_playerDialogueConnection = aDispatcher.sink<PacketEvent<PlayerDialogueRequest>>().connect<&OverlayService::OnPlayerDialogue>(this);
    m_teleportConnection = aDispatcher.sink<PacketEvent<TeleportRequest>>().connect<&OverlayService::OnTeleport>(this);
    m_playerHealthConnection = aDispatcher.sink<PacketEvent<RequestPlayerHealthUpdate>>().connect<&OverlayService::OnPlayerHealthUpdate>(this);
}

void sendPlayerMessage(const ChatMessageType acType, const String acContent, Player* aSendingPlayer) noexcept
{
    if (!aSendingPlayer)
        return;

    NotifyChatMessageBroadcast notifyMessage{};
    notifyMessage.MessageType = acType;
    notifyMessage.PlayerName = StripHtmlTagsNoThrow(aSendingPlayer->GetUsername());
    notifyMessage.ChatMessage = StripHtmlTagsNoThrow(acContent);

    auto character = aSendingPlayer->GetCharacter();

    switch (notifyMessage.MessageType)
    {
    case kGlobalChat: GameServer::Get()->SendToPlayers(notifyMessage); break;

    case kSystemMessage: spdlog::error("PlayerId {} attempted to send a System Message.", aSendingPlayer->GetId()); break;

    case kPlayerDialogue: GameServer::Get()->SendToParty(notifyMessage, aSendingPlayer->GetParty()); break;

    case kPartyChat: GameServer::Get()->SendToParty(notifyMessage, aSendingPlayer->GetParty()); break;

    case kLocalChat:
        if (character)
        {
            if (!GameServer::Get()->SendToPlayersInRange(notifyMessage, *character))
                spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
        }
        break;
    default: spdlog::error("{} is not a known MessageType", static_cast<uint64_t>(notifyMessage.MessageType)); break;
    }
}

void OverlayService::HandleChatMessage(const PacketEvent<SendChatMessageRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    if (const auto character = acMessage.pPlayer->GetCharacter())
    {
        auto [canceled, reason] = m_world.GetScriptService().HandleChatMessage(*character, acMessage.Packet.ChatMessage);
        if (canceled)
            return;
    }

    sendPlayerMessage(acMessage.Packet.MessageType, acMessage.Packet.ChatMessage, acMessage.pPlayer);
}

void OverlayService::OnPlayerDialogue(const PacketEvent<PlayerDialogueRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    sendPlayerMessage(kPlayerDialogue, acMessage.Packet.Text, acMessage.pPlayer);
}

void OverlayService::OnTeleport(const PacketEvent<TeleportRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    Player* pTargetPlayer = m_world.GetPlayerManager().GetById(acMessage.Packet.PlayerId);
    if (!pTargetPlayer)
        return;

    auto character = pTargetPlayer->GetCharacter();
    if (!character || !m_world.valid(*character))
        return;

    const auto* pMovementComponent = m_world.try_get<MovementComponent>(*character);
    const auto& cellComponent = pTargetPlayer->GetCellComponent();
    if (!pMovementComponent ||
        !CoopPacketSafety::IsFiniteVec3(pMovementComponent->Position) ||
        (!cellComponent.Cell && !cellComponent.WorldSpaceId))
        return;

    NotifyTeleport response{};
    response.CellId = cellComponent.Cell;
    response.Position = pMovementComponent->Position;
    response.WorldSpaceId = cellComponent.WorldSpaceId;

    acMessage.pPlayer->Send(response);
}

void OverlayService::OnPlayerHealthUpdate(const PacketEvent<RequestPlayerHealthUpdate>& acMessage) const noexcept
{
    if (!acMessage.pPlayer || !std::isfinite(acMessage.Packet.Percentage))
        return;

    NotifyPlayerHealthUpdate notify{};
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.Percentage = std::clamp(acMessage.Packet.Percentage, 0.0f, 100.0f);

    GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
}
