#include <TiltedOnlinePCH.h>

#include <OverlayRenderHandler.hpp>
#include <DInputHook.hpp>

#include <Services/OverlayClient.h>
#include <Services/RfabAdapterService.h>
#include <Services/TransportService.h>

#include <Messages/SendChatMessageRequest.h>
#include <Messages/TeleportRequest.h>

#include <Events/SetTimeCommandEvent.h>
#include <PlayerCharacter.h>
#include <Utils.h>
#include <common/RfabModpackCompat.h>

#include <World.h>

OverlayClient::OverlayClient(TransportService& aTransport, TiltedPhoques::OverlayRenderHandler* apHandler)
    : TiltedPhoques::OverlayClient(apHandler)
    , m_transport(aTransport)
{
}

OverlayClient::~OverlayClient() noexcept
{
}

bool OverlayClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
    if (message->GetName() == "ui-event")
    {
        auto pArguments = message->GetArgumentList();

        auto eventName = pArguments->GetString(0).ToString();
        auto eventArgs = pArguments->GetList(1);

        spdlog::debug("[OverlayClient] ui-event: {}", eventName);

#ifndef PUBLIC_BUILD
        LOG(INFO) << "event=ui_event name=" << eventName;
#endif

        if (eventName == "connect")
            ProcessConnectMessage(eventArgs);
        else if (eventName == "disconnect")
            ProcessDisconnectMessage();
        else if (eventName == "revealPlayers")
            ProcessRevealPlayersMessage();
        else if (eventName == "sendMessage")
            ProcessChatMessage(eventArgs);
        else if (eventName == "setTime")
            ProcessSetTimeCommand(eventArgs);
        else if (eventName == "launchParty")
            World::Get().GetRunner().Queue([]() { World::Get().GetPartyService().CreateParty(); });
        else if (eventName == "leaveParty")
            World::Get().GetRunner().Queue([]() { World::Get().GetPartyService().LeaveParty(); });
        else if (eventName == "createPartyInvite")
        {
            const uint32_t playerId = eventArgs->GetInt(0);
            World::Get().GetRunner().Queue([playerId]() { World::Get().GetPartyService().CreateInvite(playerId); });
        }
        else if (eventName == "acceptPartyInvite")
        {
            const uint32_t inviterId = eventArgs->GetInt(0);
            // push to main thread because the party service has to check validity of invite thread safely
            World::Get().GetRunner().Queue([inviterId]() { World::Get().GetPartyService().AcceptInvite(inviterId); });
        }
        else if (eventName == "kickPartyMember")
        {
            const uint32_t playerId = eventArgs->GetInt(0);
            World::Get().GetRunner().Queue([playerId]() { World::Get().GetPartyService().KickPartyMember(playerId); });
        }
        else if (eventName == "changePartyLeader")
        {
            const uint32_t playerId = eventArgs->GetInt(0);
            World::Get().GetRunner().Queue([playerId]() { World::Get().GetPartyService().ChangePartyLeader(playerId); });
        }
        else if (eventName == "teleportToPlayer")
            ProcessTeleportMessage(eventArgs);
        else if (eventName == "toggleDebugUI")
            ProcessToggleDebugUI();
        else if (eventName == "setAllyProtection")
        {
            const bool enabled = eventArgs->GetSize() > 0 ? eventArgs->GetBool(0) : true;
            const bool notifyHud = eventArgs->GetSize() > 1 ? eventArgs->GetBool(1) : false;
            World::Get().GetRunner().Queue([enabled, notifyHud]() {
                World::Get().GetRfabAdapterService().SetLocalAllyProtectionEnabled(enabled, notifyHud);
            });
        }

        return true;
    }

    return false;
}

void OverlayClient::ProcessConnectMessage(CefRefPtr<CefListValue> aEventArgs)
{
    const std::string rawIp = aEventArgs->GetString(0).ToString();
    const uint16_t fallbackPort = aEventArgs->GetInt(1) ? static_cast<uint16_t>(aEventArgs->GetInt(1)) : 10578;
    const std::string password = aEventArgs->GetString(2).ToString();

    const std::string endpoint = RfabCompat::NormalizeServerEndpoint(rawIp, fallbackPort);
    spdlog::info("[RFAB Connect] Normalized connect target '{}' (port={}) -> '{}'", rawIp, fallbackPort, endpoint);

    World::Get().GetRunner().Queue(
        [endpoint, password]()
        {
            World::Get().GetTransport().SetServerPassword(password);
            PlayerCharacter* pPlayer = PlayerCharacter::Get();
            if (!pPlayer || (!pPlayer->parentCell && !pPlayer->GetWorldSpace()))
            {
                spdlog::warn("[RFAB Connect] Connect requested before save loaded into world; prompting player.");
                Utils::ShowHudMessage(String("Сначала загрузите сохранение или начните новую игру, затем нажмите F2 -> Connect"));
                return;
            }
            World::Get().GetTransport().ConnectToServer(endpoint, password);
        });
}

void OverlayClient::ProcessDisconnectMessage()
{
    World::Get().GetRunner().Queue([]() { World::Get().GetTransport().Close(); });
}

void OverlayClient::ProcessRevealPlayersMessage()
{
    SetUIVisible(false);
    World::Get().GetRunner().Queue([]() { World::Get().GetMagicService().StartRevealingOtherPlayers(); });
}

void OverlayClient::ProcessChatMessage(CefRefPtr<CefListValue> aEventArgs)
{
    std::string contents = aEventArgs->GetString(1).ToString();
    if (!contents.empty())
    {
        const auto msgType = static_cast<ChatMessageType>(aEventArgs->GetInt(0));
        spdlog::info(L"Send chat message of type {}: '{}' ", msgType, aEventArgs->GetString(1).ToWString());

        World::Get().GetRunner().Queue(
            [msgType, contents = std::move(contents)]()
            {
                SendChatMessageRequest messageRequest;
                messageRequest.MessageType = msgType;
                messageRequest.ChatMessage = contents;
                World::Get().GetTransport().Send(messageRequest);
            });
    }
}

void OverlayClient::ProcessSetTimeCommand(CefRefPtr<CefListValue> aEventArgs)
{
    const uint8_t hours = static_cast<uint8_t>(aEventArgs->GetInt(0));
    const uint8_t minutes = static_cast<uint8_t>(aEventArgs->GetInt(1));
    World::Get().GetRunner().Queue(
        [hours, minutes]()
        {
            const uint32_t senderId = World::Get().GetTransport().GetLocalPlayerId();
            World::Get().GetDispatcher().trigger(SetTimeCommandEvent(hours, minutes, senderId));
        });
}

void OverlayClient::ProcessTeleportMessage(CefRefPtr<CefListValue> aEventArgs)
{
    const uint32_t playerId = aEventArgs->GetInt(0);
    World::Get().GetRunner().Queue(
        [playerId]()
        {
            TeleportRequest request{};
            request.PlayerId = playerId;
            World::Get().GetTransport().Send(request);
        });
}

void OverlayClient::ProcessToggleDebugUI()
{
    World::Get().GetRunner().Queue(
        []() { World::Get().GetDebugService().m_showDebugStuff = !World::Get().GetDebugService().m_showDebugStuff; });
}

void OverlayClient::SetUIVisible(bool aVisible) noexcept
{
    auto pRenderer = GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    TiltedPhoques::DInputHook::Get().SetEnabled(aVisible);
    World::Get().GetOverlayService().SetActive(aVisible);
    pRenderer->SetCursorVisible(aVisible);
}
