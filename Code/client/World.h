#pragma once

#include <Services/RunnerService.h>
#include <ThreadAffinity.h>
#include <Games/Events.h>
#include <Services/TransportService.h>
#include <Services/PartyService.h>
#include <Services/CharacterService.h>
#include <Services/OverlayService.h>
#include <Services/CharacterService.h>
#include <Services/MagicService.h>
#include <Services/DebugService.h>

#include <Systems/ModSystem.h>

#include <Structs/ServerSettings.h>

namespace WorldParts
{
// Owns the event dispatcher as the FIRST base class so entt::registry (and every service in ctx()
// holding entt::scoped_connection into m_dispatcher) is destroyed BEFORE m_dispatcher is freed.
struct DispatcherOwner
{
protected:
    entt::dispatcher m_dispatcher;
};
} // namespace WorldParts

struct World : WorldParts::DispatcherOwner, entt::registry, BSTEventSink<TESLoadGameEvent>
{
    World();
    ~World();

    void Update() noexcept;
    void PrepareForWorldLoad() noexcept;
    static RFAB_ANY_THREAD void SignalWorldLoad() noexcept;
    bool IsLoadingWorld() const noexcept { return m_loadingWorld.load(std::memory_order_acquire) || m_failedLoad.load(std::memory_order_acquire); }
    BSTEventResult OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*) override;
    static bool IsMainThread() noexcept { return ThreadAffinity::IsMainThread(); }
    static bool IsOnlineCached() noexcept
    {
        const auto* world = s_published.load(std::memory_order_acquire);
        return world && (world->m_nativeFlags.load(std::memory_order_acquire) & 1);
    }
    static bool IsPvpEnabledCached() noexcept
    {
        const auto* world = s_published.load(std::memory_order_acquire);
        return world && (world->m_nativeFlags.load(std::memory_order_acquire) & 2);
    }
    static bool SemanticRewardsCached() noexcept
    {
        const auto* world = s_published.load(std::memory_order_acquire);
        return world && (world->m_nativeFlags.load(std::memory_order_acquire) & 4);
    }
    void SetSemanticRewards(bool aEnabled) noexcept
    {
        if (aEnabled) m_nativeFlags.fetch_or(4u, std::memory_order_release);
        else m_nativeFlags.fetch_and(~4u, std::memory_order_release);
    }
    void SetNetworkOnline(bool aOnline) noexcept
    {
        if (aOnline) m_nativeFlags.fetch_or(1, std::memory_order_release);
        else m_nativeFlags.fetch_and(~5u, std::memory_order_release);
    }

    RunnerService& GetRunner() noexcept;
    TransportService& GetTransport() noexcept;
    ModSystem& GetModSystem() noexcept;

    PartyService& GetPartyService() noexcept { return ctx().at<PartyService>(); }
    const PartyService& GetPartyService() const noexcept { return ctx().at<const PartyService>(); }
    CharacterService& GetCharacterService() noexcept { return ctx().at<CharacterService>(); }
    const CharacterService& GetCharacterService() const noexcept { return ctx().at<const CharacterService>(); }
    OverlayService& GetOverlayService() noexcept { return ctx().at<OverlayService>(); }
    const OverlayService& GetOverlayService() const noexcept { return ctx().at<const OverlayService>(); }
    DebugService& GetDebugService() noexcept { return ctx().at<DebugService>(); }
    const DebugService& GetDebugService() const noexcept { return ctx().at<const DebugService>(); }
    MagicService& GetMagicService() noexcept { return ctx().at<MagicService>(); }
    const MagicService& GetMagicService() const noexcept { return ctx().at<const MagicService>(); }

    auto& GetDispatcher() noexcept { return m_dispatcher; }

    const ServerSettings& GetServerSettings() const noexcept { return m_serverSettings; }
    void SetServerSettings(ServerSettings aServerSettings) noexcept
    {
        m_serverSettings = aServerSettings;
        if (aServerSettings.PvpEnabled) m_nativeFlags.fetch_or(2, std::memory_order_release);
        else m_nativeFlags.fetch_and(~2u, std::memory_order_release);
    }

    [[nodiscard]] uint64_t GetTick() const noexcept;

    static void Create() noexcept;
    [[nodiscard]] static bool IsCreated() noexcept { return s_published.load(std::memory_order_acquire) != nullptr; }
    [[nodiscard]] static World& Get() noexcept;

private:
    inline static std::atomic<World*> s_published{nullptr};
    std::atomic<uint32_t> m_nativeFlags{0};
    std::atomic<uint64_t> m_tick{0};
    std::atomic_bool m_loadPending{false}, m_loadingWorld{false}, m_preloadPending{false};
    std::atomic_bool m_bridgeCompleted{false}, m_failedLoad{false};
    RunnerService m_runner;
    TransportService m_transport;
    ModSystem m_modSystem;
    ServerSettings m_serverSettings{};

    std::chrono::high_resolution_clock::time_point m_lastFrameTime;
};
