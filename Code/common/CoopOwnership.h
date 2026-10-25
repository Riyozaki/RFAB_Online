#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace CoopOwnership
{
struct PolicyConfig
{
    // Minimum hold window before another peer can voluntarily steal an actor via RequestOwnershipClaim.
    // Forced handoffs (owner disconnect, cell unload, or owner downed while challenger is alive) bypass this window.
    // Pattern adapted from cranebeaker/coop-anything (MIT License) actor-ownership playbook & OwnershipService.
    uint64_t MinVoluntaryHoldMs{1500};
};

enum class ClaimDecision : uint8_t
{
    kAccept = 0,
    kAlreadyOwner = 1,
    kRejectPlayerAvatar = 2,
    kRejectSummonPinned = 3,
    kRejectChallengerDowned = 4,
    kRejectHoldWindowActive = 5
};

inline ClaimDecision EvaluateVoluntaryClaim(
    uint32_t aCurrentOwnerPlayerId,
    uint32_t aChallengerPlayerId,
    uint64_t aLastChangedAtMs,
    uint64_t aNowMs,
    bool aIsPlayerAvatar,
    bool aIsPlayerSummon,
    bool aCurrentOwnerValidAndInRange,
    bool aCurrentOwnerDowned,
    bool aChallengerDowned,
    const PolicyConfig& acConfig = {}) noexcept
{
    if (aIsPlayerAvatar)
        return ClaimDecision::kRejectPlayerAvatar;

    if (aIsPlayerSummon)
        return ClaimDecision::kRejectSummonPinned;

    if (aChallengerPlayerId != 0 && aCurrentOwnerPlayerId == aChallengerPlayerId)
        return ClaimDecision::kAlreadyOwner;

    // Never hand AI simulation of a boss/enemy from a standing fighter to a player lying on one knee.
    if (aChallengerDowned && aCurrentOwnerValidAndInRange && !aCurrentOwnerDowned)
        return ClaimDecision::kRejectChallengerDowned;

    // If the current owner is alive, connected, and still in range of the actor, enforce anti-flap hysteresis.
    if (aCurrentOwnerPlayerId != 0 && aCurrentOwnerValidAndInRange && !aCurrentOwnerDowned)
    {
        if (aNowMs >= aLastChangedAtMs && (aNowMs - aLastChangedAtMs) < acConfig.MinVoluntaryHoldMs)
            return ClaimDecision::kRejectHoldWindowActive;
    }

    return ClaimDecision::kAccept;
}

// Validates voluntary handoff (RequestOwnershipTransfer) sent when a client unloads an actor.
// Rejects stale packets sent by a former owner after ownership already moved to another peer
// (prevents TiltedEvolution-rwf bug where a late RequestOwnershipTransfer wiped InvalidOwners and deleted live bosses).
inline bool ValidateHandoffFromOwner(
    uint32_t aAuthoritativeOwnerPlayerId,
    uint64_t aAuthoritativeEpoch,
    uint32_t aSenderPlayerId,
    uint64_t aPacketEpoch) noexcept
{
    if (aSenderPlayerId == 0 || aAuthoritativeOwnerPlayerId != aSenderPlayerId)
        return false;

    if (aPacketEpoch != 0 && aPacketEpoch != aAuthoritativeEpoch)
        return false;

    return true;
}

// Monotonic death & authoritative damage guard on ownership claim:
// 1. A dead actor on the server must NEVER be resurrected by a new owner's stale local snapshot.
// 2. If the server already tracks wounded health > 0 and < claimedHealth on a live actor, preserve the server's wounded HP
//    so a party leader walking into a boss room never heals a half-dead boss back to full HP.
inline void ReconcileClaimedState(
    bool aServerIsDead,
    float aServerCurrentHealth,
    bool aHasAuthoritativeHealth,
    bool& aInOutClaimedIsDead,
    float& aInOutClaimedHealth) noexcept
{
    if (aServerIsDead)
    {
        aInOutClaimedIsDead = true;
        aInOutClaimedHealth = 0.0f;
        return;
    }

    if (aHasAuthoritativeHealth && !aInOutClaimedIsDead && aServerCurrentHealth > 0.0f && aServerCurrentHealth < aInOutClaimedHealth)
    {
        aInOutClaimedHealth = aServerCurrentHealth;
    }
}

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

// Client-side per-actor monotonic epoch tracker preventing out-of-order Grant/Relinquish packets
// from leaving TWO simultaneous owners or ZERO owners on the wire.
class ClientEpochTracker
{
public:
    bool AcceptGrant(uint32_t aServerId, uint64_t aEpoch) noexcept
    {
        if (aEpoch == 0)
            return true;

        auto& known = m_knownEpochs[aServerId];
        if (aEpoch < known)
            return false;

        known = aEpoch;
        return true;
    }

    bool AcceptRelinquish(uint32_t aServerId, uint64_t aEpoch) noexcept
    {
        if (aEpoch == 0)
            return true;

        auto& known = m_knownEpochs[aServerId];
        if (aEpoch < known)
            return false;

        known = aEpoch;
        return true;
    }

    [[nodiscard]] uint64_t GetEpoch(uint32_t aServerId) const noexcept
    {
        const auto it = m_knownEpochs.find(aServerId);
        return it != m_knownEpochs.end() ? it->second : 0;
    }

    void Forget(uint32_t aServerId) noexcept { m_knownEpochs.erase(aServerId); }
    void Clear() noexcept { m_knownEpochs.clear(); }

private:
    std::unordered_map<uint32_t, uint64_t> m_knownEpochs;
};
} // namespace CoopOwnership
