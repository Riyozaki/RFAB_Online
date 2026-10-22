#pragma once

#include <algorithm>
#include <cstdint>

// Token bucket, used to bound how much work a single client can create.
//
// The XP report is the clearest example of an amplified message: one small
// packet from one client becomes a fan-out to every player on the server. A
// buggy or hostile client could otherwise use it to multiply traffic by the
// player count. Legitimate traffic is one report per second, so a generous
// bucket never gets in the way of real gameplay while it still stops a flood.
//
// Dependency free and unit tested on the host (Code/tests/pure/).
namespace Gameplay
{
class TokenBucket
{
public:
    TokenBucket() noexcept = default;

    TokenBucket(uint32_t aCapacity, uint32_t aRefillPerSecond) noexcept
        : m_capacity(aCapacity == 0 ? 1 : aCapacity)
        , m_refillPerSecond(aRefillPerSecond)
        , m_tokens(m_capacity)
        , m_configured(true)
    {
    }

    //! False for a default constructed bucket, which only allows a single token.
    [[nodiscard]] bool IsConfigured() const noexcept { return m_configured; }

    //! Reconfigures the bucket and refills it (config reload).
    void Configure(uint32_t aCapacity, uint32_t aRefillPerSecond) noexcept
    {
        m_capacity = aCapacity == 0 ? 1 : aCapacity;
        m_refillPerSecond = aRefillPerSecond;
        m_tokens = m_capacity;
        m_lastRefillMs = 0;
        m_initialised = false;
        m_configured = true;
    }

    /**
     * @brief Takes one token.
     * @param aNowMs monotonic milliseconds (any origin, only deltas matter)
     * @return true when the action is allowed.
     */
    bool TryConsume(uint64_t aNowMs) noexcept
    {
        Refill(aNowMs);

        if (m_tokens == 0)
        {
            ++m_rejected;
            return false;
        }

        --m_tokens;
        return true;
    }

    [[nodiscard]] uint32_t Available() const noexcept { return m_tokens; }
    [[nodiscard]] uint64_t Rejected() const noexcept { return m_rejected; }
    void ResetRejected() noexcept { m_rejected = 0; }

private:
    void Refill(uint64_t aNowMs) noexcept
    {
        // The first call only starts the clock; a client that connects while
        // the server has been running for hours must not get a huge burst.
        if (!m_initialised)
        {
            m_initialised = true;
            m_lastRefillMs = aNowMs;
            return;
        }

        // A clock that went backwards means "restart": refill to full.
        if (aNowMs < m_lastRefillMs)
        {
            m_tokens = m_capacity;
            m_lastRefillMs = aNowMs;
            return;
        }

        if (m_refillPerSecond == 0)
        {
            m_lastRefillMs = aNowMs;
            return;
        }

        const uint64_t elapsed = aNowMs - m_lastRefillMs;
        const uint64_t tokensPerMs = 1000; // denominator

        // tokens = elapsed * rate / 1000, done in 64 bit to avoid overflow for
        // very long intervals.
        const uint64_t gained = (elapsed * m_refillPerSecond) / tokensPerMs;
        if (gained == 0)
            return;

        m_lastRefillMs += (gained * tokensPerMs) / m_refillPerSecond;
        m_tokens = static_cast<uint32_t>(std::min<uint64_t>(m_capacity, static_cast<uint64_t>(m_tokens) + gained));
    }

    uint32_t m_capacity{1};
    uint32_t m_refillPerSecond{0};
    uint32_t m_tokens{1};
    uint64_t m_lastRefillMs{0};
    uint64_t m_rejected{0};
    bool m_initialised{false};
    bool m_configured{false};
};
} // namespace Gameplay
