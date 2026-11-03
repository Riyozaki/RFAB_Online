#pragma once

/**
 * Who is allowed to report Papyrus-scripted damage.
 *
 * A perk script fires on every peer that loaded the actor. Without a window saying "this
 * peer authored this hit", shared world scripts double-fire and an NPC takes the same
 * damage three times.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace Authority
{

// Zero-heap, thread-safe authorization window ledger for local-player-authored `kScript` /
// `RFAB_PapyrusFunctions.ScriptDamage` / Perk `OnHit` Papyrus health deltas targeting remote NPCs.
// Ensures:
//   - Non-owner Mages, Rogues, and Arcane Archers deal 100% of their exact RFAB perk/skill scripted damage.
//   - Shared world/quest scripts or passive world effects never double-fire across peers (`IsAuthorized == false`).
template <size_t Capacity = 64>
class ScriptDamageLedger
{
public:
    static constexpr uint64_t kMinWindowMs = 2500ULL;
    static constexpr uint64_t kMaxWindowMs = 120000ULL;

    void Clear() noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_entries.fill(Entry{});
        m_cursor = 0;
    }

    void ArmWindow(uint32_t aTargetFormId, uint64_t aNowMs, uint64_t aDurationMs) noexcept
    {
        if (aTargetFormId == 0)
            return;

        const uint64_t clampedDuration = std::clamp(aDurationMs, kMinWindowMs, kMaxWindowMs);
        const uint64_t expiresAt = aNowMs + clampedDuration;

        std::scoped_lock lock(m_mutex);
        for (auto& entry : m_entries)
        {
            if (entry.TargetFormId == aTargetFormId)
            {
                if (expiresAt > entry.ExpiresAtMs)
                    entry.ExpiresAtMs = expiresAt;
                return;
            }
        }

        for (auto& entry : m_entries)
        {
            if (entry.TargetFormId == 0 || aNowMs >= entry.ExpiresAtMs)
            {
                entry.TargetFormId = aTargetFormId;
                entry.ExpiresAtMs = expiresAt;
                return;
            }
        }

        auto& victim = m_entries[m_cursor % Capacity];
        ++m_cursor;
        victim.TargetFormId = aTargetFormId;
        victim.ExpiresAtMs = expiresAt;
    }

    [[nodiscard]] bool IsAuthorized(uint32_t aTargetFormId, uint64_t aNowMs) const noexcept
    {
        if (aTargetFormId == 0)
            return false;

        std::scoped_lock lock(m_mutex);
        for (const auto& entry : m_entries)
        {
            if (entry.TargetFormId == aTargetFormId && aNowMs < entry.ExpiresAtMs)
                return true;
        }
        return false;
    }

private:
    struct Entry
    {
        uint32_t TargetFormId{0};
        uint64_t ExpiresAtMs{0};
    };

    mutable std::mutex m_mutex{};
    std::array<Entry, Capacity> m_entries{};
    size_t m_cursor{0};
};

} // namespace Authority
