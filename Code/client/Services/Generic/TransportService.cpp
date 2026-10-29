
#include <Services/TransportService.h>

#include <Events/ConnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>

#include <Games/References.h>
#include <Games/TES.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESObjectCELL.h>

#include <TimeManager.h>

#include <Forms/TESNPC.h>
#include <TiltedOnlinePCH.h>
#include <World.h>

#include <Messages/AuthenticationRequest.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/NotifySettingsChange.h>
#include <Packet.hpp>
#include <steam/isteamnetworkingutils.h>

#include <ScriptExtender.h>
#include <Services/DiscordService.h>
#include <common/CoopPacketSafety.h>
#include <common/CoopWorldStabilizer.h>
#include <Utils.h>

// #include <imgui_internal.h>

static constexpr wchar_t kMO2DllName[] = L"usvfs_x64.dll";

using TiltedPhoques::Packet;

TransportService::TransportService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&TransportService::HandleUpdate>(this);
    m_settingsChangeConnection = m_dispatcher.sink<NotifySettingsChange>().connect<&TransportService::HandleNotifySettingsChange>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&TransportService::HandleConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&TransportService::HandleDisconnected>(this);

    m_connected = false;

    // Phase 3 (`WorldReloadBarrier`): keep GameNetworkingSockets connections alive across heavy
    // 600-plugin RFAB `.ess` world reloads (up to 60s blocking main-thread load) and prevent send-buffer
    // overflow while a peer's main thread is inside `BGSSaveLoadGame::LoadGame`.
    if (auto* pUtils = SteamNetworkingUtils())
    {
        pUtils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_TimeoutConnected, 60000);
        pUtils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendBufferSize, 4 * 1024 * 1024);
    }

    auto handlerGenerator = [this](auto& x)
    {
        using T = typename std::remove_reference_t<decltype(x)>::Type;

        m_messageHandlers[T::Opcode] = [this](UniquePtr<ServerMessage>& apMessage)
        {
            const auto pRealMessage = TiltedPhoques::CastUnique<T>(std::move(apMessage));
            m_dispatcher.trigger(*pRealMessage);
        };

        return false;
    };

    ServerMessageFactory::Visit(handlerGenerator);

    // Override authentication response
    m_messageHandlers[AuthenticationResponse::Opcode] = [this](UniquePtr<ServerMessage>& apMessage)
    {
        const auto pRealMessage = TiltedPhoques::CastUnique<AuthenticationResponse>(std::move(apMessage));
        HandleAuthenticationResponse(*pRealMessage);
    };
}

bool TransportService::Send(const ClientMessage& acMessage, const TiltedPhoques::EPacketFlags aPacketFlags) const noexcept
{
    static thread_local ScratchAllocator s_allocator(1 << 20);
    static thread_local uint32_t s_sendDepth = 0;

    CoopPacketSafety::ScopedScratchFrame allocatorGuard(s_allocator, s_sendDepth);

    if (IsConnected())
    {
        ScopedAllocator _{s_allocator};

        Buffer buffer(1 << 16);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(0, 8); // Write first byte as packet needs it

        acMessage.Serialize(writer);
        TiltedPhoques::PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), writer.Size());

        Client::Send(&packet, aPacketFlags);

        return true;
    }

    return false;
}

void TransportService::OnConsume(const void* apData, uint32_t aSize)
{
    ServerMessageFactory factory;
    TiltedPhoques::ViewBuffer buf((uint8_t*)apData, aSize);
    Buffer::Reader reader(&buf);

    auto pMessage = factory.Extract(reader);
    if (!pMessage)
    {
        spdlog::error("Couldn't parse packet from server");
        return;
    }

    const uint32_t opcode = static_cast<uint32_t>(pMessage->GetOpcode());
    if (opcode >= static_cast<uint32_t>(kServerOpcodeMax) || !m_messageHandlers[opcode])
    {
        spdlog::error("[RFAB Transport] Rejecting inbound server packet with invalid/unhandled opcode {}", opcode);
        return;
    }

    if (m_pollingWhilePaused)
    {
        switch (CoopWorldStabilizer::ClassifyPausedInboundPacket(opcode))
        {
        case CoopWorldStabilizer::PausedInboundPacketAction::kRejectOutOfRange:
        case CoopWorldStabilizer::PausedInboundPacketAction::kDropStaleUnreliable:
            return;

        case CoopWorldStabilizer::PausedInboundPacketAction::kDispatchImmediately:
            m_messageHandlers[opcode](pMessage);
            return;

        case CoopWorldStabilizer::PausedInboundPacketAction::kQueueUntilUnpaused:
            if (m_pausedInboundQueue.size() >= CoopWorldStabilizer::kMaxPausedInboundQueueSize)
            {
                spdlog::warn(
                    "[RFAB Transport] Paused inbound queue reached cap ({}); dropping oldest queued packet",
                    CoopWorldStabilizer::kMaxPausedInboundQueueSize);
                m_pausedInboundQueue.pop_front();
            }
            m_pausedInboundQueue.push_back(std::move(pMessage));
            return;
        }
    }

    m_messageHandlers[opcode](pMessage);
}

