#pragma once
#include <cstdint>

namespace Coop
{
// A native operation returning successfully is NOT a readiness receipt. Require a new,
// uninterrupted observation window after the native recovery grace has elapsed. The caller
// supplies raw engine/graph/health facts, never a network Presence with bleedout suppression.
class ReadinessGate
{
public:
    enum class Result { Waiting, Ready, Failed };
    void Begin(uint64_t aNow, uint32_t aGraceMs = 2000, uint32_t aDeadlineMs = 12000) noexcept
    {
        m_notBefore = aNow + aGraceMs;
        m_deadline = aNow + aDeadlineMs;
        m_since = 0;
        m_last = aNow;
        m_result = Result::Waiting;
    }
    Result Poll(uint64_t aNow, bool aRawReady) noexcept
    {
        if (m_result != Result::Waiting)
            return m_result;
        if (aNow < m_last || aNow >= m_deadline)
            return m_result = Result::Failed;
        // A long gap (menu/load/stalled game loop) is not an uninterrupted ready dwell.
        if (aNow - m_last > 250 || !aRawReady || aNow < m_notBefore)
            m_since = 0;
        m_last = aNow;
        if (!aRawReady || aNow < m_notBefore)
            return m_result;
        if (!m_since)
            m_since = aNow;
        if (aNow - m_since >= 500)
            m_result = Result::Ready;
        return m_result;
    }
private:
    uint64_t m_notBefore{}, m_deadline{}, m_since{}, m_last{};
    Result m_result{Result::Failed};
};
} // namespace Coop
