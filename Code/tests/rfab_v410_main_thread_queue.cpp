#include <catch2/catch.hpp>

#include <common/Session/MainThreadQueue.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// v4.1.0 architecture step 3: one main-thread inbox (common/Session/MainThreadQueue.h) replaces the
// TiltedPhoques::TaskQueue runner (ran tasks under its lock) and World's ad-hoc task vector (dropped
// silently past 4096). Behaviour is tested with real threads; the client wiring is audited at the end.
// The hook rules themselves are enforced by Tools/rfab/check_hook_discipline.py.

namespace
{
using namespace Session;

constexpr auto kAlwaysCurrent = [](Token) noexcept { return true; };

Token SessionToken(uint32_t aSession) noexcept
{
    return Token{aSession, 1};
}

std::filesystem::path FindQueueRepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "Session" / "MainThreadQueue.h"))
            return cur;
        if (!cur.has_parent_path() || cur.parent_path() == cur)
            break;
        cur = cur.parent_path();
    }
    return {};
}

std::string ReadQueueSource(const std::filesystem::path& acPath)
{
    std::ifstream in(acPath, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Body of the definition `acQualifiedName(...) ... { ... }`, or empty.
std::string QueueFunctionBody(const std::string& acSource, const std::string& acQualifiedName)
{
    size_t pos = 0;
    while ((pos = acSource.find(acQualifiedName + "(", pos)) != std::string::npos)
    {
        const size_t open = acSource.find('{', pos);
        const size_t semicolon = acSource.find(';', pos);
        const size_t lineStart = acSource.rfind('\n', pos);
        const std::string prefix = acSource.substr(lineStart == std::string::npos ? 0 : lineStart + 1,
                                                   pos - (lineStart == std::string::npos ? 0 : lineStart + 1));
        if (open != std::string::npos && (semicolon == std::string::npos || open < semicolon) && prefix.find('.') == std::string::npos &&
            prefix.find('=') == std::string::npos)
        {
            int depth = 0;
            for (size_t i = open; i < acSource.size(); ++i)
            {
                if (acSource[i] == '{')
                    ++depth;
                else if (acSource[i] == '}' && --depth == 0)
                    return acSource.substr(open, i - open + 1);
            }
            return {};
        }
        pos += acQualifiedName.size();
    }
    return {};
}
} // namespace

TEST_CASE("v4.1.0 main-thread queue: FIFO per lane, lanes drained independently", "[rfab][v410][mainthread]")
{
    MainThreadQueue queue;
    std::vector<int> ran;

    for (int i = 0; i < 5; ++i)
        REQUIRE(queue.Post(Lane::kSimulation, Token{}, false, [&ran, i]() { ran.push_back(i); }));
    REQUIRE(queue.Post(Lane::kAlways, Token{}, false, [&ran]() { ran.push_back(100); }));

    CHECK(queue.GetStats(Lane::kSimulation).Pending == 5);
    CHECK(queue.GetStats(Lane::kAlways).Pending == 1);

    // A paused menu drains only the always-lane: gameplay work waits for the next simulated frame.
    CHECK(queue.Drain(Lane::kAlways, kAlwaysCurrent) == 1);
    CHECK(ran == std::vector<int>{100});
    CHECK(queue.GetStats(Lane::kSimulation).Pending == 5);

    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 5);
    CHECK(ran == std::vector<int>{100, 0, 1, 2, 3, 4});

    const LaneStats sim = queue.GetStats(Lane::kSimulation);
    CHECK(sim.Posted == 5);
    CHECK(sim.Executed == 5);
    CHECK(sim.Pending == 0);
    CHECK(sim.HighWater == 5);
    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 0);
}

TEST_CASE("v4.1.0 main-thread queue: a task that posts runs on the next drain, never in the same batch", "[rfab][v410][mainthread]")
{
    MainThreadQueue queue;
    int generations = 0;

    std::function<void()> chain;
    chain = [&]() {
        ++generations;
        if (generations < 3)
            queue.Post(Lane::kSimulation, Token{}, false, chain);
    };
    queue.Post(Lane::kSimulation, Token{}, false, chain);

    // The old TaskQueue held its std::mutex while running tasks: this re-entrant post deadlocked.
    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 1);
    CHECK(generations == 1);
    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 1);
    CHECK(generations == 2);
    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 1);
    CHECK(generations == 3);
    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 0);
}

