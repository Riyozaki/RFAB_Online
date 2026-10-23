#pragma once

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
