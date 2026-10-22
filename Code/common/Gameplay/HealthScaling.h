#pragma once

#include <cstdint>

// Party-size health scaling for hostile NPCs.
//
// Product requirement (RFAB Online):
//   1 player  -> x1 health (vanilla, single player must not change)
//   2 players -> x3 health
//   3 players -> x4 health
//   N players -> x(N + 1)
//
// The *number of enemies* is never touched: spawns stay exactly as the game
// loads them, only the maximum health of non-player actors is scaled.
//
// This header is intentionally dependency free (no EnTT, no game headers) so
// that the exact arithmetic used by the server can be unit tested on any
// platform - see Code/tests/pure/ and tools/run_pure_tests.sh.
namespace Gameplay::HealthScaling
{
// Safety limits. A wrong config or a bug must never be able to produce an
// unkillable actor or a division by zero.
inline constexpr uint32_t kMaxPlayersConsidered = 63;
inline constexpr float kMaxMultiplier = 32.0f;
inline constexpr float kMinMultiplier = 1.0f;

/**
 * @brief Health multiplier for a given number of players.
 *
 * `aPlayers` is clamped into [1, kMaxPlayersConsidered]; 0 is treated as 1 so
 * that a transient empty player list can never produce a zero multiplier.
 */
constexpr float MultiplierFor(uint32_t aPlayers) noexcept
{
    if (aPlayers == 0)
        return kMinMultiplier;

    if (aPlayers > kMaxPlayersConsidered)
        aPlayers = kMaxPlayersConsidered;

    if (aPlayers == 1)
        return kMinMultiplier;

    const float multiplier = static_cast<float>(aPlayers) + 1.0f;

    return multiplier > kMaxMultiplier ? kMaxMultiplier : multiplier;
}

/**
 * @brief Scales a base (unscaled) maximum value.
 *
 * Values <= 0 and multipliers < 1 are returned unchanged: scaling must never
 * shrink an actor's health, and it must not touch actors the game considers
 * dead/zeroed.
 */
inline float ScaledMax(float aBaseMax, float aMultiplier) noexcept
{
    if (aBaseMax <= 0.0f)
        return aBaseMax;

    if (aMultiplier < kMinMultiplier)
        return aBaseMax;

    const float scaled = aBaseMax * aMultiplier;

    return scaled > 0.0f ? scaled : aBaseMax;
}

/**
 * @brief Keeps the health *ratio* when the maximum changes.
 *
 * A wounded actor (10 of 100) becomes 40 of 400 with x4, not 10 of 400, so that
 * phase transitions and percentage-based boss scripts keep working.
 */
inline float RescaleCurrent(float aCurrent, float aOldMax, float aNewMax) noexcept
{
    if (aOldMax <= 0.0f || aNewMax <= 0.0f)
        return 0.0f;

    float ratio = aCurrent / aOldMax;

    if (ratio < 0.0f)
        ratio = 0.0f;
    if (ratio > 1.0f)
        ratio = 1.0f;

    return ratio * aNewMax;
}

/**
 * @brief What kind of actor we are looking at, as far as scaling is concerned.
 */
struct ActorFlags
{
    bool IsPlayer{false};
    bool IsPlayerSummon{false};
    bool IsDead{false};
    bool IsMount{false};
    bool IsDragon{false};
};

struct Options
{
    bool ScaleSummons{false};
};

/**
 * @brief Whether an actor's maximum health should be scaled.
 *
 * Players are never scaled (their health is theirs), dead actors are never
 * touched (scaling must not resurrect anything), mounts are never scaled (they
 * belong to a player and a x4 horse is just a bug), summons are opt-in because
 * they routinely have scripted health thresholds. Dragons *are* scaled: they are
 * ordinary hostile enemies for the purpose of this feature.
 */
constexpr bool ShouldScale(const ActorFlags acFlags, const Options acOptions = {}) noexcept
{
    if (acFlags.IsPlayer)
        return false;

    if (acFlags.IsDead)
        return false;

    if (acFlags.IsMount)
        return false;

    if (acFlags.IsPlayerSummon && !acOptions.ScaleSummons)
        return false;

    return true;
}

/**
 * @brief Computes the health values to apply to a single actor.
 *
 * @param aBaseMax   the actor's unscaled maximum health (stored server side)
 * @param aCurrent   its current health
 * @param aOldMax    the maximum currently applied on the actor
 * @param aMultiplier desired multiplier
 * @param aNewMax    out: maximum to apply
 * @param aNewCurrent out: current health to apply (ratio preserved)
 * @return false when nothing has to change
 */
inline bool ComputeTarget(float aBaseMax,
                          float aCurrent,
                          float aOldMax,
                          float aMultiplier,
                          float& aNewMax,
                          float& aNewCurrent) noexcept
{
    if (aBaseMax <= 0.0f)
        return false;

    aNewMax = ScaledMax(aBaseMax, aMultiplier);
    if (aNewMax <= 0.0f)
        return false;

    if (aOldMax <= 0.0f)
    {
        aNewCurrent = aNewMax;
        return true;
    }

    aNewCurrent = RescaleCurrent(aCurrent, aOldMax, aNewMax);

    // Nothing to do when the maximum is already what we want (within float noise).
    const float delta = aNewMax - aOldMax;
    const float tolerance = (aOldMax > 1.0f ? aOldMax : 1.0f) * 0.0005f;

    return (delta > tolerance) || (delta < -tolerance);
}

/**
 * @brief What maximum health to restore when leaving a server.
 *
 * Two sources are possible, in order of accuracy:
 *   * the unscaled value captured locally when the server first scaled the
 *     actor (`aRememberedBase`), which is exact;
 *   * only the session multiplier (`aSessionMultiplier`), which is all a client
 *     has for actors that arrived already scaled (a late joiner receives the
 *     scaled maximum in ActorData). Dividing by the multiplier is the exact
 *     inverse when nothing else touched the value.
 *
 * Never shrinks nothing: when the actor is not scaled, `ShouldRestore` is false.
 */
struct RestoreTarget
{
    float Base{0.0f};
    bool ShouldRestore{false};
};

inline RestoreTarget ComputeRestore(float aCurrentMax, float aRememberedBase, float aSessionMultiplier) noexcept
{
    RestoreTarget result{};

    if (!(aCurrentMax > 0.0f)) // also rejects NaN
        return result;

    float base = 0.0f;

    if (aRememberedBase > 0.0f)
    {
        // A remembered base is authoritative - including when it equals the
        // current maximum, which simply means "this actor was never scaled".
        base = aRememberedBase;
    }
    else if (aSessionMultiplier > 1.0f)
    {
        base = aCurrentMax / aSessionMultiplier;
    }

    // A base that is not a real reduction means there is nothing to undo.
    const float tolerance = (aCurrentMax > 1.0f ? aCurrentMax : 1.0f) * 0.0001f;
    if (!(base > 0.0f) || base >= aCurrentMax - tolerance)
        return result;

    result.Base = base;
    result.ShouldRestore = true;
    return result;
}

} // namespace Gameplay::HealthScaling