TEST_CASE("v4.1.0 main-thread queue: posting never waits for a running task", "[rfab][v410][mainthread][threads]")
{
    // A Havok worker posting a HitEvent must not block behind the main thread's batch. The task below
    // waits until another thread manages to post; with a queue that runs tasks under its lock this
    // would be a deadlock, here it must complete well within the timeout.
    MainThreadQueue queue;
    std::mutex mutex;
    std::condition_variable cv;
    bool posted = false;
    bool sawPost = false;

    queue.Post(Lane::kSimulation, Token{}, false, [&]() {
        std::thread producer([&]() {
            queue.Post(Lane::kSimulation, Token{}, false, []() {});
            std::lock_guard<std::mutex> lock(mutex);
            posted = true;
            cv.notify_all();
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            sawPost = cv.wait_for(lock, std::chrono::seconds(10), [&]() { return posted; });
        }
        producer.join();
    });

    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 1);
    CHECK(sawPost);
    CHECK(queue.GetStats(Lane::kSimulation).Pending == 1);
    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 1);
}

TEST_CASE("v4.1.0 main-thread queue: work posted inside a session dies with it", "[rfab][v410][mainthread][session]")
{
    Lifecycle lifecycle;
    MainThreadQueue queue;
    const auto isCurrent = [&](Token t) noexcept { return lifecycle.IsStillCurrentSession(t); };
    const auto post = [&](Lane aLane, std::vector<std::string>& aRan, std::string aName) {
        queue.Post(aLane, lifecycle.GetTokenAnyThread(), lifecycle.IsWorldLoadingAnyThread(),
                   [&aRan, name = std::move(aName)]() { aRan.push_back(name); });
    };

    std::vector<std::string> ran;

    SECTION("offline work always runs, also once a session began")
    {
        post(Lane::kSimulation, ran, "offline");
        lifecycle.OnTransportUp();
        lifecycle.OnAuthenticated(7);
        queue.Drain(Lane::kSimulation, isCurrent);
        CHECK(ran == std::vector<std::string>{"offline"});
        CHECK(queue.GetStats(Lane::kSimulation).DroppedStale == 0);
    }

    SECTION("a hit queued before the transport dropped does not run after it")
    {
        lifecycle.OnTransportUp();
        lifecycle.OnAuthenticated(7);
        post(Lane::kSimulation, ran, "hit in session 1");
        lifecycle.OnTransportDown();
        post(Lane::kSimulation, ran, "offline after");
        queue.Drain(Lane::kSimulation, isCurrent);
        CHECK(ran == std::vector<std::string>{"offline after"});
        CHECK(queue.GetStats(Lane::kSimulation).DroppedStale == 1);
    }

    SECTION("a reconnect while a menu paused the simulation: the old session's events are dropped")
    {
        lifecycle.OnTransportUp();
        lifecycle.OnAuthenticated(7);
        post(Lane::kSimulation, ran, "equip in session 1");
        lifecycle.OnTransportDown();
        lifecycle.OnTransportUp();
        lifecycle.OnAuthenticated(8);
        post(Lane::kSimulation, ran, "equip in session 2");
        queue.Drain(Lane::kSimulation, isCurrent);
        CHECK(ran == std::vector<std::string>{"equip in session 2"});
    }

    SECTION("re-authentication (End then Begin in one input) also invalidates")
    {
        lifecycle.OnTransportUp();
        lifecycle.OnAuthenticated(7);
        post(Lane::kSimulation, ran, "before reauth");
        lifecycle.OnAuthenticationReceived();
        lifecycle.OnAuthenticated(7);
        queue.Drain(Lane::kSimulation, isCurrent);
        CHECK(ran.empty());
        CHECK(queue.GetStats(Lane::kSimulation).DroppedStale == 1);
    }

    SECTION("a coordinated live reload keeps the session: its work survives and is counted as load-time work")
    {
        lifecycle.OnTransportUp();
        lifecycle.OnAuthenticated(7);
        lifecycle.OnWorldLoadBegin(true);
        post(Lane::kSimulation, ran, "during load");
        lifecycle.OnWorldLoadEnd(true);
        queue.Drain(Lane::kSimulation, isCurrent);
        CHECK(ran == std::vector<std::string>{"during load"});
        CHECK(queue.GetStats(Lane::kSimulation).PostedDuringWorldLoad == 1);
    }

    SECTION("work queued by a participant from OnSessionEnd is offline work and runs")
    {
        struct Poster final : IParticipant
        {
            std::function<void()> OnEnd;
            void OnSessionEnd(const Transition&) noexcept override { OnEnd(); }
        } poster;
        poster.OnEnd = [&]() { post(Lane::kSimulation, ran, "teardown follow-up"); };
        REQUIRE(lifecycle.Register(ParticipantId::kParty, poster));

        lifecycle.OnTransportUp();
        lifecycle.OnAuthenticated(7);
        lifecycle.OnTransportDown();
        queue.Drain(Lane::kSimulation, isCurrent);
        CHECK(ran == std::vector<std::string>{"teardown follow-up"});
    }
}

