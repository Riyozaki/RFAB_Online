#pragma once

/**
 * Whether the server believes a hit reported by someone who does not simulate the target.
 *
 * A peer that is not the owner still swings a sword. The server has to decide if the
 * damage is real: the attacker must be in range, the target alive, the magnitude sane,
 * and friendly fire off unless PvP is on. Without that check, a client can delete any
 * actor by sending one packet.
 */

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Authority
{

// ============================================================================
// 5. Server-Side Combat & HitClaim Authority Ledger (v2.1.0)
// ============================================================================
// Server-side counterpart to client `ScriptDamageLedger`:
// - Records explicit combat engagement claims (`SpellOrTarget`, `ProjectileLaunch`, `MeleeOrDamage`) per player.
// - Tracks when a player enters the Downed-on-Knee state (`DownedSinceMs`).
// - Validates non-owner `HealthChangeBroadcast` deltas on the server:
//   1. Rejects non-finite, positive (`>= 0`), or out-of-bounds spoofed deltas (`< -100000.0f`).
//   2. Rejects hits on targets already marked dead on the server (`aIsTargetAlreadyDead`).
//   3. Rejects friendly-fire on co-op party members/summons when PvP is disabled (`!aIsPvpEnabled && aAreCoopPartyAllies`).
//   4. Rejects ambient/cloak/script damage emitted by a currently Downed player (`aIsSenderDowned`),
//      UNLESS that player recorded an in-flight projectile or spell claim BEFORE going downed within `kPostDownedInFlightGraceMs` (1500 ms)!
// Stamp for an outgoing hit, unique for the lifetime of this process. The server only
// compares it with the last few ids from the same player: it says "this is the swing you
// already told me about", nothing more. 0 means the sender did not stamp it.
[[nodiscard]] inline uint32_t NextOutgoingHitId() noexcept
{
    static std::atomic<uint32_t> s_counter{0};
    uint32_t id = s_counter.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (id == 0u)
        id = s_counter.fetch_add(1, std::memory_order_relaxed) + 1u;
    return id;
}

enum class HitKind : uint8_t
{
    kNone = 0,
    kSpellOrTarget = 1,
    kProjectileLaunch = 2,
    kMeleeOrDamage = 3,
};

template <size_t Capacity = 64>
class HitLedger
{
public:
    static constexpr uint64_t kDefaultClaimDurationMs = 4000;
    static constexpr uint64_t kPostDownedInFlightGraceMs = 1500;
    static constexpr float kMaxValidSingleHitMagnitude = 100000.0f;

    // How many recent hits per attacker are remembered. A swing re-sent after this many
    // later hits from the same player is indistinguishable from a new one, which is fine:
    // by then the damage has long been applied either way.
    static constexpr size_t kRecentHitMemory = 16;

    void Clear() noexcept
    {
        for (auto& slot : m_claims)
            slot = {};
        for (auto& d : m_downedStates)
            d = {};
        for (auto& hit : m_recentHits)
            hit = {};
        m_claimCursor = 0;
        m_downedCursor = 0;
        m_hitCursor = 0;
    }

    // One swing, one application. The transport delivers each packet once; a client that
    // re-sends after a hiccup - or one that was told to - otherwise has its damage counted
    // twice, and the party watches the boss melt at half the hits it should take.
    [[nodiscard]] bool IsDuplicateHit(uint32_t aPlayerId, uint32_t aTargetServerId, uint32_t aHitId) noexcept
    {
        if (aPlayerId == 0 || aHitId == 0)
            return false;

        for (const auto& hit : m_recentHits)
        {
            if (hit.Active && hit.PlayerId == aPlayerId && hit.TargetServerId == aTargetServerId &&
                hit.HitId == aHitId)
            {
                return true;
            }
        }

        m_recentHits[m_hitCursor % kRecentHitMemory] = RecentHit{aPlayerId, aTargetServerId, aHitId, true};
        ++m_hitCursor;
        return false;
    }

    void ForgetPlayer(uint32_t aPlayerId) noexcept
    {
        if (aPlayerId == 0)
            return;
        for (auto& hit : m_recentHits)
        {
            if (hit.Active && hit.PlayerId == aPlayerId)
                hit = {};
        }
        for (auto& slot : m_claims)
        {
            if (slot.Active && slot.AttackerPlayerId == aPlayerId)
                slot = {};
        }
        for (auto& d : m_downedStates)
        {
            if (d.Active && d.PlayerId == aPlayerId)
                d = {};
        }
    }

    void RecordPlayerDownedState(uint32_t aPlayerId, bool aIsDowned, uint64_t aNowMs) noexcept
    {
        if (aPlayerId == 0)
            return;
        for (auto& d : m_downedStates)
        {
            if (d.Active && d.PlayerId == aPlayerId)
            {
                if (!aIsDowned)
                {
                    d = {};
                }
                else if (!d.IsDowned)
                {
                    d.IsDowned = true;
                    d.DownedSinceMs = aNowMs;
                }
                return;
            }
        }
        if (aIsDowned)
        {
            m_downedStates[m_downedCursor % Capacity] = DownedSlot{aPlayerId, aNowMs, true, true};
            ++m_downedCursor;
        }
    }

    void RecordCombatClaim(
        uint32_t aAttackerPlayerId,
        uint32_t aTargetServerId,
        HitKind aKind,
        uint64_t aNowMs,
        uint64_t aDurationMs = kDefaultClaimDurationMs) noexcept
    {
        if (aAttackerPlayerId == 0 || aKind == HitKind::kNone)
            return;

        const uint64_t expiresAt = aNowMs + aDurationMs;
        for (auto& slot : m_claims)
        {
            if (slot.Active && slot.AttackerPlayerId == aAttackerPlayerId && slot.TargetServerId == aTargetServerId)
            {
                slot.Kind = aKind;
                slot.ClaimedAtMs = aNowMs;
                slot.ExpiresAtMs = expiresAt;
                return;
            }
        }

        for (auto& slot : m_claims)
        {
            if (!slot.Active || aNowMs >= slot.ExpiresAtMs)
            {
                slot = ClaimSlot{aAttackerPlayerId, aTargetServerId, aNowMs, expiresAt, aKind, true};
                return;
            }
        }

        m_claims[m_claimCursor % Capacity] =
            ClaimSlot{aAttackerPlayerId, aTargetServerId, aNowMs, expiresAt, aKind, true};
        ++m_claimCursor;
    }

    [[nodiscard]] bool HasActiveClaim(
        uint32_t aAttackerPlayerId,
        uint32_t aTargetServerId,
        uint64_t aNowMs) const noexcept
    {
        if (aAttackerPlayerId == 0)
            return false;
        for (const auto& slot : m_claims)
        {
            if (!slot.Active || slot.AttackerPlayerId != aAttackerPlayerId || aNowMs >= slot.ExpiresAtMs)
                continue;
            if (slot.TargetServerId == aTargetServerId || slot.TargetServerId == 0)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool ValidateNonOwnerHealthDelta(
        uint32_t aAttackerPlayerId,
        uint32_t aTargetServerId,
        float aDeltaHealth,
        bool aIsSenderInRange,
        bool aIsSenderDowned,
        bool aIsTargetAlreadyDead,
        bool aAreCoopPartyAllies,
        bool aIsPvpEnabled,
        uint64_t aNowMs) noexcept
    {
        if (aAttackerPlayerId == 0 || aTargetServerId == 0 || !aIsSenderInRange)
            return false;

        if (!std::isfinite(aDeltaHealth) || aDeltaHealth >= 0.0f || aDeltaHealth < -kMaxValidSingleHitMagnitude)
            return false;

        if (aIsTargetAlreadyDead)
            return false;

        if (aAreCoopPartyAllies && !aIsPvpEnabled)
            return false;

        if (aIsSenderDowned)
        {
            uint64_t downedSinceMs = aNowMs;
            for (const auto& d : m_downedStates)
            {
                if (d.Active && d.PlayerId == aAttackerPlayerId && d.IsDowned)
                {
                    downedSinceMs = d.DownedSinceMs;
                    break;
                }
            }

            // Only allow pre-downed in-flight projectiles or spells launched BEFORE `downedSinceMs`
            // within `kPostDownedInFlightGraceMs` (1500 ms); block all post-downed cloak/script bleed!
            bool hasPreDownedInFlightClaim = false;
            for (const auto& slot : m_claims)
            {
                if (!slot.Active || slot.AttackerPlayerId != aAttackerPlayerId || aNowMs >= slot.ExpiresAtMs)
                    continue;
                if (slot.Kind != HitKind::kProjectileLaunch &&
                    slot.Kind != HitKind::kSpellOrTarget)
                {
                    continue;
                }
                if (slot.TargetServerId != aTargetServerId && slot.TargetServerId != 0)
                    continue;
                if (slot.ClaimedAtMs <= downedSinceMs &&
                    aNowMs <= (downedSinceMs + kPostDownedInFlightGraceMs))
                {
                    hasPreDownedInFlightClaim = true;
                    break;
                }
            }

            if (!hasPreDownedInFlightClaim)
                return false;
        }
        else
        {
            RecordCombatClaim(aAttackerPlayerId, aTargetServerId, HitKind::kMeleeOrDamage, aNowMs);
        }

        return true;
    }

private:
    struct ClaimSlot
    {
        uint32_t AttackerPlayerId{0};
        uint32_t TargetServerId{0};
        uint64_t ClaimedAtMs{0};
        uint64_t ExpiresAtMs{0};
        HitKind Kind{HitKind::kNone};
        bool Active{false};
    };

    struct RecentHit
    {
        uint32_t PlayerId{0};
        uint32_t TargetServerId{0};
        uint32_t HitId{0};
        bool Active{false};
    };

    struct DownedSlot
    {
        uint32_t PlayerId{0};
        uint64_t DownedSinceMs{0};
        bool IsDowned{false};
        bool Active{false};
    };

    std::array<ClaimSlot, Capacity> m_claims{};
    std::array<DownedSlot, Capacity> m_downedStates{};
    std::array<RecentHit, kRecentHitMemory> m_recentHits{};
    size_t m_claimCursor{0};
    size_t m_downedCursor{0};
    size_t m_hitCursor{0};
};

} // namespace Authority
