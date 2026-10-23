#pragma once

#include <Services/RunnerService.h>
#include <Services/TransportService.h>
#include <Services/PartyService.h>
#include <Services/CharacterService.h>
#include <Services/ActorValueService.h>
#include <Services/OverlayService.h>
#include <Services/CharacterService.h>
#include <Services/MagicService.h>
#include <Services/DebugService.h>
#include <Services/RfabAdapterService.h>
#include <Services/DownedStateService.h>
#include <Services/CoordinatedSaveService.h>

#include <Systems/ModSystem.h>

#include <Structs/ServerSettings.h>

struct World : entt::registry
{
    World();
    ~World();

    void Update() noexcept;

    RunnerService& GetRunner() noexcept;
    TransportService& GetTransport() noexcept;
    ModSystem& GetModSystem() noexcept;

    PartyService& GetPartyService() noexcept { return ctx().at<PartyService>(); }
    const PartyService& GetPartyService() const noexcept { return ctx().at<const PartyService>(); }
    CharacterService& GetCharacterService() noexcept { return ctx().at<CharacterService>(); }
    const CharacterService& GetCharacterService() const noexcept { return ctx().at<const CharacterService>(); }
    ActorValueService& GetActorValueService() noexcept { return ctx().at<ActorValueService>(); }
    const ActorValueService& GetActorValueService() const noexcept { return ctx().at<const ActorValueService>(); }
    OverlayService& GetOverlayService() noexcept { return ctx().at<OverlayService>(); }
    const OverlayService& GetOverlayService() const noexcept { return ctx().at<const OverlayService>(); }
    DebugService& GetDebugService() noexcept { return ctx().at<DebugService>(); }
    const DebugService& GetDebugService() const noexcept { return ctx().at<const DebugService>(); }
    MagicService& GetMagicService() noexcept { return ctx().at<MagicService>(); }
    const MagicService& GetMagicService() const noexcept { return ctx().at<const MagicService>(); }
    RfabAdapterService& GetRfabAdapterService() noexcept { return ctx().at<RfabAdapterService>(); }
    const RfabAdapterService& GetRfabAdapterService() const noexcept { return ctx().at<const RfabAdapterService>(); }
    DownedStateService& GetDownedStateService() noexcept { return ctx().at<DownedStateService>(); }
    const DownedStateService& GetDownedStateService() const noexcept { return ctx().at<const DownedStateService>(); }
    CoordinatedSaveService& GetCoordinatedSaveService() noexcept { return ctx().at<CoordinatedSaveService>(); }
    const CoordinatedSaveService& GetCoordinatedSaveService() const noexcept { return ctx().at<const CoordinatedSaveService>(); }

    auto& GetDispatcher() noexcept { return m_dispatcher; }

    const ServerSettings& GetServerSettings() const noexcept { return m_serverSettings; }
    void SetServerSettings(ServerSettings aServerSettings) noexcept { m_serverSettings = aServerSettings; }

    [[nodiscard]] uint64_t GetTick() const noexcept;

    static void Create() noexcept;
    [[nodiscard]] static World& Get() noexcept;

private:
    entt::dispatcher m_dispatcher;
    RunnerService m_runner;
    TransportService m_transport;
    ModSystem m_modSystem;
    ServerSettings m_serverSettings{};

    std::chrono::high_resolution_clock::time_point m_lastFrameTime;
};
