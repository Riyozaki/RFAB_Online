#pragma once

/**
 * Arithmetic for health, and only arithmetic.
 *
 * Applying a delta to an actor's health is where double application, negative health and
 * bosses healing back from a late packet live. These functions take values and return
 * values - no actors, no packets - so the rules can be executed in the fast loop.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Authority
{

// ============================================================================
// 6. Native RFAB MaxHP Preservation & Incoming Enemy Damage Scaling (v3.0 Phase 4A)
// ============================================================================
// Instead of mutating client `PERMANENT kHealth` (`ForceActorValue(PERMANENT, kHealth, base * M)`),
// every NPC keeps its exact 1.0x native `RFAB.esp` `MaxHP` across Host, Guest, Server, and `.ess`
// checkpoints, while incoming enemy health damage is scaled by `1 / M` at the engine damage funnel:
//   - Boss percentage-based phase transitions (`50%`, `25%`), execute perks, and HUD bars stay 100% exact.
//   - Zero `3^K` / `1/3` permanent HP drift across ownership handoffs or `.ess` checkpoint saves.
//   - Percentage `HealRate` regeneration and boss phase percentage heals (`+0.5 * MaxHP`) stay 1:1 exact.
[[nodiscard]] inline float NormalizeEnemyHealthMultiplier(float aMultiplier) noexcept
{
    if (!std::isfinite(aMultiplier) || aMultiplier <= 0.0f)
        return 1.0f;
    return std::clamp(aMultiplier, 0.1f, 10.0f);
}

[[nodiscard]] constexpr bool ShouldScaleIncomingEnemyDamage(
    bool aIsPlayer,
    bool aIsPlayerTeammate,
    bool aIsPlayerSummon,
    bool aIsCommandedActor,
    bool aIsMount) noexcept
{
    return !aIsPlayer && !aIsPlayerTeammate && !aIsPlayerSummon && !aIsCommandedActor && !aIsMount;
}

[[nodiscard]] inline float ScaleIncomingEnemyDamageDelta(
    float aRawDelta,
    float aEffectiveMultiplier,
    bool aShouldScale) noexcept
{
    if (!std::isfinite(aRawDelta))
        return 0.0f;
    if (!aShouldScale || aRawDelta >= 0.0f)
        return aRawDelta;
    return aRawDelta / NormalizeEnemyHealthMultiplier(aEffectiveMultiplier);
}

struct HealthDeltaResult
{
    float UpdatedHealth{0.0f};
    float ClampedBroadcastDelta{0.0f};
};

[[nodiscard]] inline HealthDeltaResult ApplyHealthDelta(
    float aCurrentHealth,
    float aMaxHealth,
    float aSanitizedDelta) noexcept
{
    HealthDeltaResult out{};
    if (!std::isfinite(aSanitizedDelta))
    {
        out.UpdatedHealth = std::isfinite(aCurrentHealth) ? std::max(0.0f, aCurrentHealth) : 0.0f;
        return out;
    }

    const float safeCurrent = std::isfinite(aCurrentHealth) ? aCurrentHealth : 0.0f;
    const float rawUpdated = safeCurrent + aSanitizedDelta;
    if (std::isfinite(aMaxHealth) && aMaxHealth > 0.0f)
    {
        out.UpdatedHealth = std::clamp(rawUpdated, 0.0f, aMaxHealth);
        out.ClampedBroadcastDelta = std::clamp(aSanitizedDelta, -aMaxHealth, aMaxHealth);
    }
    else
    {
        out.UpdatedHealth = std::max(0.0f, rawUpdated);
        out.ClampedBroadcastDelta = aSanitizedDelta;
    }
    return out;
}

// ============================================================================
// 8. Hybrid Combat, Regen Parity, Perk-Exact Magic & Hit-Impact Gate (v3.1)
// ============================================================================
//
// 8A. `1/M` Flat Enemy Combat Regeneration & ActiveEffect Heal Parity:
// When incoming player damage on hostile/world NPCs is divided by `M` (`1x` native MaxHP preserved),
// leaving Requiem/RFAB flat HP/sec regeneration abilities (trolls, spriggans, vampires, dragon priests,
// enchanted spheres, NPC healing potions, enemy Restoration heals) unscaled multiplies effective enemy
// regeneration by `M` (3x-4x DPS check inflation!).
// Scaling `ActiveEffect` positive health ticks (and small flat heals `< 25%` of native MaxHP) by `1/M`
// keeps solo RFAB DPS-vs-regen balance 1:1 invariant while preserving instantaneous scripted Boss phase
// reset bursts (`!aIsActiveEffectTick && aRawDelta >= 0.25f * aNativeMaxHp`) at exact native `%` thresholds.
[[nodiscard]] inline float ScaleIncomingEnemyHealDelta(
    float aRawDelta,
    float aNativeMaxHp,
    float aEnemyHealthMultiplier,
    bool aIsEligibleEnemyTarget,
    bool aIsActiveEffectTick) noexcept
{
    if (!std::isfinite(aRawDelta))
        return 0.0f;

    if (aRawDelta <= 0.0f || !aIsEligibleEnemyTarget)
        return aRawDelta;

    const float mult = NormalizeEnemyHealthMultiplier(aEnemyHealthMultiplier);
    if (std::abs(mult - 1.0f) <= 0.001f)
        return aRawDelta;

    // Preserve instantaneous non-ActiveEffect scripted boss phase heals (>= 25% of native MaxHP) 1:1
    // so multi-phase boss scripts (e.g., +50% or +100% phase transition) reach exact target percentages.
    if (!aIsActiveEffectTick && std::isfinite(aNativeMaxHp) && aNativeMaxHp > 0.0f && aRawDelta >= 0.25f * aNativeMaxHp)
        return aRawDelta;

    return aRawDelta / mult;
}

// 8B. Outgoing Caster `AdjustForPerks` Spell Power Ratio:
// Captures the exact multiplier applied by `ActiveEffect::AdjustForPerks` on the caster's real `0x14`
// (Requiem/RFAB `CustomSkills` trees, 2nd ring spell power, Fortify potions, Dual-Cast perks, target-conditional
// Sun/Silver perks) while canceling out local target resistance (`M_after / M_before`).
[[nodiscard]] inline float ComputeCasterOutgoingPerkMultiplier(
    float aMagnitudeBeforeAdjust,
    float aMagnitudeAfterAdjust) noexcept
{
    if (!std::isfinite(aMagnitudeBeforeAdjust) || !std::isfinite(aMagnitudeAfterAdjust))
        return 1.0f;

    if (std::abs(aMagnitudeBeforeAdjust) <= 1e-4f)
        return 1.0f;

    const float rawRatio = aMagnitudeAfterAdjust / aMagnitudeBeforeAdjust;
    if (!std::isfinite(rawRatio) || rawRatio <= 0.0f)
        return 1.0f;

    return std::clamp(rawRatio, 0.05f, 25.0f);
}

// 8D. Whose absolute health value is worth believing.
//
// The client that simulates an actor restates its health every few frames, and every one
// of those reports was sampled before it was sent. A report that crossed a hit on the wire
// hands the health back. Holding such reports for a few hundred milliseconds only delays
// them: the same stale number arrives again on the next tick, and once the window closes
// the server believes it. A timer cannot tell a stale reading from a fresh one.
//
// So the server counts instead. One revision per actor, bumped every time health goes
// down. Whoever reports an absolute value also says which revision it had seen when it
// read it. Lower health is always believed - damage must never be lost. Higher health is
// believed only from someone who had already seen the latest decrease, because only then
// is "it healed" a statement about the present instead of an echo of the past.
struct SnapshotPolicy
{
    // Differences smaller than this are float noise, not a correction.
    float NoiseBand{0.5f};
};

enum class SnapshotVerdict : uint8_t
{
    kApply,          // believe it
    kAlreadyCurrent, // says what we already know - still worth passing on to the others
    kStaleIncrease,  // would hand back health somebody took off: refuse, and answer with
                     // the truth, or the sender stays behind and every later reading of
                     // its own is refused too
    kMalformed,
};

[[nodiscard]] inline SnapshotVerdict ClassifyHealthSnapshot(
    float aCurrentHp,
    float aSnapshotHp,
    uint32_t aCurrentRevision,
    uint32_t aSnapshotRevision,
    SnapshotPolicy aPolicy = {}) noexcept
{
    if (!std::isfinite(aCurrentHp) || !std::isfinite(aSnapshotHp) || aSnapshotHp < 0.0f)
        return SnapshotVerdict::kMalformed;

    const float diff = aSnapshotHp - aCurrentHp;
    if (diff < -aPolicy.NoiseBand)
        return SnapshotVerdict::kApply; // damage always lands

    if (diff <= aPolicy.NoiseBand)
        return SnapshotVerdict::kAlreadyCurrent;

    return aSnapshotRevision == aCurrentRevision ? SnapshotVerdict::kApply
                                                 : SnapshotVerdict::kStaleIncrease;
}

// 8C. Non-Rubberbanding Authoritative `CurrentHP` Snapshot Reconciliation:
// Allows remote non-player NPCs on Guests to reconcile `kHealth` from periodic owner snapshots
// (preventing permanent HP bar drift on missed deltas, regen, or DoTs) WITHOUT upward rubberbanding
// during the `700 ms` window right after the local player landed a predicted hit.
[[nodiscard]] inline bool ShouldReconcileRemoteActorHealthSnapshot(
    bool aIsRemoteNonPlayer,
    bool aIsDead,
    float aLocalHp,
    float aSnapshotHp,
    uint64_t aNowMs,
    uint64_t aLastLocalPredictedHitMs,
    uint64_t aPredictionHoldWindowMs = 700ULL) noexcept
{
    if (!aIsRemoteNonPlayer || aIsDead)
        return false;

    if (!std::isfinite(aLocalHp) || !std::isfinite(aSnapshotHp) || aSnapshotHp < 0.0f)
        return false;

    const float diff = aSnapshotHp - aLocalHp;
    if (std::abs(diff) <= 0.5f)
        return false;

    // Downward corrections (NPC took additional damage on Host/allies) always apply immediately.
    if (diff < 0.0f)
        return true;

    // Upward corrections (regen/heals) hold briefly right after a local hit so in-flight deltas never bounce.
    if (aLastLocalPredictedHitMs != 0 &&
        aNowMs >= aLastLocalPredictedHitMs &&
        (aNowMs - aLastLocalPredictedHitMs) <= aPredictionHoldWindowMs)
    {
        return false;
    }

    return true;
}

} // namespace Authority