TEST_CASE("v4.1.0 main-thread queue: the staleness predicate is only asked for session work", "[rfab][v410][mainthread][session]")
{
    MainThreadQueue queue;
    int asked = 0;
    int ran = 0;
    queue.Post(Lane::kSimulation, Token{0, 5}, false, [&]() { ++ran; });
    queue.Post(Lane::kSimulation, SessionToken(3), false, [&]() { ++ran; });
    queue.Post(Lane::kSimulation, SessionToken(4), false, [&]() { ++ran; });

    queue.Drain(Lane::kSimulation, [&](Token t) noexcept {
        ++asked;
        return t.Session == 4;
    });
    CHECK(asked == 2);
    CHECK(ran == 2);
    CHECK(queue.GetStats(Lane::kSimulation).DroppedStale == 1);
    CHECK(queue.GetStats(Lane::kSimulation).Executed == 2);
}

TEST_CASE("v4.1.0 main-thread queue: a full lane drops and counts instead of growing or blocking", "[rfab][v410][mainthread]")
{
    MainThreadQueue queue(4, 2);
    CHECK(queue.GetCapacity(Lane::kSimulation) == 4);
    CHECK(queue.GetCapacity(Lane::kAlways) == 2);

    int ran = 0;
    for (int i = 0; i < 4; ++i)
        CHECK(queue.Post(Lane::kSimulation, Token{}, false, [&]() { ++ran; }));
    CHECK_FALSE(queue.Post(Lane::kSimulation, Token{}, false, [&]() { ++ran; }));
    CHECK_FALSE(queue.Post(Lane::kSimulation, Token{}, false, [&]() { ++ran; }));

    LaneStats stats = queue.GetStats(Lane::kSimulation);
    CHECK(stats.Posted == 6);
    CHECK(stats.DroppedOverflow == 2);
    CHECK(stats.Pending == 4);
    CHECK(stats.HighWater == 4);

    // The other lane has its own budget.
    CHECK(queue.Post(Lane::kAlways, Token{}, false, []() {}));

    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 4);
    CHECK(ran == 4);
    CHECK(queue.Post(Lane::kSimulation, Token{}, false, [&]() { ++ran; }));  // room again

    // Invalid input is refused without touching the counters.
    CHECK_FALSE(queue.Post(Lane::kSimulation, Token{}, false, MainThreadQueue::Task{}));
    CHECK_FALSE(queue.Post(Lane::kCount, Token{}, false, []() {}));
    CHECK(queue.GetStats(Lane::kSimulation).Posted == 7);
    CHECK(queue.Drain(Lane::kCount, kAlwaysCurrent) == 0);
    CHECK(queue.GetStats(Lane::kCount).Posted == 0);
}

TEST_CASE("v4.1.0 main-thread queue: a throwing task is counted and the batch goes on", "[rfab][v410][mainthread]")
{
    MainThreadQueue queue;
    std::vector<int> ran;
    queue.Post(Lane::kAlways, Token{}, false, [&]() { ran.push_back(1); });
    queue.Post(Lane::kAlways, Token{}, false, []() { throw std::runtime_error("boom"); });
    queue.Post(Lane::kAlways, Token{}, false, [&]() { ran.push_back(3); });

    CHECK(queue.Drain(Lane::kAlways, kAlwaysCurrent) == 2);
    CHECK(ran == std::vector<int>{1, 3});
    CHECK(queue.GetStats(Lane::kAlways).Failed == 1);
    CHECK(queue.GetStats(Lane::kAlways).Executed == 2);
}