void TransportService::OnConnected()
{
    AuthenticationRequest request{};
    request.Version = BUILD_COMMIT;
    request.SKSEActive = IsScriptExtenderLoaded();
    request.MO2Active = GetModuleHandleW(kMO2DllName);

    request.Token = m_serverPassword;
    m_serverPassword = "";

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
    {
        spdlog::error("TransportService::OnConnected: PlayerCharacter::Get() is null; aborting handshake cleanly.");
        Close();
        return;
    }

    if (m_world.ctx().contains<DiscordService>())
        request.DiscordId = m_world.ctx().at<DiscordService>().GetUser().id;

    auto* pNpc = pPlayer->baseForm ? Cast<TESNPC>(pPlayer->baseForm) : nullptr;
    if (pNpc && pNpc->fullName.value.AsAscii())
    {
        request.Username = pNpc->fullName.value.AsAscii();
    }
    else
    {
        request.Username = "Dragonborn";
    }

    if (auto* const cpModManager = ModManager::Get())
    {
        for (auto* pMod : cpModManager->mods)
        {
            if (!pMod || !pMod->IsLoaded())
                continue;

            auto& entry = request.UserMods.ModList.emplace_back();
            entry.Id = pMod->GetId();
            entry.IsLite = pMod->IsLite();
            entry.Filename = pMod->filename;
        }
    }

    auto& modSystem = m_world.GetModSystem();
    if (auto* pWorldSpace = pPlayer->GetWorldSpace())
        modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId);

    if (pPlayer->parentCell)
        modSystem.GetServerModId(pPlayer->parentCell->formID, request.CellId);

    request.Level = pPlayer->GetLevel();

    if (auto* pGameTime = TimeData::Get())
    {
        if (pGameTime->TimeScale)
            request.PlayerTime.TimeScale = pGameTime->TimeScale->f;
        if (pGameTime->GameHour)
            request.PlayerTime.Time = pGameTime->GameHour->f;
        if (pGameTime->GameYear)
            request.PlayerTime.Year = pGameTime->GameYear->f;
        if (pGameTime->GameMonth)
            request.PlayerTime.Month = pGameTime->GameMonth->f;
        if (pGameTime->GameDay)
            request.PlayerTime.Day = pGameTime->GameDay->f;
    }

    Send(request);
}

void TransportService::OnDisconnected(EDisconnectReason aReason)
{
    m_connected = false;
    ClearPausedInboundPackets();
    World::SetConnectedHint(false);
    World::SetContainerMenuOpenHint(false);

    spdlog::warn("Disconnected from server {}", aReason);

    m_dispatcher.trigger(DisconnectedEvent());
}

void TransportService::OnUpdate()
{
}

double TransportService::GetMessageReceiveTime() const noexcept
{
    return static_cast<double>(GetCurrentMessageReceiveTime()) / 1000.0;
}

void TransportService::PollWhileSimulationPaused() noexcept
{
    if (auto* pUtils = SteamNetworkingUtils())
        m_lastPollTime = static_cast<double>(pUtils->GetLocalTimestamp()) / 1000.0;

    const bool prevPaused = m_pollingWhilePaused;
    m_pollingWhilePaused = true;
    Update();
    m_pollingWhilePaused = prevPaused;
}

void TransportService::FlushPausedInboundPackets() noexcept
{
    if (m_pausedInboundQueue.empty())
        return;

    std::deque<UniquePtr<ServerMessage>> pending;
    pending.swap(m_pausedInboundQueue);

    for (auto& pMessage : pending)
    {
        if (!m_connected)
            break;
        if (!pMessage)
            continue;

        const uint32_t opcode = static_cast<uint32_t>(pMessage->GetOpcode());
        if (opcode < static_cast<uint32_t>(kServerOpcodeMax) && m_messageHandlers[opcode])
            m_messageHandlers[opcode](pMessage);
    }
}

void TransportService::ClearPausedInboundPackets() noexcept
{
    m_pausedInboundQueue.clear();
}

void TransportService::HandleUpdate(const UpdateEvent& acEvent) noexcept
{
    FlushPausedInboundPackets();

    if (auto* pUtils = SteamNetworkingUtils())
        m_lastPollTime = static_cast<double>(pUtils->GetLocalTimestamp()) / 1000.0;

    Update();
}

