#include <TiltedOnlinePCH.h>

#include <Services/OverlayService.h>

#include <OverlayApp.hpp>

#include <D3D11Hook.hpp>
#include <OverlayRenderHandlerD3D11.hpp>

#include <Systems/RenderSystemD3D11.h>

#include <World.h>

#include <Services/OverlayClient.h>
#include <Services/TransportService.h>

#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerHealthUpdate.h>

#include <Structs/GridCellCoords.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>

#include <PlayerCharacter.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESObjectCELL.h>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Services/DownedStateService.h>
#include <common/RfabModpackCompat.h>

using TiltedPhoques::OverlayRenderHandler;
using TiltedPhoques::OverlayRenderHandlerD3D11;

struct D3D11RenderProvider final : OverlayApp::RenderProvider, OverlayRenderHandlerD3D11::Renderer
{
    explicit D3D11RenderProvider(RenderSystemD3D11* apRenderSystem)
        : m_pRenderSystem(apRenderSystem)
    {
    }

    OverlayRenderHandler* Create() override
    {
        auto* pHandler = new OverlayRenderHandlerD3D11(this);
        pHandler->SetVisible(true);

        return pHandler;
    }

    [[nodiscard]] HWND GetWindow() override { return m_pRenderSystem->GetWindow(); }

    [[nodiscard]] IDXGISwapChain* GetSwapChain() const noexcept override { return m_pRenderSystem->GetSwapChain(); }
    [[nodiscard]] ID3D11Device* GetDevice() const noexcept override { return m_pRenderSystem->GetDevice(); }
    [[nodiscard]] ID3D11DeviceContext* GetDeviceContext() const noexcept override { return m_pRenderSystem->GetDeviceContext(); }

private:
    RenderSystemD3D11* m_pRenderSystem;
};

String GetCellName(const GameId& aWorldSpaceId, const GameId& aCellId) noexcept
{
    auto& modSystem = World::Get().GetModSystem();

    String cellName = "UNKNOWN";

    if (aWorldSpaceId)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(aWorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
            cellName = pWorldSpace->GetName();
    }
    else
    {
        const uint32_t cellId = modSystem.GetGameId(aCellId);
        TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
        if (pCell)
            cellName = pCell->GetName();
    }

    return cellName;
}

float CalculateHealthPercentage(Actor* apActor) noexcept
{
    if (!apActor)
        return 0.f;

    const float maxHealth = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
    const float tempModHealth = apActor->healthModifiers.temporaryModifier;
    const float effectiveMaxHealth = maxHealth + tempModHealth;

    if (!std::isfinite(effectiveMaxHealth) || effectiveMaxHealth <= 0.001f)
        return 0.f;

    const float health = apActor->GetActorValue(ActorValueInfo::kHealth);
    if (!std::isfinite(health))
        return 0.f;

    const float percentage = (health / effectiveMaxHealth) * 100.f;
    return std::clamp(percentage, 0.f, 100.f);
}

