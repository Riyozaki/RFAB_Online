// Work plan 3.4: breadcrumbs written into the crash record (common/Diag/Breadcrumbs.h).
#include <catch2/catch.hpp>

#include <common/Diag/Breadcrumbs.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::string FormatAll(const Diag::BreadcrumbRing<8>& acRing, uint64_t aNowMs, size_t aMax = 8, size_t aBufferSize = 4096)
{
    std::vector<char> buffer(aBufferSize, 'Z');
    const size_t written = acRing.Format(buffer.data(), buffer.size(), "hook", aNowMs, aMax);
    REQUIRE(written < buffer.size());
    REQUIRE(buffer[written] == '\0');
    return std::string(buffer.data(), written);
}

size_t CountOf(const std::string& acText, const char* acNeedle)
{
    size_t count = 0;
    for (size_t at = acText.find(acNeedle); at != std::string::npos; at = acText.find(acNeedle, at + 1))
        ++count;
    return count;
}
} // namespace

TEST_CASE("Breadcrumbs: the ring keeps the newest events, oldest first", "[rfab][v411][breadcrumbs]")
{
    Diag::BreadcrumbRing<8> ring;

    SECTION("empty: nothing is written and the text is terminated")
    {
        CHECK(FormatAll(ring, 1000).empty());
        Diag::Crumb rows[8];
        CHECK(ring.Snapshot(rows, 8) == 0);
        CHECK(ring.Total() == 0);
    }

    SECTION("one event: its name, values, age and thread")
    {
        ring.Note("HookDamageActor", 0xFF000A12u, 0x14u, 990, 4242);
        const std::string text = FormatAll(ring, 1000);
        CHECK(text == "\r\n    [crumb hook -10ms T4242] HookDamageActor a=0xFF000A12 b=0x14 x1");
    }

    SECTION("more events than the ring holds: the oldest are gone, order is kept")
    {
        static const char* const kNames[] = {"e0", "e1", "e2", "e3", "e4", "e5", "e6", "e7", "e8", "e9", "e10", "e11"};
        for (uint32_t i = 0; i < 12; ++i)
            ring.Note(kNames[i], i, 0, 100 + i, 1);

        Diag::Crumb rows[8];
        REQUIRE(ring.Snapshot(rows, 8) == 8);
        for (uint32_t i = 0; i < 8; ++i)
        {
            CHECK(rows[i].A == i + 4);
            CHECK(std::strcmp(rows[i].Name, kNames[i + 4]) == 0);
        }
        CHECK(ring.Total() == 12);

        // Asking for fewer gives the newest ones.
        REQUIRE(ring.Snapshot(rows, 3) == 3);
        CHECK(rows[0].A == 9);
        CHECK(rows[2].A == 11);
    }

    SECTION("the same event repeated is one row with a count: a per-frame hook cannot flush the ring")
    {
        ring.Note("remove-character", 0x1A3, 0, 100, 1);
        for (uint32_t i = 0; i < 5000; ++i)
            ring.Note("HookActorProcess", 0, 0xFF000000u + i, 200 + i, 1);
        ring.Note("HookDamageActor", 7, 0, 6000, 1);

        Diag::Crumb rows[8];
        REQUIRE(ring.Snapshot(rows, 8) == 3);
        CHECK(std::strcmp(rows[0].Name, "remove-character") == 0);
        CHECK(rows[1].Count == 5000);
        CHECK(rows[1].B == 0xFF000000u + 4999);  // the last occurrence
        CHECK(rows[1].TimeMs == 200 + 4999);
        CHECK(std::strcmp(rows[2].Name, "HookDamageActor") == 0);
    }

    SECTION("a different subject or a different thread is a different row")
    {
        ring.Note("HookDamageActor", 1, 0, 100, 1);
        ring.Note("HookDamageActor", 2, 0, 100, 1);
        ring.Note("HookDamageActor", 2, 0, 100, 2);
        Diag::Crumb rows[8];
        CHECK(ring.Snapshot(rows, 8) == 3);
    }

    SECTION("a clock that went backwards gives age 0, not a huge number")
    {
        ring.Note("packet-in", 3, 40, 5000, 1);
        CHECK(FormatAll(ring, 4000).find("-0ms") != std::string::npos);
    }
}