void TransportService::HandleConnected(const ConnectedEvent& acEvent) noexcept
{
    m_localPlayerId = acEvent.PlayerId;
}

void TransportService::HandleDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    m_localPlayerId = NULL;
}

void TransportService::HandleAuthenticationResponse(const AuthenticationResponse& acMessage) noexcept
{
    using AR = AuthenticationResponse::ResponseType;
    if (acMessage.Type == AR::kAccepted)
    {
        m_connected = true;
        World::SetConnectedHint(true);

        m_world.SetServerSettings(acMessage.Settings);

        if (auto* const cpModManager = ModManager::Get())
        {
            std::vector<std::string> serverModNames;
            serverModNames.reserve(acMessage.UserMods.ModList.size());
            for (const auto& m : acMessage.UserMods.ModList)
                serverModNames.emplace_back(m.Filename.c_str(), m.Filename.size());

            std::vector<std::string> clientModNames;
            for (auto* pMod : cpModManager->mods)
            {
                if (pMod && pMod->IsLoaded() && pMod->filename[0] != '\0')
                    clientModNames.emplace_back(pMod->filename);
            }

            const auto audit = CoopPacketSafety::AuditModListCompatibility(serverModNames, clientModNames);
            if (!audit.Compatible)
            {
                spdlog::warn("[RFAB LoadOrder] {} (serverRfab={}, clientRfab={}, missingCritical={}, orderMismatches={})",
                    audit.SummaryRu,
                    audit.ServerHasRfab,
                    audit.ClientHasRfab,
                    audit.MissingCriticalMods.size(),
                    audit.OrderMismatchCount);
                if (!audit.SummaryRu.empty())
                    Utils::ShowHudMessage(String(audit.SummaryRu.c_str()));
            }
            else
            {
                spdlog::info("[RFAB LoadOrder] Mod list handshake verified cleanly (serverMods={}, clientMods={}, rfabActive={})",
                    serverModNames.size(),
                    clientModNames.size(),
                    audit.ClientHasRfab);
            }
        }

        m_dispatcher.trigger(acMessage.UserMods);
        m_dispatcher.trigger(acMessage.Settings);
        m_dispatcher.trigger(ConnectedEvent(acMessage.PlayerId));
        return; // quit the function here.
    }

    // error finding

    TiltedPhoques::String ErrorInfo;

    ErrorInfo = "{";

    switch (acMessage.Type)
    {
    case AR::kWrongVersion:
        ErrorInfo += "\"error\": \"wrong_version\", \"data\": {";
        ErrorInfo += fmt::format("\"expectedVersion\": \"{}\", \"version\": \"{}\"", acMessage.Version, BUILD_COMMIT);
        ErrorInfo += "}";
        break;
    case AR::kModsMismatch:
    {
        ErrorInfo += "\"error\": \"mods_mismatch\", \"data\": {\"mods\": [";
        bool first = true;
        for (const auto& m : acMessage.UserMods.ModList)
        {
            if (!first)
                ErrorInfo += ",";
            ErrorInfo += fmt::format("[\"{}\",\"{}\"]", m.Filename.c_str(), m.Id);
            first = false;
        }
        ErrorInfo += "]}";
        break;
    }
    case AR::kClientModsDisallowed:
    {
        ErrorInfo += "\"error\": \"client_mods_disallowed\", \"data\": { \"mods\": [";
        if (acMessage.SKSEActive)
            ErrorInfo += "\"SKSE\"";
        if (acMessage.MO2Active)
            if (acMessage.SKSEActive)
                ErrorInfo += ",";
        ErrorInfo += "\"MO2\"";
        ErrorInfo += "]}";
        break;
    }
    case AR::kWrongPassword:
    {
        ErrorInfo += "\"error\": \"wrong_password\"";
        break;
    }
    case AR::kServerFull:
    {
        ErrorInfo += "\"error\": \"server_full\"";
        break;
    }
    default: ErrorInfo += "\"error\": \"no_reason\""; break;
    }

    ErrorInfo += "}";

    ConnectionErrorEvent errorEvent;
    if (!ErrorInfo.empty())
    {
        spdlog::error(ErrorInfo.c_str());
        errorEvent.ErrorDetail = std::move(ErrorInfo);
    }

    m_dispatcher.trigger(errorEvent);
}

void TransportService::HandleNotifySettingsChange(const NotifySettingsChange& acMessage) noexcept
{
    m_world.SetServerSettings(acMessage.Settings);
    m_dispatcher.trigger(acMessage.Settings);
}