OverlayService::OverlayService(World& aWorld, TransportService& transport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(transport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&OverlayService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&OverlayService::OnConnectedEvent>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&OverlayService::OnDisconnectedEvent>(this);
    m_connectionErrorConnection = aDispatcher.sink<ConnectionErrorEvent>().connect<&OverlayService::OnConnectionError>(this);
    m_chatMessageConnection = aDispatcher.sink<NotifyChatMessageBroadcast>().connect<&OverlayService::OnChatMessageReceived>(this);
    m_playerJoinedConnection = aDispatcher.sink<NotifyPlayerJoined>().connect<&OverlayService::OnPlayerJoined>(this);
    m_playerLeftConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&OverlayService::OnPlayerLeft>(this);
    m_playerDialogueConnection = aDispatcher.sink<NotifyPlayerDialogue>().connect<&OverlayService::OnPlayerDialogue>(this);
    m_playerAddedConnection = m_world.on_destroy<WaitingFor3D>().connect<&OverlayService::OnWaitingFor3DRemoved>(this);
    m_playerRemovedConnection = m_world.on_destroy<PlayerComponent>().connect<&OverlayService::OnPlayerComponentRemoved>(this);
    m_playerLevelConnection = aDispatcher.sink<NotifyPlayerLevel>().connect<&OverlayService::OnPlayerLevel>(this);
    m_cellChangedConnection = aDispatcher.sink<NotifyPlayerCellChanged>().connect<&OverlayService::OnPlayerCellChanged>(this);
    m_teleportConnection = aDispatcher.sink<NotifyTeleport>().connect<&OverlayService::OnNotifyTeleport>(this);
    m_playerHealthConnection = aDispatcher.sink<NotifyPlayerHealthUpdate>().connect<&OverlayService::OnNotifyPlayerHealthUpdate>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&OverlayService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&OverlayService::OnPartyLeftEvent>(this);
}

OverlayService::~OverlayService() noexcept
{
}

struct CoopOverlayApp final : OverlayApp
{
    CoopOverlayApp(RenderProvider* apProvider, OverlayClient* apClient)
        : OverlayApp(apProvider, apClient)
    {
    }

    void OnBeforeCommandLineProcessing(const CefString& aProcessType, CefRefPtr<CefCommandLine> aCommandLine) override
    {
        OverlayApp::OnBeforeCommandLineProcessing(aProcessType, aCommandLine);
        if (!aCommandLine)
            return;

        // Suppress Chromium background subsystems unused by an off-screen HUD overlay
        // (eliminates gpu_init, usb_descriptors, and media_router spam in cef_debug.log).
        aCommandLine->AppendSwitch("disable-gpu-shader-disk-cache");
        aCommandLine->AppendSwitch("disable-usb-keyboard-detect");
        aCommandLine->AppendSwitch("disable-background-networking");
        aCommandLine->AppendSwitchWithValue(
            "disable-features",
            "MediaRouter,WinUseBrowserSpellChecker,HardwareMediaKeyHandling");
        aCommandLine->AppendSwitchWithValue("log-severity", "error");
    }
};

void OverlayService::Create(RenderSystemD3D11* apRenderSystem) noexcept
{
    m_pProvider = TiltedPhoques::MakeUnique<D3D11RenderProvider>(apRenderSystem);
    m_pOverlay = new CoopOverlayApp(m_pProvider.get(), new ::OverlayClient(m_transport, m_pProvider->Create()));

    if (!m_pOverlay->Initialize())
    {
        spdlog::error("Overlay could not be initialized");
        if (int32_t exitCode = CefGetExitCode())
        {
            spdlog::critical("CEF failed to initialize, exit code {}. See 'cef_types.h' for description", exitCode);
        }
        return;
    }

    m_pOverlay->GetClient()->Create();
}

void OverlayService::Render() noexcept
{
    if (!m_pOverlay || !m_pOverlay->GetClient())
        return;

    auto pPlayer = PlayerCharacter::Get();
    bool inGame = pPlayer && pPlayer->GetNiNode();
    if (inGame && !m_inGame)
        SetInGame(true);
    else if (!inGame && m_inGame)
        SetInGame(false);

    m_pOverlay->GetClient()->Render();
}

void OverlayService::Reset() const noexcept
{
    if (!m_pOverlay || !m_pOverlay->GetClient())
        return;

    m_pOverlay->GetClient()->Reset();
}

void OverlayService::Reload() noexcept
{
    if (!m_pOverlay || !m_pOverlay->GetClient() || !m_pOverlay->GetClient()->GetBrowser())
        return;

    SetInGame(false);
    SetActive(false);
    m_pOverlay->GetClient()->GetBrowser()->Reload();
    Initialize();
    SetInGame(true);
    m_pOverlay->ExecuteAsync("enterGame");
    SetActive(true);
}

void OverlayService::Initialize() noexcept
{
    if (!m_pOverlay)
        return;

    m_pOverlay->ExecuteAsync("init");
}

void OverlayService::SetActive(bool aActive) noexcept
{
    if (!m_inGame || !m_pOverlay)
        return;
    if (m_active == aActive)
        return;

    m_active = aActive;

    m_pOverlay->ExecuteAsync(m_active ? "activate" : "deactivate");
}

bool OverlayService::GetActive() const noexcept
{
    return m_active;
}

void OverlayService::SetInGame(bool aInGame) noexcept
{
    if (m_inGame == aInGame || !m_pOverlay)
        return;
    m_inGame = aInGame;

    if (m_inGame)
    {
        SetVersion(BUILD_COMMIT);
        m_pOverlay->ExecuteAsync("enterGame");
    }
    else
    {
        m_pOverlay->ExecuteAsync("exitGame");
        // TODO: this does nothing, since m_inGame is false
        SetActive(false);
    }
}

bool OverlayService::GetInGame() const noexcept
{
    return m_inGame;
}

void OverlayService::SetVersion(const std::string& acVersion)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();

    pArguments->SetString(0, acVersion);
    m_pOverlay->ExecuteAsync("setVersion", pArguments);
}

