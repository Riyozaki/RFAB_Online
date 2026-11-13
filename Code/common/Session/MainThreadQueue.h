#pragma once

// Architecture step 3: the one way work crosses from an engine thread to the client's main thread.
//
// Skyrim runs our hooks on whatever thread the engine happens to use: Havok/AI job workers (damage,
// magic, combat targeting), Papyrus VM threads (script natives, quest events), the CEF IPC thread
// (overlay UI), the save/load path. The client's state (the entt registry, the services, the
// transport) belongs to the main thread. The rule, enforced by Code/tests/rfab_v410_hook_discipline.cpp:
//
//   an off-thread entry point only reads lock-free / internally synchronised state (methods named
//   *AnyThread, World::*Cached(), Session::Lifecycle::*AnyThread(), ModSystem's id translation)
//   and posts everything else here.
//
// Before v4.1.0 two queues did this. RunnerService wrapped TiltedPhoques::TaskQueue, whose Drain()
// runs every task WHILE HOLDING the queue mutex: a Havok worker posting a HitEvent from a damage hook
// blocked for the whole main-thread batch (handlers, sends, engine calls), and a handler that waited
// on an engine lock held by that worker froze the game (lock-order inversion). World kept a second
// vector for UI work that silently dropped tasks past 4096. Neither knew about sessions: a gameplay
// event queued while a menu paused the simulation ran after the session it belonged to had ended.
//
// MainThreadQueue:
//   * lanes          - kSimulation runs once per simulated frame (World::Update, the old runner);
//                      kAlways runs on every main-loop iteration, menus included (UI commands, the
//                      save/load control path).
//   * batch drain    - Drain() takes the queued batch under the lock and runs it OUTSIDE the lock.
//                      Posting never waits for a task, and a task may post (it runs next drain).
//   * session scope  - every task carries the poster's Session::Token. A task posted inside session N
//                      is dropped if session N is over when it would run. Work posted while no session
//                      exists (connect, settings, offline save control) always runs.
//   * accounting     - posted / executed / stale / overflow / failed / posted-during-world-load, so a
//                      playtest log shows when the queue misbehaves instead of guessing.
//
// Portable (no engine headers): the Linux suite drives it with real threads.

