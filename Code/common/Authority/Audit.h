#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Authority
{

//! @brief Things the server refused to do, and why.
//!
//! Every one of these is the server telling a client "no". In a working session they are
//! rare and harmless (a packet arrived twice, a reading was taken before a hit landed).
//! A flood of them is the signature of a real problem, and before this existed the only
//! evidence a tester could bring back was "combat felt wrong".
enum class AuditEvent : uint8_t
{
    kRejectedHit = 0,     //!< damage from a player who was not allowed to deal it
    kDuplicateHit,        //!< the same swing arrived more than once
    kStaleSnapshot,       //!< an owner's reading would have handed back health already taken off
    kSnapshotCorrection,  //!< that owner was told the authoritative value
    kDeniedOwnership,     //!< a claim/handoff the lease rules refused
    kCount
};

//! @brief Counts refusals and decides when they are worth saying out loud.
//!
//! The decision of *when to log* is here, not at the call sites, because it is the part
//! that can be got wrong in a way nobody notices: too chatty and the log is useless
//! during a fight, too quiet and a session produces no evidence at all. Both failure
//! modes are covered by the loop.
class CombatAudit
{
public:
    //! A quiet summary cadence: often enough to place an event inside a fight, rare
    //! enough that a busy server does not drown its own log.
    static constexpr uint64_t kReportIntervalMs = 60000;

    void Record(AuditEvent aEvent) noexcept
    {
        const auto index = static_cast<std::size_t>(aEvent);
        if (index >= static_cast<std::size_t>(AuditEvent::kCount))
            return;

        ++m_window[index];
        ++m_total[index];
    }

    //! @brief Should the caller print a summary now?
    //!
    //! True the first time a kind of refusal happens at all - a tester should see it
    //! while it is still obvious what they just did - and then at most once per
    //! interval, and never when nothing has happened since the last summary.
    [[nodiscard]] bool ShouldReport(uint64_t aNowMs) noexcept
    {
        if (!HasPending())
            return false;

        if (HasUnreportedKind())
        {
            MarkReported(aNowMs);
            return true;
        }

        // A clock that jumps backwards (or a first call) must not lock reporting out.
        if (m_lastReportMs == 0 || aNowMs < m_lastReportMs)
        {
            MarkReported(aNowMs);
            return true;
        }

        if ((aNowMs - m_lastReportMs) < kReportIntervalMs)
            return false;

        MarkReported(aNowMs);
        return true;
    }

    //! Counts since the previous summary. Valid until the next ShouldReport().
    [[nodiscard]] uint32_t WindowCount(AuditEvent aEvent) const noexcept
    {
        const auto index = static_cast<std::size_t>(aEvent);
        return index < m_reported.size() ? m_reported[index] : 0u;
    }

    //! Counts since the server started - what an operator wants when asking after the fact.
    [[nodiscard]] uint32_t TotalCount(AuditEvent aEvent) const noexcept
    {
        const auto index = static_cast<std::size_t>(aEvent);
        return index < m_total.size() ? m_total[index] : 0u;
    }

    [[nodiscard]] bool AnythingRecorded() const noexcept
    {
        for (const auto count : m_total)
        {
            if (count != 0u)
                return true;
        }
        return false;
    }

    void Clear() noexcept
    {
        m_window = {};
        m_reported = {};
        m_total = {};
        m_seen = {};
        m_lastReportMs = 0;
    }

private:
    [[nodiscard]] bool HasPending() const noexcept
    {
        for (const auto count : m_window)
        {
            if (count != 0u)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool HasUnreportedKind() const noexcept
    {
        for (std::size_t i = 0; i < m_window.size(); ++i)
        {
            if (m_window[i] != 0u && !m_seen[i])
                return true;
        }
        return false;
    }

    void MarkReported(uint64_t aNowMs) noexcept
    {
        for (std::size_t i = 0; i < m_window.size(); ++i)
        {
            if (m_window[i] != 0u)
                m_seen[i] = true;
        }

        m_reported = m_window;
        m_window = {};
        m_lastReportMs = aNowMs;
    }

    static constexpr std::size_t kKinds = static_cast<std::size_t>(AuditEvent::kCount);

    std::array<uint32_t, kKinds> m_window{};
    std::array<uint32_t, kKinds> m_reported{};
    std::array<uint32_t, kKinds> m_total{};
    std::array<bool, kKinds> m_seen{};
    uint64_t m_lastReportMs{0};
};

} // namespace Authority