TEST_CASE("v4.1.0 main-thread queue: a nested drain of the same lane does not run the batch twice", "[rfab][v410][mainthread]")
{
    MainThreadQueue queue;
    int outer = 0;
    int inner = 0;
    size_t nestedSim = 99;
    size_t nestedAlways = 99;

    queue.Post(Lane::kAlways, Token{}, false, [&]() { ++inner; });
    queue.Post(Lane::kSimulation, Token{}, false, [&]() {
        ++outer;
        // A task that pumps the main loop (OnMainThreadUpdate inside a frame) must not re-enter.
        nestedSim = queue.Drain(Lane::kSimulation, kAlwaysCurrent);
        nestedAlways = queue.Drain(Lane::kAlways, kAlwaysCurrent);
    });
    queue.Post(Lane::kSimulation, Token{}, false, [&]() { ++outer; });

    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 2);
    CHECK(outer == 2);
    CHECK(nestedSim == 0);
    CHECK(nestedAlways == 1);  // the other lane is independent
    CHECK(inner == 1);

    // Nothing of the batch may survive into the next drain (a nested swap would hand the batch being
    // iterated back to the pending list and run it twice).
    CHECK(queue.GetStats(Lane::kSimulation).Pending == 0);
    CHECK(queue.Drain(Lane::kSimulation, kAlwaysCurrent) == 0);
    CHECK(outer == 2);
}

TEST_CASE("v4.1.0 main-thread queue: captures are released after running and on Clear", "[rfab][v410][mainthread]")
{
    MainThreadQueue queue;
    auto payload = std::make_shared<int>(42);
    queue.Post(Lane::kSimulation, Token{}, false, [payload]() { (void)*payload; });
    queue.Post(Lane::kAlways, Token{}, false, [payload]() { (void)*payload; });
    queue.Post(Lane::kSimulation, SessionToken(9), false, [payload]() { (void)*payload; });
    CHECK(payload.use_count() == 4);

    queue.Drain(Lane::kSimulation, [](Token) noexcept { return false; });  // one run, one stale
    CHECK(payload.use_count() == 2);

    queue.Clear();
    CHECK(payload.use_count() == 1);
    CHECK(queue.GetStats(Lane::kAlways).Pending == 0);
    CHECK(queue.Drain(Lane::kAlways, kAlwaysCurrent) == 0);
}

TEST_CASE("v4.1.0 main-thread queue: many engine threads, one main thread, nothing lost or reordered", "[rfab][v410][mainthread][threads]")
{
    constexpr int kProducers = 6;
    constexpr int kPerProducer = 4000;
    MainThreadQueue queue;

    std::vector<std::vector<int>> seen(kProducers);
    std::atomic<int> started{0};
    std::atomic<bool> done{false};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&, p]() {
            started.fetch_add(1);
            while (started.load() < kProducers)
                std::this_thread::yield();
            for (int i = 0; i < kPerProducer; ++i)
            {
                const Lane lane = (i % 3 == 0) ? Lane::kAlways : Lane::kSimulation;
                // seen[p] is only touched by the main thread (inside Drain)
                while (!queue.Post(lane, Token{}, false, [&seen, p, i]() { seen[p].push_back(i); }))
                    std::this_thread::yield();
            }
        });
    }

    std::thread joiner([&]() {
        for (auto& t : producers)
            t.join();
        done.store(true);
    });

    // The main thread: drain both lanes in a loop while producers post.
    size_t executed = 0;
    while (!done.load())
    {
        executed += queue.Drain(Lane::kAlways, kAlwaysCurrent);
        executed += queue.Drain(Lane::kSimulation, kAlwaysCurrent);
    }
    executed += queue.Drain(Lane::kAlways, kAlwaysCurrent);
    executed += queue.Drain(Lane::kSimulation, kAlwaysCurrent);
    joiner.join();

    CHECK(executed == static_cast<size_t>(kProducers) * kPerProducer);
    for (int p = 0; p < kProducers; ++p)
    {
        INFO("producer " << p);
        REQUIRE(seen[p].size() == static_cast<size_t>(kPerProducer));
        // Within one lane a producer's order is kept; across lanes it is not promised. Check per lane.
        int lastAlways = -1;
        int lastSim = -1;
        bool ordered = true;
        for (int v : seen[p])
        {
            int& last = (v % 3 == 0) ? lastAlways : lastSim;
            ordered = ordered && v > last;
            last = v;
        }
        CHECK(ordered);
    }

    const LaneStats sim = queue.GetStats(Lane::kSimulation);
    const LaneStats always = queue.GetStats(Lane::kAlways);
    CHECK(sim.Posted == sim.Executed);
    CHECK(always.Posted == always.Executed);
    CHECK(sim.DroppedOverflow == 0);
    CHECK(always.DroppedOverflow == 0);
}