TEST_CASE("Breadcrumbs: the crash writer's text never overruns its buffer", "[rfab][v411][breadcrumbs]")
{
    Diag::BreadcrumbRing<8> ring;
    static const char* const kNames[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"};
    for (uint32_t i = 0; i < 8; ++i)
        ring.Note(kNames[i], i, i, 100, 77);

    const std::string full = FormatAll(ring, 200);
    REQUIRE(CountOf(full, "[crumb hook") == 8);

    SECTION("every buffer size gives whole rows, a terminator and nothing past the end; the NEWEST rows survive")
    {
        size_t previousRows = 0;
        for (size_t size = 1; size <= full.size() + 2; ++size)
        {
            std::vector<char> buffer(size + 8, 'Z');
            const size_t written = ring.Format(buffer.data(), size, "hook", 200, 8);
            INFO("buffer size " << size);
            REQUIRE(written < size);
            CHECK(buffer[written] == '\0');
            for (size_t guard = size; guard < size + 8; ++guard)
                REQUIRE(buffer[guard] == 'Z');

            const std::string text(buffer.data(), written);
            // The end of the full text: a small buffer loses the oldest rows, never the newest.
            REQUIRE(text.size() <= full.size());
            CHECK(full.compare(full.size() - text.size(), text.size(), text) == 0);
            CHECK((text.empty() || text.compare(0, 2, "\r\n") == 0));  // starts at a row boundary
            const size_t rows = CountOf(text, "[crumb hook");
            CHECK(rows == CountOf(text, " x1"));
            CHECK(rows >= previousRows);                                // more room never gives fewer rows
            previousRows = rows;
            if (!text.empty())
                CHECK(text.find("theta") != std::string::npos);         // the newest event is always there
        }
        CHECK(previousRows == 8);
    }

    SECTION("no buffer: nothing happens")
    {
        CHECK(ring.Format(nullptr, 100, "hook", 200, 8) == 0);
        char one = 'Z';
        CHECK(ring.Format(&one, 0, "hook", 200, 8) == 0);
        CHECK(one == 'Z');
    }

    SECTION("a long name and a long label are cut, not overrun")
    {
        Diag::BreadcrumbRing<8> other;
        other.Note("a-name-that-is-much-longer-than-anything-a-call-site-would-pass-and-keeps-going-and-going-until-it-is-cut", 1, 2, 100, 3);
        std::vector<char> buffer(512, 'Z');
        const size_t written = other.Format(buffer.data(), buffer.size(), "a-very-long-label", 200, 8);
        const std::string text(buffer.data(), written);
        CHECK(text.find("[crumb a-very-l -100ms T3]") != std::string::npos);
        CHECK(text.find("a=0x1 b=0x2 x1") != std::string::npos);
        CHECK(text.size() < 176);
    }
}

TEST_CASE("Breadcrumbs: hooks on several threads never lose or tear an event", "[rfab][v411][breadcrumbs]")
{
    Diag::BreadcrumbRing<8> ring;
    static const char* const kNames[] = {"thread-0", "thread-1", "thread-2", "thread-3"};
    constexpr uint32_t kPerThread = 20000;

    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < 4; ++t)
    {
        threads.emplace_back([&, t] {
            while (!go.load())
            {
            }
            for (uint32_t i = 0; i < kPerThread; ++i)
                ring.Note(kNames[t], t, i, i, t + 1);
        });
    }
    go.store(true);
    for (auto& thread : threads)
        thread.join();

    // Every call is either a new row or a repeat of the newest one.
    Diag::Crumb rows[8];
    const size_t count = ring.Snapshot(rows, 8);
    REQUIRE(count >= 1);
    for (size_t i = 0; i < count; ++i)
    {
        REQUIRE(rows[i].Name != nullptr);
        const uint32_t owner = rows[i].A;
        REQUIRE(owner < 4);
        CHECK(rows[i].Name == kNames[owner]);     // name and values belong to the same event
        CHECK(rows[i].Thread == owner + 1);
        CHECK(rows[i].Count >= 1);
    }
    CHECK(ring.Total() >= 1);
    // Every call is a new row, a repeat of the newest row, or - when another thread held the ring
    // for too long - a counted drop. A hook never waits without bound.
    CHECK(ring.Total() + ring.Dropped() <= 4ull * kPerThread);

    // The three rings the client uses exist and are independent: packets cannot push out events.
    const uint64_t hooksBefore = Diag::g_hookCrumbs.Total();
    const uint64_t eventsBefore = Diag::g_eventCrumbs.Total();
    Diag::g_eventCrumbs.Note("test-event", 1, 2, 3, 4);
    for (uint32_t i = 0; i < 500; ++i)
        Diag::g_packetCrumbs.Note(i % 2 ? "packet-in" : "packet-out", i, 0, i, 4);
    CHECK(Diag::g_hookCrumbs.Total() == hooksBefore);
    CHECK(Diag::g_eventCrumbs.Total() == eventsBefore + 1);
    Diag::Crumb newest[1];
    REQUIRE(Diag::g_eventCrumbs.Snapshot(newest, 1) == 1);
    CHECK(std::string(newest[0].Name) == "test-event");
}