void OverlayService::SendSystemMessage(const std::string& acMessage)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, kSystemMessage);
    pArguments->SetString(1, acMessage);

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::SetPlayerHealthPercentage(uint32_t aFormId) const noexcept
{
    if (!m_pOverlay)
        return;

    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (!pActor)
    {
        spdlog::debug("{}: cannot find actor for form id {:X}", __FUNCTION__, aFormId);
        return;
    }

    float percentage = CalculateHealthPercentage(pActor);

    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    auto entityIt = std::find_if(view.begin(), view.end(), [view, aFormId](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == aFormId; });

    if (entityIt == view.end())
    {
        spdlog::debug("{}: cannot find player entity for form id {:X}", __FUNCTION__, aFormId);
        return;
    }

    const auto& playerComponent = view.get<PlayerComponent>(*entityIt);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, playerComponent.Id);
    pArguments->SetDouble(1, static_cast<double>(percentage));
    m_pOverlay->ExecuteAsync("setHealth", pArguments);
}

void OverlayService::OnUpdate(const UpdateEvent&) noexcept
{
    RunDebugDataUpdates();
    RunPlayerHealthUpdates();
}

void OverlayService::OnConnectedEvent(const ConnectedEvent& acEvent) noexcept
{
    if (!m_pOverlay)
        return;

    m_pOverlay->ExecuteAsync("connect");

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acEvent.PlayerId);
    m_pOverlay->ExecuteAsync("setLocalPlayerId", pArguments);
}

void OverlayService::OnDisconnectedEvent(const DisconnectedEvent&) noexcept
{
    if (!m_pOverlay)
        return;

    m_pOverlay->ExecuteAsync("disconnect");
}

void OverlayService::OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    if (!m_pOverlay)
        return;

    const auto* pPlayerComponent = m_world.try_get<PlayerComponent>(aEntity);
    if (!pPlayerComponent)
        return;

    const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(aEntity);
    if (!pFormIdComponent)
        return;

    Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));
    if (!pActor)
    {
        spdlog::error("{}: cannot find actor for form id {:X}", __FUNCTION__, pFormIdComponent->Id);
        return;
    }

    float percentage = CalculateHealthPercentage(pActor);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, pPlayerComponent->Id);
    pArguments->SetInt(1, static_cast<int>(percentage));

    m_pOverlay->ExecuteAsync("setPlayer3dLoaded", pArguments);
}

void OverlayService::OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    if (!m_pOverlay)
        return;

    const auto* pPlayerComponent = m_world.try_get<PlayerComponent>(aEntity);
    if (!pPlayerComponent)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, pPlayerComponent->Id);

    m_pOverlay->ExecuteAsync("setPlayer3dUnloaded", pArguments);
}

void OverlayService::OnChatMessageReceived(const NotifyChatMessageBroadcast& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, (int)acMessage.MessageType);
    pArguments->SetString(1, acMessage.ChatMessage.c_str());
    pArguments->SetString(2, acMessage.PlayerName.c_str());

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::OnPlayerDialogue(const NotifyPlayerDialogue& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, kPlayerDialogue);
    pArguments->SetString(1, acMessage.Text.c_str());
    pArguments->SetString(2, acMessage.Name.c_str());

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::OnConnectionError(const ConnectionErrorEvent& acConnectedEvent) const noexcept
{
    if (!m_pOverlay)
        return;

    auto pArgs = CefListValue::Create();
    pArgs->SetString(0, acConnectedEvent.ErrorDetail.c_str());
    m_pOverlay->ExecuteAsync("triggerError", pArgs);
}

void OverlayService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetString(1, acMessage.Username.c_str());
    pArguments->SetInt(2, acMessage.Level);

    String cellName = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    pArguments->SetString(3, cellName.c_str());

    m_pOverlay->ExecuteAsync("playerConnected", pArguments);
}

void OverlayService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetString(1, acMessage.Username.c_str());
    m_pOverlay->ExecuteAsync("playerDisconnected", pArguments);
}

