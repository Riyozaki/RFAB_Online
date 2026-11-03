#pragma once

/**
 * Rate limit for concentration spells.
 *
 * Flames, Sparks and the rest evaluate at frame rate. Sending every evaluation floods the
 * channel, so beams are coalesced to Skyrim's own magic tick cadence without losing the
 * caster's perk scaling.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace Sync
{

// Zero-heap, thread-safe packet rate-limiter for hostile concentration `AddTargetEvent` beams
// (Flames, Sparks, Frostbite, Lightning Ray), coalescing 60 Hz Havok beam evaluations to Skyrim's
// ~5 Hz magic tick cadence without dropping caster-authoritative RFAB Destruction perk scaling.
template <size_t Capacity = 64>
class ConcentrationLimiter
{
public:
    static constexpr uint64_t kDefaultCadenceMs = 180ULL;

    void Clear() noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_slots.fill(Slot{});
        m_cursor = 0;
    }

    [[nodiscard]] bool ShouldAllowSend(
        uint32_t aCasterFormId,
        uint32_t aTargetFormId,
        uint32_t aEffectFormId,
        uint64_t aNowMs,
        uint64_t aCadenceMs = kDefaultCadenceMs) noexcept
    {
        if (aTargetFormId == 0 || aEffectFormId == 0)
            return true;

        std::scoped_lock lock(m_mutex);
        for (auto& slot : m_slots)
        {
            if (slot.Active &&
                slot.CasterFormId == aCasterFormId &&
                slot.TargetFormId == aTargetFormId &&
                slot.EffectFormId == aEffectFormId)
            {
                if (aNowMs >= slot.LastSentAtMs && (aNowMs - slot.LastSentAtMs) < aCadenceMs)
                    return false;
                slot.LastSentAtMs = aNowMs;
                return true;
            }
        }

        for (auto& slot : m_slots)
        {
            if (!slot.Active || (aNowMs >= slot.LastSentAtMs && (aNowMs - slot.LastSentAtMs) >= aCadenceMs * 8))
            {
                slot = Slot{aCasterFormId, aTargetFormId, aEffectFormId, aNowMs, true};
                return true;
            }
        }

        auto& victim = m_slots[m_cursor % Capacity];
        ++m_cursor;
        victim = Slot{aCasterFormId, aTargetFormId, aEffectFormId, aNowMs, true};
        return true;
    }

private:
    struct Slot
    {
        uint32_t CasterFormId{0};
        uint32_t TargetFormId{0};
        uint32_t EffectFormId{0};
        uint64_t LastSentAtMs{0};
        bool Active{false};
    };

    std::mutex m_mutex{};
    std::array<Slot, Capacity> m_slots{};
    size_t m_cursor{0};
};

} // namespace Sync
