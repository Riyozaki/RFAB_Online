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

#include <Events/PreUpdateEvent.h>
#include <Events/UpdateEvent.h>

#include <Games/Skyrim/Interface/UI.h>
#include <Misc/BSFixedString.h>
#include <ModCompat/BehaviorVar.h>  

World::World()
    : m_runner(m_dispatcher)
    , m_transport(*this, m_dispatcher)
    , m_modSystem(m_dispatcher)
    , m_lastFrameTime{std::chrono::high_resolution_clock::now()}
{
    s_mainThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    ctx().emplace<ImguiService>();
    m_pRfabAdapterService = &ctx().emplace<RfabAdapterService>(*this, m_dispatcher, m_transport);
    m_pDownedStateService = &ctx().emplace<DownedStateService>(*this, m_dispatcher, m_transport);
    ctx().emplace<CoordinatedSaveService>(*this, m_dispatcher, m_transport);
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

    BehaviorVar::Get()->Init();
}

World::~World()
{
    {
        std::lock_guard<std::mutex> lock(m_mainThreadTasksMutex);
        m_mainThreadTasks.clear();
    }
    s_isConnectedCached.store(false, std::memory_order_release);
    s_isContainerMenuOpenCached.store(false, std::memory_order_release);
    s_syncPlayerHomesCached.store(false, std::memory_order_release);
    s_pvpEnabledCached.store(false, std::memory_order_release);
    s_enemyHealthMultiplierCached.store(1.0f, std::memory_order_release);
    s_currentTickCached.store(0, std::memory_order_release);
    s_worldCreated.store(false, std::memory_order_release);
}

void World::Update() noexcept
{
    // Architectural invariant: `World::Update()` is driven strictly from `HookMainLoop` (`SE 35565`)
    // on Skyrim's real `WinMain` thread BEFORE `RealMainLoop` dispatches `BSJobs` workers or runs
    // `TES::PurgeExteriorCells`. `s_mainThreadId` is anchored exclusively in `World::World()` and
    // `World::OnMainThreadUpdate()`.
    const bool isConn = m_transport.IsConnected();
    s_isConnectedCached.store(isConn, std::memory_order_release);
    if (isConn)
    {
        static const BSFixedString* s_pContainerMenu = new BSFixedString("ContainerMenu");
        auto* const pUi = UI::Get();
        const bool containerOpen = pUi && pUi->GetMenuOpen(*s_pContainerMenu);
        s_isContainerMenuOpenCached.store(containerOpen, std::memory_order_release);
        s_currentTickCached.store(m_transport.GetClock().GetCurrentTick(), std::memory_order_release);
    }
    else
    {
        s_isContainerMenuOpenCached.store(false, std::memory_order_release);
        s_currentTickCached.store(0, std::memory_order_release);
    }

    const auto cNow = std::chrono::high_resolution_clock::now();
    const auto cDelta = cNow - m_lastFrameTime;
    m_lastFrameTime = cNow;

    const double rawDeltaSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(cDelta).count();
    const double cDeltaSeconds = std::clamp(rawDeltaSeconds, 0.0, 0.25);

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
    if (IsMainThread())
        return m_transport.GetClock().GetCurrentTick();
    return s_currentTickCached.load(std::memory_order_acquire);
}

void World::Create() noexcept
{
    if (!entt::locator<World>::has_value())
    {
        entt::locator<World>::emplace();
    }
    s_worldCreated.store(true, std::memory_order_release);
}

World& World::Get() noexcept
{
    return entt::locator<World>::value();
}

void World::OnMainThreadUpdate() noexcept
{
    s_mainThreadId.store(GetCurrentThreadId(), std::memory_order_release);

    std::vector<std::function<void()>> tasksToRun;
    {
        std::lock_guard<std::mutex> lock(m_mainThreadTasksMutex);
        if (m_mainThreadTasks.empty())
            return;
        tasksToRun.swap(m_mainThreadTasks);
    }

    for (auto& task : tasksToRun)
    {
        if (task)
            task();
    }
}

void World::QueueMainThreadTask(std::function<void()> aTask) noexcept
{
    if (!aTask)
        return;

    std::lock_guard<std::mutex> lock(m_mainThreadTasksMutex);
    if (m_mainThreadTasks.size() < 4096)
        m_mainThreadTasks.push_back(std::move(aTask));
}

bool World::IsMainThread() noexcept
{
    const uint32_t mainTid = s_mainThreadId.load(std::memory_order_acquire);
    return mainTid != 0 && GetCurrentThreadId() == mainTid;
}