TEST_CASE("v4.1.0 main-thread queue: the client drives both lanes from the right places", "[rfab][v410][mainthread][audit]")
{
    const auto root = FindQueueRepoRoot();
    if (root.empty())
    {
        WARN("repository root not found from the test working directory; skipping source wiring audit");
        return;
    }

    const std::string world = ReadQueueSource(root / "Code" / "client" / "World.cpp");
    const std::string worldH = ReadQueueSource(root / "Code" / "client" / "World.h");
    const std::string runner = ReadQueueSource(root / "Code" / "client" / "Services" / "Generic" / "RunnerService.cpp");
    const std::string runnerH = ReadQueueSource(root / "Code" / "client" / "Services" / "RunnerService.h");
    REQUIRE_FALSE(world.empty());
    REQUIRE_FALSE(runner.empty());

    // One queue, owned by World, declared before the runner that references it.
    const size_t queueMember = worldH.find("Session::MainThreadQueue m_mainThreadQueue;");
    const size_t runnerMember = worldH.find("RunnerService m_runner;");
    REQUIRE(queueMember != std::string::npos);
    REQUIRE(runnerMember != std::string::npos);
    CHECK(queueMember < runnerMember);
    CHECK(worldH.find("m_mainThreadTasks") == std::string::npos);
    CHECK(runnerH.find("TaskQueue") == std::string::npos);
    CHECK(world.find("m_runner(m_dispatcher, m_mainThreadQueue, m_lifecycle)") != std::string::npos);

    // Simulated frame: snapshot, then the simulation lane, before the services update.
    const std::string update = QueueFunctionBody(world, "void World::Update");
    REQUIRE_FALSE(update.empty());
    const size_t snapshot = update.find("PublishThreadSnapshot();");
    const size_t drain = update.find("m_runner.OnUpdate(");
    const size_t services = update.find("m_dispatcher.trigger(UpdateEvent(");
    REQUIRE(snapshot != std::string::npos);
    REQUIRE(drain != std::string::npos);
    REQUIRE(services != std::string::npos);
    CHECK(snapshot < drain);
    CHECK(drain < services);

    // Paused pump: the same snapshot, no gameplay lane.
    const std::string paused = QueueFunctionBody(world, "void World::UpdatePausedPump");
    REQUIRE_FALSE(paused.empty());
    CHECK(paused.find("PublishThreadSnapshot();") != std::string::npos);
    CHECK(paused.find("m_runner.OnUpdate") == std::string::npos);
    CHECK(paused.find("Lane::kSimulation") == std::string::npos);
    CHECK(paused.find("s_isConnectedCached.store") == std::string::npos);  // published in one place only

    // Every main-loop iteration: the always-lane.
    const std::string onMain = QueueFunctionBody(world, "void World::OnMainThreadUpdate");
    REQUIRE_FALSE(onMain.empty());
    CHECK(onMain.find("Drain(Session::Lane::kAlways") != std::string::npos);

    // Session scoping of gameplay work: the runner posts with the lifecycle token and checks it.
    CHECK(runner.find("GetTokenAnyThread()") != std::string::npos);
    CHECK(runner.find("IsStillCurrentSession(") != std::string::npos);
    CHECK(runner.find("Lane::kSimulation") != std::string::npos);
}
