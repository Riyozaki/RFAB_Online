#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Reconciliation helpers for state that travels *with* an ownership change: health,
// death and handoff coordinates. Who owns what is not decided here - that is
// Authority::Lease (Code/common/Authority/Lease.h), which replaced the claim policy,
// the handoff validation and the client-side epoch tracker that used to live in this
// header.
namespace CoopOwnership
{
// Ratio- and RFAB-armor-aware ownership health reconciliation:
// - If the server shows the actor is dead -> enforce dead (0 HP).
// - If the server shows the actor was UNWOUNDED (Current >= Max - 0.5f) and the claiming peer reports
//   higher Max & Current HP (e.g. RFAB OnLoad script finished equipping Fortify-Health guard armor or
//   HealthMultiplier scaled max HP), preserve the claimer's full HP instead of clamping down to pre-armor base HP!
// - If the server shows genuine combat damage (Current < Max - 0.5f), preserve the wounded health ratio
//   against effectiveMax while strictly keeping living actors at >= 1.0f HP.
inline void ReconcileClaimedStateWithMax(
    bool aServerIsDead,
    float aServerCurrentHealth,
    float aServerMaxHealth,
    bool aHasAuthoritativeHealth,
    bool& aInOutClaimedIsDead,
    float& aInOutClaimedHealth,
    float& aInOutClaimedMaxHealth) noexcept
{
    if (aServerIsDead)
    {
        aInOutClaimedIsDead = true;
        aInOutClaimedHealth = 0.0f;
        return;
    }

    if (!aHasAuthoritativeHealth || aInOutClaimedIsDead || aServerCurrentHealth <= 0.0f)
        return;

    if (aServerMaxHealth > 0.0f)
    {
        const bool serverUnwounded = (aServerCurrentHealth >= aServerMaxHealth - 0.5f);
        if (serverUnwounded && aInOutClaimedMaxHealth >= aServerMaxHealth && aInOutClaimedHealth >= aInOutClaimedMaxHealth - 0.5f)
        {
            return;
        }

        const float effectiveMax = std::max(aServerMaxHealth, aInOutClaimedMaxHealth);
        aInOutClaimedMaxHealth = effectiveMax;

        const float ratio = std::clamp(aServerCurrentHealth / aServerMaxHealth, 0.0f, 1.0f);
        const float targetWoundedHp = std::max(1.0f, ratio * effectiveMax);
        if (targetWoundedHp < aInOutClaimedHealth)
            aInOutClaimedHealth = targetWoundedHp;
    }
    else if (aServerCurrentHealth < aInOutClaimedHealth)
    {
        aInOutClaimedHealth = std::max(1.0f, aServerCurrentHealth);
    }

    if (aInOutClaimedHealth <= 0.0f)
        aInOutClaimedHealth = 1.0f;
}

struct SafeHealthStepPlan
{
    bool AdjustDamageBeforePermanent{false};
    float DamageModeTarget{0.0f};
    float PermanentModeTarget{0.0f};
};

// Computes the exact non-lethal two-step ForceActorValue(PERMANENT / DAMAGE) sequence so scaling down
// MaxHealth (or applying pre-armor snapshots) NEVER transiently drives GetActorValue(kHealth) <= 0 on a live actor.
[[nodiscard]] inline SafeHealthStepPlan ComputeSafeHealthStepPlan(
    float aCurrentPermanentMax,
    float aDesiredPermanentMax,
    float aDesiredCurrentHealth,
    bool aKeepActorAlive) noexcept
{
    SafeHealthStepPlan plan{};
    const float safeDesiredMax = (std::isfinite(aDesiredPermanentMax) && aDesiredPermanentMax > 0.0f)
        ? aDesiredPermanentMax
        : std::max(1.0f, aCurrentPermanentMax);
    float safeDesiredHealth = std::isfinite(aDesiredCurrentHealth) ? aDesiredCurrentHealth : safeDesiredMax;
    if (aKeepActorAlive && safeDesiredHealth < 1.0f)
        safeDesiredHealth = 1.0f;

    plan.PermanentModeTarget = safeDesiredMax;
    if (safeDesiredMax >= aCurrentPermanentMax)
    {
        plan.AdjustDamageBeforePermanent = false;
        plan.DamageModeTarget = safeDesiredHealth;
    }
    else
    {
        plan.AdjustDamageBeforePermanent = true;
        plan.DamageModeTarget = aCurrentPermanentMax + (safeDesiredHealth - safeDesiredMax);
    }
    return plan;
}

[[nodiscard]] inline bool IsValidHandoffCoordinates(float aPosX, float aPosY, float aPosZ) noexcept
{
    if (!std::isfinite(aPosX) || !std::isfinite(aPosY) || !std::isfinite(aPosZ))
        return false;
    return std::fabs(aPosX) < 4000000.0f && std::fabs(aPosY) < 4000000.0f && std::fabs(aPosZ) < 4000000.0f;
}

// Rejects bogus (0, 0, 0) coordinates sent when an already-unloaded actor form is cancelled,
// preventing exterior persistent/city NPCs from being warped to world origin (0, 0, 0) and falling to death.
[[nodiscard]] inline bool IsValidHandoffCoordinates(
    bool aHasWorldOrCell,
    bool aIsExterior,
    float aPosX,
    float aPosY,
    float aPosZ,
    bool aHasPriorServerPos,
    float aPriorX,
    float aPriorY,
    float aPriorZ) noexcept
{
    if (!aHasWorldOrCell)
        return false;
    if (!std::isfinite(aPosX) || !std::isfinite(aPosY) || !std::isfinite(aPosZ))
        return false;

    const bool isExactOrigin = (std::fabs(aPosX) < 0.01f && std::fabs(aPosY) < 0.01f && std::fabs(aPosZ) < 0.01f);
    if (isExactOrigin && aIsExterior && aHasPriorServerPos)
    {
        const float dx = aPriorX - aPosX;
        const float dy = aPriorY - aPosY;
        const float dz = aPriorZ - aPosZ;
        if ((dx * dx + dy * dy + dz * dz) > (2048.0f * 2048.0f))
            return false;
    }

    return true;
}

} // namespace CoopOwnership
