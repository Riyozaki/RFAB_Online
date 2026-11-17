#include <TiltedOnlinePCH.h>
#include <OverlayRenderHandler.hpp>
#include <DInputHook.hpp>
#include <Services/OverlayClient.h>
#include <Services/TransportService.h>
#include <Messages/SendChatMessageRequest.h>
#include <Messages/TeleportRequest.h>
#include <Events/SetTimeCommandEvent.h>
#include <World.h>

OverlayClient::OverlayClient(TransportService& aTransport, TiltedPhoques::OverlayRenderHandler* apHandler)
    : TiltedPhoques::OverlayClient(apHandler), m_transport(aTransport)
{
}
OverlayClient::~OverlayClient() noexcept = default;

bool OverlayClient::OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefProcessId, CefRefPtr<CefProcessMessage> aMessage)
{
    if (!World::IsCreated() || aMessage->GetName() != "ui-event")
        return false;
    const auto arguments = aMessage->GetArgumentList();
    if (!arguments || arguments->GetSize() < 2)
        return false;
    const auto eventName = arguments->GetString(0).ToString();
    const auto eventArgs = arguments->GetList(1);
    if (!eventArgs)
        return false;
    // Never log the argument list: connect's third argument is a password.
    spdlog::debug("Overlay event: {}", eventName);
    if (eventName == "connect") ProcessConnectMessage(eventArgs);
    else if (eventName == "disconnect") ProcessDisconnectMessage();
    else if (eventName == "revealPlayers") ProcessRevealPlayersMessage();
    else if (eventName == "sendMessage") ProcessChatMessage(eventArgs);
    else if (eventName == "setTime") ProcessSetTimeCommand(eventArgs);
    else if (eventName == "teleportToPlayer") ProcessTeleportMessage(eventArgs);
    else if (eventName == "toggleDebugUI") ProcessToggleDebugUI();
    else if (eventName == "openPlayGuide") ProcessOpenPlayGuide();
    else if (eventName == "launchParty")
        World::Get().GetRunner().QueueForThisWorld([] { World::Get().GetPartyService().CreateParty(); });
    else if (eventName == "leaveParty")
        World::Get().GetRunner().QueueForThisWorld([] { World::Get().GetPartyService().LeaveParty(); });
    else if (eventArgs->GetSize() && (eventName == "createPartyInvite" || eventName == "acceptPartyInvite" ||
        eventName == "kickPartyMember" || eventName == "changePartyLeader"))
    {
        const auto playerId = static_cast<uint32_t>(eventArgs->GetInt(0));
        World::Get().GetRunner().QueueForThisWorld([eventName, playerId] {
            auto& party = World::Get().GetPartyService();
            if (eventName == "createPartyInvite") party.CreateInvite(playerId);
            else if (eventName == "acceptPartyInvite") party.AcceptInvite(playerId);
            else if (eventName == "kickPartyMember") party.KickPartyMember(playerId);
            else party.ChangePartyLeader(playerId);
        });
    }
    return true;
}

void OverlayClient::ProcessConnectMessage(CefRefPtr<CefListValue> aEventArgs)
{
    if (!World::IsCreated() || aEventArgs->GetSize() < 3)
        return;
    auto host = aEventArgs->GetString(0).ToString();
    if (host.empty() || host.size() > 255)
        return;
    if (host == "localhost") host = "127.0.0.1";
    const int requestedPort = aEventArgs->GetInt(1);
    const auto port = requestedPort > 0 && requestedPort <= 65535 ? requestedPort : 10578;
    const auto password = aEventArgs->GetString(2).ToString();
    const auto endpoint = host + ":" + std::to_string(port);
    World::Get().GetRunner().QueueForThisWorld([endpoint, password] {
        auto& transport = World::Get().GetTransport();
        transport.SetServerPassword(password);
        transport.Connect(endpoint);
    });
}
void OverlayClient::ProcessDisconnectMessage()
{
    if (World::IsCreated())
        World::Get().GetRunner().QueueForThisWorld([] { World::Get().GetTransport().Close(); });
}
void OverlayClient::ProcessRevealPlayersMessage()
{
    if (!World::IsCreated()) return;
    CefRefPtr<OverlayClient> self(this);
    World::Get().GetRunner().QueueForThisWorld([self] {
        self->SetUIVisible(false);
        World::Get().GetMagicService().StartRevealingOtherPlayers();
    });
}
void OverlayClient::ProcessChatMessage(CefRefPtr<CefListValue> aEventArgs)
{
    if (!World::IsCreated() || aEventArgs->GetSize() < 2) return;
    const auto contents = aEventArgs->GetString(1).ToString();
    const auto type = static_cast<ChatMessageType>(aEventArgs->GetInt(0));
    if (contents.empty() || contents.size() > 1024) return;
    World::Get().GetRunner().QueueForThisWorld([contents, type] {
        SendChatMessageRequest request;
        request.MessageType = type;
        request.ChatMessage = contents;
        World::Get().GetTransport().Send(request);
    });
}
void OverlayClient::ProcessSetTimeCommand(CefRefPtr<CefListValue> aEventArgs)
{
    if (!World::IsCreated() || aEventArgs->GetSize() < 2) return;
    const int hours = aEventArgs->GetInt(0), minutes = aEventArgs->GetInt(1);
    if (hours < 0 || hours >= 24 || minutes < 0 || minutes >= 60) return;
    World::Get().GetRunner().QueueForThisWorld([hours, minutes] {
        World::Get().GetDispatcher().trigger(SetTimeCommandEvent(static_cast<uint8_t>(hours), static_cast<uint8_t>(minutes),
            World::Get().GetTransport().GetLocalPlayerId()));
    });
}
void OverlayClient::ProcessTeleportMessage(CefRefPtr<CefListValue> aEventArgs)
{
    if (!World::IsCreated() || !aEventArgs->GetSize()) return;
    const auto playerId = static_cast<uint32_t>(aEventArgs->GetInt(0));
    World::Get().GetRunner().QueueForThisWorld([playerId] {
        TeleportRequest request;
        request.PlayerId = playerId;
        World::Get().GetTransport().Send(request);
    });
}
void OverlayClient::ProcessToggleDebugUI()
{
    if (!World::IsCreated()) return;
    World::Get().GetRunner().QueueForThisWorld([] {
        auto& debug = World::Get().GetDebugService();
        debug.m_showDebugStuff = !debug.m_showDebugStuff;
    });
}
void OverlayClient::ProcessOpenPlayGuide()
{
    ShellExecuteW(nullptr, L"open", LR"(https://wiki.tiltedphoques.com/tilted-online/general-information/playguide)", nullptr, nullptr, SW_SHOWNORMAL);
}
void OverlayClient::SetUIVisible(bool aVisible) noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD()) return;
    const auto renderer = GetOverlayRenderHandler();
    if (!renderer) return;
    TiltedPhoques::DInputHook::Get().SetEnabled(aVisible);
    World::Get().GetOverlayService().SetActive(aVisible);
    renderer->SetCursorVisible(aVisible);
}
