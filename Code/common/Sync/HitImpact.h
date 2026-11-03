#pragma once

/**
 * What a hit looks like on the receiving end: stagger direction and how often it may play.
 *
 * Stagger is cosmetic but it interrupts actions, so a remote hit storm turns a fight into
 * a stun lock. The gate spaces them out per target.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace Sync
{

// 8D. Normalized Skyrim Havok `staggerDirection` (`[0.0, 1.0)`):
// `0.0` = hit from front (stagger backward), `0.25` = right, `0.5` = behind (stagger forward), `0.75` = left.
// Note: Skyrim's Z-rotation (`rot.z`) is clockwise heading from +Y (`0 = North (+Y), pi/2 = East (+X)`).
[[nodiscard]] inline float ComputeNormalizedStaggerDirection(
    float aAttackerX,
    float aAttackerY,
    float aTargetX,
    float aTargetY,
    float aTargetHeadingRad) noexcept
{
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float dx = aAttackerX - aTargetX;
    const float dy = aAttackerY - aTargetY;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(aTargetHeadingRad))
        return 0.0f;

    if ((dx * dx + dy * dy) <= 1e-4f)
        return 0.0f;

    float worldBearingRad = std::atan2(dx, dy);
    float relRad = worldBearingRad - aTargetHeadingRad;
    relRad = std::fmod(relRad, kTwoPi);
    if (relRad < 0.0f)
        relRad += kTwoPi;

    return std::clamp(relRad / kTwoPi, 0.0f, 0.9999f);
}

// 8E. Per-Target Poise / Stagger Cooldown Gate on Authoritative Owner:
// Propagates Guest Shield Bashes, Power Attacks, Power Shots, and RFAB poise breaks onto Host's live AI
// while enforcing a per-target poise recovery window (`1650 ms`) so multi-player hits cannot infinitely stunlock bosses.
class HitImpactGate
{
public:
    static constexpr uint64_t kMinStaggerIntervalMs = 1650ULL;
    static constexpr uint64_t kEntryTtlMs = 30000ULL;

    [[nodiscard]] float TryConsumeStagger(
        uint32_t aTargetServerId,
        float aRequestedMagnitude,
        bool aIsTargetGhostOrDead,
        uint64_t aNowMs) noexcept
    {
        if (aTargetServerId == 0 || aIsTargetGhostOrDead || !std::isfinite(aRequestedMagnitude) || aRequestedMagnitude <= 0.05f)
            return 0.0f;

        std::lock_guard<std::mutex> lock(m_mutex);
        PruneExpiredLocked(aNowMs);

        auto it = m_lastStaggerMsByTarget.find(aTargetServerId);
        if (it != m_lastStaggerMsByTarget.end() &&
            aNowMs >= it->second &&
            (aNowMs - it->second) < kMinStaggerIntervalMs)
        {
            return 0.0f;
        }

        m_lastStaggerMsByTarget[aTargetServerId] = aNowMs;
        return std::clamp(aRequestedMagnitude, 0.10f, 1.0f);
    }

    void Clear() noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastStaggerMsByTarget.clear();
    }

private:
    void PruneExpiredLocked(uint64_t aNowMs) noexcept
    {
        if (m_lastStaggerMsByTarget.size() < 64)
            return;

        for (auto it = m_lastStaggerMsByTarget.begin(); it != m_lastStaggerMsByTarget.end();)
        {
            if (aNowMs > it->second + kEntryTtlMs)
                it = m_lastStaggerMsByTarget.erase(it);
            else
                ++it;
        }
    }

    std::mutex m_mutex;
    std::unordered_map<uint32_t, uint64_t> m_lastStaggerMsByTarget;
};

} // namespace Sync
