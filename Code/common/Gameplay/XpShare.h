#pragma once

#include <cstdint>
#include <vector>

// Server side bookkeeping for shared experience.
//
// Design constraints (RFAB Online):
//   * every player on the server must end up with the same experience for the
//     same in-game event;
//   * a reconnect, a duplicated packet or a resent report must never award the
//     same experience twice;
//   * the server must stay authoritative so that a single client cannot DDoS
//     everybody else's level with a bogus huge number.
//
// The RFAB build computes XP in Papyrus (RFAB_XP_Handler) and stores it in a
// global; there is no per-event id on the wire. Two layers of protection are
// therefore used:
//
//   1. Per-player monotonic sequence numbers (exact - survives reconnects and
//      duplicated packets).
//   2. A short "same amount, same moment" window across players. When the game
//      syncs one kill/quest to several clients, every one of them runs its own
//      XP handler and reports the same amount within a few hundred ms; without
//      this filter every player would receive N copies of the same award.
//      This layer is a heuristic and is documented as such.
//
// Everything here is dependency free and unit tested on the host
// (Code/tests/pure/, tools/run_pure_tests.sh).
namespace Gameplay::XpShare
{
inline constexpr uint32_t kMaxSingleReport = 100'000;
inline constexpr uint64_t kDuplicateWindowMs = 5'000;
inline constexpr uint32_t kMaxTrackedAmounts = 64;

struct ReportDecision
{
    bool Accept{false};    // report is well formed and not a replay
    bool Broadcast{false}; // other players should receive this amount
    uint32_t Amount{0};    // sanitised amount to broadcast
};

/**
 * @brief Per-player report tracker: rejects replays and out-of-order reports.
 *
 * Sequence numbers are produced by the client and restart at 0 after a
 * reconnect, so a sequence of 0 always starts a new session and is accepted.
 */
class SequenceTracker
{
public:
    void Reset() noexcept
    {
        m_hasSequence = false;
        m_lastSequence = 0;
    }

    /**
     * @return true when the report is new (not a replay / not out of order).
     */
    bool Accept(uint32_t aSequence) noexcept
    {
        // A client that reconnected starts counting from zero again.
        if (aSequence == 0)
        {
            m_hasSequence = false;
        }

        if (!m_hasSequence)
        {
            m_hasSequence = true;
            m_lastSequence = aSequence;
            return true;
        }

        if (aSequence <= m_lastSequence)
            return false;

        m_lastSequence = aSequence;
        return true;
    }

    [[nodiscard]] bool HasSequence() const noexcept { return m_hasSequence; }
    [[nodiscard]] uint32_t LastSequence() const noexcept { return m_lastSequence; }

private:
    bool m_hasSequence{false};
    uint32_t m_lastSequence{0};
};

/**
 * @brief Cross-player "same amount within a window" filter.
 *
 * Not a replacement for real event ids - it only removes the most common
 * duplication (the game telling every client about the same kill/quest).
 */
class AmountWindow
{
public:
    explicit AmountWindow(uint64_t aWindowMs = kDuplicateWindowMs) noexcept
        : m_windowMs(aWindowMs)
    {
    }

    /**
     * @return true when this amount should be broadcast, i.e. no player reported
     *         the same amount inside the window.
     */
    bool ShouldBroadcast(uint32_t aAmount, uint64_t aNowMs) noexcept
    {
        Purge(aNowMs);

        for (const auto& entry : m_entries)
        {
            if (entry.Amount == aAmount)
                return false;
        }

        if (m_entries.size() >= kMaxTrackedAmounts)
            m_entries.erase(m_entries.begin());

        m_entries.push_back({aAmount, aNowMs});
        return true;
    }

    void Reset() noexcept { m_entries.clear(); }

    void SetWindow(uint64_t aWindowMs) noexcept { m_windowMs = aWindowMs; }

    [[nodiscard]] size_t Size() const noexcept { return m_entries.size(); }

private:
    struct Entry
    {
        uint32_t Amount;
        uint64_t TimestampMs;
    };

    void Purge(uint64_t aNowMs) noexcept
    {
        // Timestamps are monotonic within a server run; a smaller "now" than a
        // stored entry means the clock source restarted, so drop everything.
        size_t keep = 0;
        for (size_t i = 0; i < m_entries.size(); ++i)
        {
            const auto& entry = m_entries[i];
            if (entry.TimestampMs > aNowMs)
            {
                m_entries.clear();
                return;
            }

            if (aNowMs - entry.TimestampMs <= m_windowMs)
            {
                if (keep != i)
                    m_entries[keep] = entry;
                ++keep;
            }
        }

        m_entries.resize(keep);
    }

    uint64_t m_windowMs;
    std::vector<Entry> m_entries{};
};

/**
 * @brief Client side "I already awarded this myself" filter.
 *
 * When several clients run the same RFAB event (a quest completion is evaluated
 * by every client, a book read is local, a kill is only simulated by the cell
 * owner), each of them awards the XP locally *and* reports it. The server fans
 * out the first report to everybody except the reporter, so a client that also
 * ran the event locally would end up with the XP twice.
 *
 * The filter closes that hole: an award that was applied locally a moment ago is
 * not applied again when it arrives from the network. The window is deliberately
 * much shorter than the server's dedup window - the same quest finishes on all
 * clients within a second, whereas two players killing equally strong monsters a
 * few seconds apart is legitimate and must not be swallowed.
 */
class EchoFilter
{
public:
    static constexpr uint64_t kEchoWindowMs = 1500;
    static constexpr size_t kMaxEntries = 32;

    /** Remember an award that this client just applied locally. */
    void Record(uint32_t aAmount, uint64_t aNowMs) noexcept
    {
        if (aAmount == 0)
            return;

        if (m_entries.size() >= kMaxEntries)
            m_entries.erase(m_entries.begin());

        m_entries.push_back({aAmount, aNowMs});
    }

    /**
     * @return true when the incoming award was already applied locally and must
     *         be ignored (the local total is then already correct).
     */
    bool ShouldSuppress(uint32_t aAmount, uint64_t aNowMs) noexcept
    {
        if (aAmount == 0)
            return true;

        size_t keep = 0;
        bool found = false;

        for (size_t i = 0; i < m_entries.size(); ++i)
        {
            const auto& entry = m_entries[i];

            // A clock that went backwards means "restart": drop the history.
            if (entry.TimestampMs > aNowMs)
                continue;

            const uint64_t age = aNowMs - entry.TimestampMs;
            if (age > kEchoWindowMs)
                continue;

            if (entry.Amount == aAmount)
            {
                found = true;
                continue; // consume the entry
            }

            if (keep != i)
                m_entries[keep] = entry;
            ++keep;
        }

        m_entries.resize(keep);
        return found;
    }

    void Reset() noexcept { m_entries.clear(); }

    [[nodiscard]] size_t Size() const noexcept { return m_entries.size(); }

private:
    struct Entry
    {
        uint32_t Amount;
        uint64_t TimestampMs;
    };

    std::vector<Entry> m_entries{};
};

/**
 * @brief Sanitises an incoming amount.
 */
inline uint32_t SanitiseAmount(uint32_t aAmount) noexcept
{
    return aAmount > kMaxSingleReport ? kMaxSingleReport : aAmount;
}

} // namespace Gameplay::XpShare