void OverlayService::OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetInt(1, acMessage.NewLevel);
    m_pOverlay->ExecuteAsync("setLevel", pArguments);
}

void OverlayService::OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) const noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    String cellName = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    pArguments->SetString(1, cellName.c_str());
    m_pOverlay->ExecuteAsync("setCell", pArguments);
}

void OverlayService::OnNotifyTeleport(const NotifyTeleport& acMessage) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    const bool isDowned = m_world.GetDownedStateService().IsLocalPlayerDowned();
    if (isDowned)
    {
        SendSystemMessage("Нельзя телепортироваться в состоянии ранения на колене.");
        return;
    }

    bool isLoading = pPlayer->IsStillLoading();
    if (auto* pUi = UI::Get())
    {
        isLoading = isLoading ||
                    pUi->GetMenuOpen(BSFixedString("Loading Menu")) ||
                    pUi->GetMenuOpen(BSFixedString("Main Menu"));
    }

    const bool hasCellId = static_cast<bool>(acMessage.CellId);
    const bool isTempCellId = acMessage.CellId.ModId == 0xFFFFFFFF;
    const bool hasWorldSpaceId = static_cast<bool>(acMessage.WorldSpaceId);

    if (!RfabCompat::CanResolveTeleportDestination(
            hasCellId,
            isTempCellId,
            hasWorldSpaceId,
            acMessage.Position.x,
            acMessage.Position.y,
            acMessage.Position.z,
            isDowned,
            isLoading))
    {
        spdlog::warn("OverlayService::OnNotifyTeleport rejected invalid or unsafe teleport destination.");
        SendSystemMessage("Teleporting to player failed.");
        return;
    }

    auto& modSystem = m_world.GetModSystem();
    TESObjectCELL* pCell = nullptr;

    // Never resolve unnamed 0xFFxxxxxx wilderness cells via TESForm::GetById, because runtime 0xFFxxxxxx IDs
    // are client-local and can collide with a completely different loaded exterior cell on this client.
    if (hasCellId && !isTempCellId)
    {
        const uint32_t cellId = modSystem.GetGameId(acMessage.CellId);
        pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
    }

    if (!pCell && hasWorldSpaceId)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(acMessage.WorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acMessage.Position);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }
    }

    if (!pCell)
    {
        spdlog::error("Failed to fetch cell to teleport to.");
        SendSystemMessage("Teleporting to player failed.");
        return;
    }

    pPlayer->MoveTo(pCell, acMessage.Position);
}

void OverlayService::OnNotifyPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    const float percentage = acMessage.Percentage >= 0.f ? acMessage.Percentage : 0.f;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetDouble(1, static_cast<double>(percentage));
    m_pOverlay->ExecuteAsync("setHealth", pArguments);
}

void OverlayService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    if (acEvent.IsLeader && m_pOverlay)
        m_pOverlay->ExecuteAsync("partyCreated");

    if (m_transport.IsConnected())
    {
        if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
        {
            RequestPlayerHealthUpdate request{};
            request.Percentage = CalculateHealthPercentage(pPlayer);
            m_transport.Send(request);
        }
    }
}

void OverlayService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    if (m_pOverlay)
        m_pOverlay->ExecuteAsync("partyLeft");
}

void OverlayService::RunDebugDataUpdates() noexcept
{
    if (!m_pOverlay)
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto internalStats = m_transport.GetStatistics();
    auto steamStats = m_transport.GetConnectionStatus();

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, steamStats.m_flOutPacketsPerSec);
    pArguments->SetInt(1, steamStats.m_flInPacketsPerSec);
    pArguments->SetInt(2, steamStats.m_nPing);
    pArguments->SetInt(3, 0);
    pArguments->SetInt(4, internalStats.SentBytes);
    pArguments->SetInt(5, internalStats.RecvBytes);

    m_pOverlay->ExecuteAsync("debugData", pArguments);
}

// TODO(cosideci): this whole thing is a really hacky solution to
// health sync code being somewhat broken for players.
void OverlayService::RunPlayerHealthUpdates() noexcept
{
    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    static float s_previousPercentage = -1.f;

    const float newPercentage = CalculateHealthPercentage(PlayerCharacter::Get());
    if (newPercentage == s_previousPercentage)
        return;

    s_previousPercentage = newPercentage;

    RequestPlayerHealthUpdate request{};
    request.Percentage = newPercentage;

    m_transport.Send(request);
}
