#include <Services/WeatherService.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>

#include <Messages/RequestWeatherChange.h>
#include <Messages/NotifyWeatherChange.h>
#include <Messages/RequestCurrentWeather.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWeather.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Misc/BSFixedString.h>
#include <PlayerCharacter.h>
#include <Sky/Sky.h>

#include <array>

namespace
{
constexpr uint32_t kMapWeatherFormId = 0xA6858;

struct CachedSkyBlockingMenuNames
{
    std::array<BSFixedString, 9> Names{
        BSFixedString("StatsMenu"),
        BSFixedString("CustomMenu"),
        BSFixedString("MapMenu"),
        BSFixedString("Loading Menu"),
        BSFixedString("Main Menu"),
        BSFixedString("Fader Menu"),
        BSFixedString("Mist Menu"),
        BSFixedString("Sleep/Wait Menu"),
        BSFixedString("RaceSex Menu"),
    };
};

bool IsSkyDomeOrBlockingMenuOpen() noexcept
{
    UI* pUi = UI::Get();
    if (!pUi)
        return true;

    // Intentionally heap-allocated once on the game thread so ~BSFixedString() never runs after BSStringPool teardown.
    static const CachedSkyBlockingMenuNames* s_pMenus = new CachedSkyBlockingMenuNames();
    for (const auto& menuName : s_pMenus->Names)
    {
        if (pUi->GetMenuOpen(menuName))
            return true;
    }

    return false;
}

bool IsLocalPlayerInExteriorWorldspace() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return false;

    TESObjectCELL* pCell = pPlayer->parentCell;
    if (!pCell)
        return false;

    // Bit 0 of TESObjectCELL::cellFlags is kIsInteriorCell; exterior weather requires an active worldspace.
    if ((pCell->cellFlags[0] & 0x01u) != 0 || pCell->worldspace == nullptr)
        return false;

    return true;
}

// Guard Sky::ForceWeather / ReleaseWeatherOverride so we NEVER mutate Sky while StatsMenu (CustomSkills / RFAB
// kSkyDomeOnly mode where pSky->GetWeather() == nullptr), CustomMenu, MapMenu, Loading Menu, or an interior cell is active.
bool CanMutateOutdoorSkyWeather(Sky* apSky) noexcept
{
    if (!apSky)
        return false;

    if (!IsLocalPlayerInExteriorWorldspace())
        return false;

    if (IsSkyDomeOrBlockingMenuOpen())
        return false;

    TESWeather* pCurrentWeather = apSky->GetWeather();
    if (!pCurrentWeather || pCurrentWeather->formID == kMapWeatherFormId)
        return false;

    return true;
}
} // namespace

WeatherService::WeatherService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&WeatherService::OnUpdate>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&WeatherService::OnDisconnected>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&WeatherService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&WeatherService::OnPartyLeftEvent>(this);
    m_playerAddedConnection = m_world.on_destroy<WaitingFor3D>().connect<&WeatherService::OnWaitingFor3DRemoved>(this);
    m_playerRemovedConnection = m_world.on_destroy<PlayerComponent>().connect<&WeatherService::OnPlayerComponentRemoved>(this);
    m_weatherChangeConnection = aDispatcher.sink<NotifyWeatherChange>().connect<&WeatherService::OnWeatherChange>(this);
}

void WeatherService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    RunWeatherUpdates(acEvent.Delta);
}

void WeatherService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    ToggleGameWeatherSystem(true);
}

void WeatherService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    if (!acEvent.IsLeader)
    {
        auto view = m_world.view<PlayerComponent>();
        const auto& partyService = m_world.GetPartyService();

        for (auto entity : view)
        {
            const auto& playerComponent = view.get<PlayerComponent>(entity);
            if (playerComponent.Id == partyService.GetLeaderPlayerId())
            {
                ToggleGameWeatherSystem(false);
                break;
            }
        }
    }
    else
    {
        Sky* pSky = Sky::Get();
        if (!CanMutateOutdoorSkyWeather(pSky))
        {
            m_cachedWeatherId = 0;
            return;
        }

        TESWeather* pWeather = pSky->GetWeather();
        if (!pWeather || pWeather->formID == kMapWeatherFormId)
        {
            m_cachedWeatherId = 0;
            return;
        }

        m_cachedWeatherId = pWeather->formID;

        RequestWeatherChange request{};

        auto& modSystem = m_world.GetModSystem();
        if (!modSystem.GetServerModId(pWeather->formID, request.Id))
        {
            spdlog::error(__FUNCTION__ ": weather server ID not found, form id: {:X}", pWeather->formID);
            return;
        }

        m_transport.Send(request);
    }
}

