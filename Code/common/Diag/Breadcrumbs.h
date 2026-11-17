#pragma once

/**
 * Breadcrumbs (work plan, package 3.4): what the mod was doing just before a crash.
 *
 * A crash record has an address and a stack. The stack says where the game died; it does not say
 * that 40 ms earlier a removal for the same actor was applied, or that the last hook to run was the
 * inventory one. A fixed ring of the most recent events, written into the crash record, does.
 *
 * Rules of the ring:
 *  - `Note` takes a STRING LITERAL (or any pointer with static storage). The crash writer reads the
 *    name long after the call returned, possibly from another thread.
 *  - The same event repeated (same name, same first value, same thread) is one row with a count, so
 *    a hook that runs for every actor every frame does not push everything else out.
 *  - Writers take a spin lock for a few instructions and give up after a bounded number of tries:
 *    a hook must never wait on a diagnostic. A crumb lost that way is counted (`Dropped`).
 *  - The reader (`Snapshot`, `Format`) takes no lock: it runs inside a crash handler, where the
 *    thread that holds the lock may be suspended for good. A row being written while it is read can
 *    come out mixed (name of one event, values of another); that is accepted, and the name is still
 *    a valid literal.
 *  - Nothing allocates, throws or calls the OS. Time and thread id are passed in by the caller.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Diag
{

struct Crumb
{
    const char* Name{nullptr};
    uint64_t TimeMs{0};  // time of the last occurrence
    uint32_t A{0};
    uint32_t B{0};       // of the last occurrence
    uint32_t Thread{0};
    uint32_t Count{0};
};

template <size_t Capacity> class BreadcrumbRing
{
public:
    static_assert(Capacity >= 4, "a ring this small records nothing useful");

    constexpr BreadcrumbRing() noexcept = default;

    void Note(const char* apLiteral, uint32_t aA, uint32_t aB, uint64_t aNowMs, uint32_t aThread) noexcept
    {
        for (uint32_t tries = 0; m_busy.test_and_set(std::memory_order_acquire); ++tries)
        {
            if (tries >= kMaxSpins)
            {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }

        Crumb& last = m_ring[(m_next + Capacity - 1) % Capacity];
        if (m_total != 0 && last.Name == apLiteral && last.A == aA && last.Thread == aThread)
        {
            ++last.Count;
            last.B = aB;
            last.TimeMs = aNowMs;
        }
        else
        {
            Crumb& slot = m_ring[m_next];
            slot.Name = apLiteral;
            slot.TimeMs = aNowMs;
            slot.A = aA;
            slot.B = aB;
            slot.Thread = aThread;
            slot.Count = 1;
            m_next = (m_next + 1) % Capacity;
            ++m_total;
        }

        m_busy.clear(std::memory_order_release);
    }

    // Copies the newest `aMax` rows, oldest first. Lock-free on purpose (see the header comment).
    size_t Snapshot(Crumb* apOut, size_t aMax) const noexcept
    {
        const size_t next = m_next % Capacity;
        const size_t available = Available(aMax);
        for (size_t i = 0; i < available; ++i)
            apOut[i] = m_ring[(next + Capacity - available + i) % Capacity];
        return available;
    }

    /**
     * Writes the newest `aMaxCrumbs` rows as text, oldest first, one per line:
     *     "\r\n    [crumb <label> -<age>ms T<thread>] <name> a=0x<A> b=0x<B> x<count>"
     * Returns the number of characters written (never more than aSize - 1); the text is always
     * terminated. When the buffer is too small the OLDEST rows are left out: the newest ones are
     * what a crash report is read for. A row is never cut in the middle. Uses about 200 bytes of
     * stack: it is called from a crash handler, possibly after a stack overflow.
     */
    size_t Format(char* apBuffer, size_t aSize, const char* apLabel, uint64_t aNowMs, size_t aMaxCrumbs) const noexcept
    {
        if (!apBuffer || aSize == 0)
            return 0;
        apBuffer[0] = '\0';

        const size_t next = m_next % Capacity;
        const size_t count = Available(aMaxCrumbs);
        char line[176];

        // Pass 1, newest to oldest: how many rows fit.
        size_t fitting = 0;
        size_t needed = 1;  // the terminator
        for (; fitting < count; ++fitting)
        {
            const size_t length = FormatRow(line, sizeof(line), m_ring[(next + Capacity - 1 - fitting) % Capacity], apLabel, aNowMs);
            if (needed + length > aSize)
                break;
            needed += length;
        }

        // Pass 2, oldest to newest. Other threads keep writing, so a row may have changed since
        // pass 1: the bound is checked again.
        size_t written = 0;
        for (size_t i = 0; i < fitting; ++i)
        {
            const size_t length = FormatRow(line, sizeof(line), m_ring[(next + Capacity - fitting + i) % Capacity], apLabel, aNowMs);
            if (written + length + 1 > aSize)
                break;
            for (size_t c = 0; c < length; ++c)
                apBuffer[written++] = line[c];
            apBuffer[written] = '\0';
        }
        return written;
    }

    // Crumbs given up because another thread held the ring for too long.
    [[nodiscard]] uint64_t Dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

    [[nodiscard]] uint64_t Total() const noexcept { return m_total; }

private:
    // A writer holds the ring for a dozen instructions; this many tries is far more than that takes
    // and far less than a frame.
    static constexpr uint32_t kMaxSpins = 20000;

    // Length of the row's text in apLine; 0 for an empty slot or a row that does not fit the line.
    static size_t FormatRow(char* apLine, size_t aLineSize, Crumb aRow, const char* apLabel, uint64_t aNowMs) noexcept
    {
        if (!aRow.Name)
            return 0;
        const uint64_t age = aNowMs >= aRow.TimeMs ? aNowMs - aRow.TimeMs : 0;
        const int length = std::snprintf(apLine, aLineSize, "\r\n    [crumb %.8s -%llums T%lu] %.64s a=0x%lX b=0x%lX x%lu",
                                         apLabel ? apLabel : "", static_cast<unsigned long long>(age),
                                         static_cast<unsigned long>(aRow.Thread), aRow.Name, static_cast<unsigned long>(aRow.A),
                                         static_cast<unsigned long>(aRow.B), static_cast<unsigned long>(aRow.Count));
        return (length > 0 && static_cast<size_t>(length) < aLineSize) ? static_cast<size_t>(length) : 0;
    }

    [[nodiscard]] size_t Available(size_t aMax) const noexcept
    {
        const uint64_t total = m_total;
        const size_t available = total < Capacity ? static_cast<size_t>(total) : Capacity;
        return available < aMax ? available : aMax;
    }

    Crumb m_ring[Capacity]{};
    size_t m_next{0};
    uint64_t m_total{0};
    std::atomic<uint64_t> m_dropped{0};
    std::atomic_flag m_busy = ATOMIC_FLAG_INIT;
};

// Three rings, by how often their events happen, so that the frequent ones cannot push out the
// rare ones: hooks run thousands of times a second, packets tens of times, and a cell change or a
// removed character - often the actual cause - may be many seconds old when the game dies.
inline BreadcrumbRing<48> g_hookCrumbs{};    // game hooks
inline BreadcrumbRing<64> g_packetCrumbs{};  // packets in and out
inline BreadcrumbRing<96> g_eventCrumbs{};   // session, world load, cell change, remote actors

} // namespace Diag
