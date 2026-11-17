#include <TiltedOnlinePCH.h>

#include "World.h"

#include <Services/DiscoveryService.h>
#include <Services/InputService.h>
#include <Services/TransportService.h>
#include <Services/RunnerService.h>
#include <Services/ImguiService.h>
#include <Services/PapyrusService.h>
#include <Services/DiscordService.h>
#include <Services/ObjectService.h>
#include <Services/QuestService.h>
#include <Services/ActorValueService.h>
#include <Services/InventoryService.h>
#include <Services/MagicService.h>
#include <Services/CommandService.h>
#include <Services/CalendarService.h>
#include <Services/StringCacheService.h>
#include <Services/PlayerService.h>
#include <Services/CombatService.h>
#include <Services/WeatherService.h>
#include <Services/MapService.h>
#include <Services/CoopService.h>
#include <Services/LootService.h>
#include <Services/SaveStateService.h>
#include <Services/StorageService.h>

#include <Events/PreUpdateEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/WorldLoadedEvent.h>

#include <ModCompat/BehaviorVar.h>

World::World()
    : m_runner(m_dispatcher)
    , m_transport(*this, m_dispatcher)
    , m_modSystem(m_dispatcher)
    , m_lastFrameTime{std::chrono::high_resolution_clock::now()}
{
    ctx().emplace<ImguiService>();
    ctx().emplace<DiscoveryService>(*this, m_dispatcher);
    ctx().emplace<OverlayService>(*this, m_transport, m_dispatcher);
    ctx().emplace<InputService>(ctx().at<OverlayService>());
    ctx().emplace<CharacterService>(*this, m_dispatcher, m_transport);
    ctx().emplace<DebugService>(m_dispatcher, *this, m_transport, ctx().at<ImguiService>());
    ctx().emplace<PapyrusService>(m_dispatcher);
    ctx().emplace<DiscordService>(m_dispatcher);
    ctx().emplace<ObjectService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CalendarService>(*this, m_dispatcher, m_transport);
    ctx().emplace<QuestService>(*this, m_dispatcher);
    ctx().emplace<PartyService>(*this, m_dispatcher, m_transport);
    ctx().emplace<ActorValueService>(*this, m_dispatcher, m_transport);
    ctx().emplace<InventoryService>(*this, m_dispatcher, m_transport);
    ctx().emplace<MagicService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CommandService>(*this, m_transport, m_dispatcher);
    ctx().emplace<PlayerService>(*this, m_dispatcher, m_transport);
    ctx().emplace<StringCacheService>(m_dispatcher);
    ctx().emplace<CombatService>(*this, m_transport, m_dispatcher);
    ctx().emplace<WeatherService>(*this, m_transport, m_dispatcher);
    ctx().emplace<MapService>(*this, m_dispatcher, m_transport);
    ctx().emplace<SaveStateService>(*this);
    ctx().emplace<CoopService>(*this, m_dispatcher, m_transport);
    ctx().emplace<LootService>(*this, m_dispatcher, m_transport);
    ctx().emplace<StorageService>(*this, m_dispatcher);

    BehaviorVar::Get()->Init();
    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

World::~World()
{
    s_published.store(nullptr, std::memory_order_release);
    EventDispatcherManager::Get()->loadGameEvent.UnRegisterSink(this);
    m_runner.Invalidate();
}

void World::SignalWorldLoad() noexcept
{
    auto* world = s_published.load(std::memory_order_acquire);
    if (!world) return;
    world->m_loadingWorld.store(true, std::memory_order_release);
    world->m_nativeFlags.fetch_and(~1u, std::memory_order_release);
    world->m_runner.Invalidate();
    world->m_preloadPending.store(true, std::memory_order_release);
}

void World::PrepareForWorldLoad() noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD()) return;
    SignalWorldLoad();
    m_preloadPending.store(false, std::memory_order_release);
    m_transport.Close();
    // Disconnect observers drop network bindings but NEVER queue Delete/Disable/Enable tasks
    // against actors that the engine is about to replace. The engine unloads those references.
}

BSTEventResult World::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    // The engine can report this from its load/VM path. Never enter the transport, dispatcher
    // or registry here. Invalidate immediately; finish teardown BEFORE the next discovery pass.
    m_loadingWorld.store(true, std::memory_order_release);
    m_nativeFlags.fetch_and(~1u, std::memory_order_release);
    m_runner.Invalidate();
    m_loadPending.store(true, std::memory_order_release);
    return BSTEventResult::kOk;
}

void World::Update() noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD())
        return;
    if (m_preloadPending.exchange(false, std::memory_order_acq_rel)) m_transport.Close();
    bool bridgeComplete = false;
    if (auto loaded = SaveStateBridge::TakeCompleted())
    {
        bridgeComplete = true;
        ctx().at<SaveStateService>().Adopt(std::move(*loaded));
        m_loadPending.store(true, std::memory_order_release);
    }
    // No discovery, packet consumers or posted engine work may run during native load. A failed
    // load is also completed by SKSE; it disconnects and faults persistence, never resumes a party.
    if (m_loadingWorld.load(std::memory_order_acquire) && SaveStateBridge::Installed() && !bridgeComplete) return;
    if (m_loadPending.exchange(false, std::memory_order_acq_rel))
    {
        m_runner.Invalidate();
        m_transport.Close();
        if (!SaveStateBridge::Installed()) ctx().at<SaveStateService>().AdoptWithoutBridge();
        m_dispatcher.trigger(WorldLoadedEvent{});
        m_loadingWorld.store(false, std::memory_order_release);
    }
    ctx().at<SaveStateService>().PollFault();
    m_tick.store(m_transport.GetClock().GetCurrentTick(), std::memory_order_release);
    const auto cNow = std::chrono::high_resolution_clock::now();
    const auto cDelta = cNow - m_lastFrameTime;
    m_lastFrameTime = cNow;

    const auto cDeltaSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(cDelta).count();

    if (m_runner.ConsumeOverflow())
    {
        spdlog::error("[RFAB] main-thread queue overflow: disconnecting rather than losing authoritative operations silently");
        m_runner.Invalidate();
        m_transport.Close();
    }
    m_dispatcher.trigger(PreUpdateEvent(cDeltaSeconds));

    // Force run this before so we get the tasks scheduled to run
    m_runner.OnUpdate(UpdateEvent(cDeltaSeconds));
    m_dispatcher.trigger(UpdateEvent(cDeltaSeconds));
}

RunnerService& World::GetRunner() noexcept
{
    return m_runner;
}

TransportService& World::GetTransport() noexcept
{
    return m_transport;
}

ModSystem& World::GetModSystem() noexcept
{
    return m_modSystem;
}

uint64_t World::GetTick() const noexcept
{
    return m_tick.load(std::memory_order_acquire);
}

void World::Create() noexcept
{
    ThreadAffinity::BindMainThread();
    if (!entt::locator<World>::has_value())
    {
        entt::locator<World>::emplace();
        s_published.store(&entt::locator<World>::value(), std::memory_order_release);
    }
}

World& World::Get() noexcept
{
    return *s_published.load(std::memory_order_acquire);
}
