
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
    static thread_local ScratchAllocator s_allocator(1 << 18);

    struct ScopedReset
    {
        ~ScopedReset() { s_allocator.Reset(); }
    } allocatorGuard;

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

    m_messageHandlers[pMessage->GetOpcode()](pMessage);
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

void TransportService::HandleUpdate(const UpdateEvent& acEvent) noexcept
{
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