void WeatherService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    ToggleGameWeatherSystem(true);
}

void WeatherService::OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    const auto* pPlayerComponent = m_world.try_get<PlayerComponent>(aEntity);
    if (!pPlayerComponent)
        return;

    const auto& partyService = m_world.GetPartyService();
    if (!partyService.IsInParty() || partyService.IsLeader())
        return;

    if (partyService.GetLeaderPlayerId() == pPlayerComponent->Id)
        ToggleGameWeatherSystem(false);
}

void WeatherService::OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    const auto& playerComponent = m_world.get<PlayerComponent>(aEntity);

    const auto& partyService = m_world.GetPartyService();
    if (!partyService.IsInParty() || partyService.IsLeader())
        return;

    if (partyService.GetLeaderPlayerId() == playerComponent.Id)
        ToggleGameWeatherSystem(true);
}

void WeatherService::OnWeatherChange(const NotifyWeatherChange& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();
    const uint32_t weatherId = modSystem.GetGameId(acMessage.Id);
    if (weatherId == 0 || weatherId == kMapWeatherFormId)
        return;

    TESWeather* pWeather = Cast<TESWeather>(TESForm::GetById(weatherId));
    if (!pWeather)
    {
        spdlog::error(__FUNCTION__ ": weather not found, form id: {:X}", acMessage.Id.ModId + acMessage.Id.BaseId);
        return;
    }

    // Always cache first; if the non-leader currently has StatsMenu open (kSkyDomeOnly) or is inside an interior,
    // defer ForceWeather until RunWeatherUpdates sees an active exterior Sky again.
    m_cachedWeatherId = weatherId;

    if (Sky* pSky = Sky::Get())
    {
        if (CanMutateOutdoorSkyWeather(pSky))
            pSky->ForceWeather(pWeather);
    }
}

void WeatherService::RunWeatherUpdates(const double acDelta) noexcept
{
    (void)acDelta;

    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty())
    {
        m_cachedWeatherId = 0;
        return;
    }

    Sky* pSky = Sky::Get();
    if (!pSky)
        return;

    const bool isLeader = m_world.GetPartyService().IsLeader();
    if (!CanMutateOutdoorSkyWeather(pSky))
    {
        // Clear leader cache only when actually inside an interior cell, never merely because StatsMenu opened;
        // on non-leaders, NEVER call ForceWeather onto StatsMenu's kSkyDomeOnly Sky or an interior Sky.
        if (isLeader && !IsLocalPlayerInExteriorWorldspace())
            m_cachedWeatherId = 0;

        return;
    }

    TESWeather* pWeather = pSky->GetWeather();
    if (!pWeather || pWeather->formID == kMapWeatherFormId)
        return;

    // Have to manually check each frame because there's no singular SetWeather being used in-game.
    if (pWeather->formID == m_cachedWeatherId)
        return;

    if (isLeader)
    {
        m_cachedWeatherId = pWeather->formID;

        RequestWeatherChange request{};

        auto& modSystem = m_world.GetModSystem();
        if (!modSystem.GetServerModId(pWeather->formID, request.Id))
        {
            spdlog::error(__FUNCTION__ ": weather server ID not found, form id: {:X}", pWeather->formID);
            return;
        }

        m_transport.Send(request);
    }
    else if (m_cachedWeatherId != 0)
    {
        SetCachedWeather();
    }
}

void WeatherService::ToggleGameWeatherSystem(bool aToggle) noexcept
{
    if (aToggle)
    {
        if (Sky* pSky = Sky::Get())
        {
            if (CanMutateOutdoorSkyWeather(pSky))
                pSky->ReleaseWeatherOverride();
        }
    }
    else if (m_transport.IsConnected())
    {
        m_transport.Send(RequestCurrentWeather());
    }

    m_cachedWeatherId = 0;
}

void WeatherService::SetCachedWeather() noexcept
{
    if (m_cachedWeatherId == 0 || m_cachedWeatherId == kMapWeatherFormId)
        return;

    Sky* pSky = Sky::Get();
    if (!CanMutateOutdoorSkyWeather(pSky))
        return;

    TESWeather* pWeather = Cast<TESWeather>(TESForm::GetById(m_cachedWeatherId));
    if (!pWeather)
    {
        spdlog::error(__FUNCTION__ ": weather not found, form id: {:X}", m_cachedWeatherId);
        return;
    }

    pSky->ForceWeather(pWeather);
}
