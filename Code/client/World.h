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
#include <Services/DiscoveryService.h>

#include <Systems/ModSystem.h>

#include <Structs/ServerSettings.h>

#include <atomic>

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
    DiscoveryService& GetDiscoveryService() noexcept { return ctx().at<DiscoveryService>(); }
    const DiscoveryService& GetDiscoveryService() const noexcept { return ctx().at<const DiscoveryService>(); }

    auto& GetDispatcher() noexcept { return m_dispatcher; }

    const ServerSettings& GetServerSettings() const noexcept { return m_serverSettings; }
    void SetServerSettings(ServerSettings aServerSettings) noexcept
    {
        m_serverSettings = aServerSettings;
        s_syncPlayerHomesCached.store(m_serverSettings.SyncPlayerHomes, std::memory_order_release);
    }

    [[nodiscard]] uint64_t GetTick() const noexcept;

    static void Create() noexcept;
    [[nodiscard]] static bool IsCreated() noexcept { return s_worldCreated.load(std::memory_order_acquire); }
    [[nodiscard]] static World& Get() noexcept;

    // Lock-free main-thread state snapshots safe to query from Papyrus VM & AI worker threads
    [[nodiscard]] static bool IsConnectedCached() noexcept { return s_isConnectedCached.load(std::memory_order_acquire); }
    [[nodiscard]] static bool IsContainerMenuOpenCached() noexcept { return s_isContainerMenuOpenCached.load(std::memory_order_acquire); }
    [[nodiscard]] static bool IsSyncPlayerHomesCached() noexcept { return s_syncPlayerHomesCached.load(std::memory_order_acquire); }
    static void SetConnectedHint(bool aConnected) noexcept { s_isConnectedCached.store(aConnected, std::memory_order_release); }
    static void SetContainerMenuOpenHint(bool aOpen) noexcept { s_isContainerMenuOpenCached.store(aOpen, std::memory_order_release); }

private:
    inline static std::atomic<bool> s_worldCreated{false};
    inline static std::atomic<bool> s_isConnectedCached{false};
    inline static std::atomic<bool> s_isContainerMenuOpenCached{false};
    inline static std::atomic<bool> s_syncPlayerHomesCached{false};

    entt::dispatcher m_dispatcher;
    RunnerService m_runner;
    TransportService m_transport;
    ModSystem m_modSystem;
    ServerSettings m_serverSettings{};

    std::chrono::high_resolution_clock::time_point m_lastFrameTime;
};