#include <common/Session/Lifecycle.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace Session
{
enum class Lane : uint8_t
{
    kSimulation = 0,  // drained by World::Update: gameplay events from hooks, deferred service work
    kAlways = 1,      // drained every main-loop iteration, paused menus included: UI, save/load control
    kCount
};

inline constexpr size_t kLaneCount = static_cast<size_t>(Lane::kCount);

[[nodiscard]] constexpr std::string_view ToString(Lane aLane) noexcept
{
    switch (aLane)
    {
    case Lane::kSimulation: return "simulation";
    case Lane::kAlways: return "always";
    case Lane::kCount: break;
    }
    return "?";
}

struct LaneStats
{
    uint64_t Posted{0};
    uint64_t Executed{0};
    uint64_t DroppedStale{0};           // posted inside a session that ended before the task could run
    uint64_t DroppedOverflow{0};        // the lane was full (or out of memory) when it was posted
    uint64_t Failed{0};                 // the task threw; the queue kept going
    uint64_t PostedDuringWorldLoad{0};  // diagnostics: engine-generated work while a save was loading
    uint64_t Pending{0};
    uint64_t HighWater{0};              // the most tasks ever waiting at once
};

class MainThreadQueue
{
public:
    using Task = std::function<void()>;

    // The simulation lane is not drained while a menu or a loading screen holds the simulation, and a
    // coordinated reload of a modded save fires inventory/actor hooks by the thousand: the old runner
    // was unbounded, so the limit sits far above any burst and only stops a stuck lane from eating
    // the process (~20 MB when full). HighWater in the session-end log shows the real peak.
    // The always-lane keeps the old 4096.
    static constexpr size_t kDefaultSimulationCapacity = 1u << 18;
    static constexpr size_t kDefaultAlwaysCapacity = 4096;

    explicit MainThreadQueue(size_t aSimulationCapacity = kDefaultSimulationCapacity, size_t aAlwaysCapacity = kDefaultAlwaysCapacity) noexcept
    {
        m_lanes[static_cast<size_t>(Lane::kSimulation)].Capacity = aSimulationCapacity;
        m_lanes[static_cast<size_t>(Lane::kAlways)].Capacity = aAlwaysCapacity;
    }

    MainThreadQueue(const MainThreadQueue&) = delete;
    MainThreadQueue& operator=(const MainThreadQueue&) = delete;

    // ---- any thread ----

    // aPostedIn: the poster's Lifecycle::GetTokenAnyThread(). Returns false when the task was dropped
    // (lane full, out of memory, empty task); the drop is counted either way.
    bool Post(Lane aLane, Token aPostedIn, bool aDuringWorldLoad, Task aTask) noexcept
    {
        if (!aTask || aLane >= Lane::kCount)
            return false;

        LaneState& lane = m_lanes[static_cast<size_t>(aLane)];
        lane.Posted.fetch_add(1, std::memory_order_relaxed);
        if (aDuringWorldLoad)
            lane.PostedDuringWorldLoad.fetch_add(1, std::memory_order_relaxed);

        try
        {
            std::lock_guard<std::mutex> lock(lane.Mutex);
            if (lane.Pending.size() >= lane.Capacity)
            {
                lane.DroppedOverflow.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            lane.Pending.push_back(Entry{aPostedIn, std::move(aTask)});
            const uint64_t pending = lane.Pending.size();
            lane.PendingCount.store(pending, std::memory_order_relaxed);
            if (pending > lane.HighWater.load(std::memory_order_relaxed))
                lane.HighWater.store(pending, std::memory_order_relaxed);
        }
        catch (...)
        {
            lane.DroppedOverflow.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    // ---- main thread ----

    // Runs, in FIFO order, the tasks that were queued when the call started; tasks they post run on the
    // next drain. aIsCurrentSession(Token) -> bool is asked at execution time for every task posted
    // inside a session (Token::Session != 0). A nested Drain of the same lane (a task that pumps the
    // main loop) returns 0 instead of running the batch twice. Returns the number of tasks executed.
    template <class TIsCurrentSession> size_t Drain(Lane aLane, TIsCurrentSession&& aIsCurrentSession) noexcept
    {
        if (aLane >= Lane::kCount)
            return 0;

        LaneState& lane = m_lanes[static_cast<size_t>(aLane)];
        if (lane.Draining)
            return 0;
        lane.Draining = true;

        {
            std::lock_guard<std::mutex> lock(lane.Mutex);
            lane.Batch.swap(lane.Pending);  // Batch is empty here: it keeps its capacity between drains
            lane.PendingCount.store(0, std::memory_order_relaxed);
        }

        size_t executed = 0;
        for (Entry& entry : lane.Batch)
        {
            if (entry.PostedIn.Session != 0 && !aIsCurrentSession(entry.PostedIn))
            {
                lane.DroppedStale.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            try
            {
                entry.Work();
                ++executed;
            }
            catch (...)
            {
                lane.Failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        lane.Executed.fetch_add(executed, std::memory_order_relaxed);

        // Destroy the captures (service pointers, strings) before returning, still outside the lock.
        lane.Batch.clear();
        lane.Draining = false;
        return executed;
    }

    // Drops everything queued without running it (shutdown).
    void Clear() noexcept
    {
        for (LaneState& lane : m_lanes)
        {
            std::vector<Entry> discarded;
            {
                std::lock_guard<std::mutex> lock(lane.Mutex);
                discarded.swap(lane.Pending);
                lane.PendingCount.store(0, std::memory_order_relaxed);
            }
            // discarded dies here, outside the lock
        }
    }

    // ---- any thread (monotonic counters, individually consistent) ----

    [[nodiscard]] LaneStats GetStats(Lane aLane) const noexcept
    {
        LaneStats stats{};
        if (aLane >= Lane::kCount)
            return stats;
        const LaneState& lane = m_lanes[static_cast<size_t>(aLane)];
        stats.Posted = lane.Posted.load(std::memory_order_relaxed);
        stats.Executed = lane.Executed.load(std::memory_order_relaxed);
        stats.DroppedStale = lane.DroppedStale.load(std::memory_order_relaxed);
        stats.DroppedOverflow = lane.DroppedOverflow.load(std::memory_order_relaxed);
        stats.Failed = lane.Failed.load(std::memory_order_relaxed);
        stats.PostedDuringWorldLoad = lane.PostedDuringWorldLoad.load(std::memory_order_relaxed);
        stats.Pending = lane.PendingCount.load(std::memory_order_relaxed);
        stats.HighWater = lane.HighWater.load(std::memory_order_relaxed);
        return stats;
    }

    [[nodiscard]] size_t GetCapacity(Lane aLane) const noexcept
    {
        return aLane < Lane::kCount ? m_lanes[static_cast<size_t>(aLane)].Capacity : 0;
    }

private:
    struct Entry
    {
        Token PostedIn;
        Task Work;
    };

    struct LaneState
    {
        std::mutex Mutex;
        std::vector<Entry> Pending;  // guarded by Mutex
        std::vector<Entry> Batch;    // main thread only (Drain)
        bool Draining{false};        // main thread only
        size_t Capacity{0};          // set once in the constructor

        std::atomic<uint64_t> Posted{0};
        std::atomic<uint64_t> Executed{0};
        std::atomic<uint64_t> DroppedStale{0};
        std::atomic<uint64_t> DroppedOverflow{0};
        std::atomic<uint64_t> Failed{0};
        std::atomic<uint64_t> PostedDuringWorldLoad{0};
        std::atomic<uint64_t> PendingCount{0};
        std::atomic<uint64_t> HighWater{0};
    };

    std::array<LaneState, kLaneCount> m_lanes{};
};
} // namespace Session
