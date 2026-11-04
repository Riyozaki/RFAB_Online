#pragma once

/**
 * Pure decision logic for the RFAB downed-on-knee / ally-revive mechanic.
 *
 * DownedStateService (Code/client/Services) used to keep these decisions inline, mixed
 * with engine calls (TESForm lookups, ForceActorValue, transport.Send, HUD messages), so
 * none of them could be exercised without a full Windows build and a live game. This is
 * the same split PR #17 applied to ownership (Authority::Lease) and saves (Save::Coordinator):
 * the rules live here as values-in / values-out, the service only performs the engine calls
 * the rules ask for.
 *
 * What is modelled:
 *   - ShouldBlockHealthMutation: while on the knee, freeze every heal AND every poison/DoT
 *     tick; during the post-revive grace, block only damage; an authorized revive heal passes.
 *   - SelectNearestDownedAlly: pick the closest downed party member within reach.
 *   - ReviveChannel: the hold-E channel state machine (start / continue / retarget / cancel),
 *     including clearing when the server confirms the revive or the target is lost.
 *   - ComputeReviveHeal: the 30%-style post-revive heal target and delta.
 *   - ClampGraceSec: the post-revive invulnerability window bound.
 *
 * The behaviour here is intentionally identical to what the service did before the split;
 * the accompanying test (rfab_downed_revive.cpp) pins it so the extraction is provably
 * faithful and future edits cannot silently change the mechanic.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Downed
{

// ============================================================================
// Health mutation gate
// ============================================================================
// While downed, both positive (regen/potions/spells) and negative (poison/bleed/ground-fire)
// health deltas are frozen so the player stays at exactly the knee HP. During the post-revive
// grace only damage is blocked (heals still land). An authorized revive heal always passes.
[[nodiscard]] inline bool ShouldBlockHealthMutation(
    bool aSystemActive,
    bool aAuthorizedHeal,
    bool aIsDowned,
    bool aGraceActive,
    float aDelta) noexcept
{
    if (!aSystemActive)
        return false;
    if (aAuthorizedHeal)
        return false;
    if (aIsDowned)
        return aDelta != 0.0f;
    if (aGraceActive && aDelta < 0.0f)
        return true;
    return false;
}

// ============================================================================
// Nearest downed ally selection
// ============================================================================
struct DownedCandidate
{
    uint32_t PlayerId{0};
    float X{0.0f};
    float Y{0.0f};
    float Z{0.0f};
    bool IsDowned{false};
};

// Returns the PlayerId of the closest downed candidate within aMaxRangeUnits, or 0 if none.
// The caller excludes the local player from the candidate list. A candidate exactly at the
// range boundary is included; equidistant candidates resolve to the last one seen (matches
// the original `distSq <= bestDistSq` scan).
[[nodiscard]] inline uint32_t SelectNearestDownedAlly(
    float aLocalX,
    float aLocalY,
    float aLocalZ,
    float aMaxRangeUnits,
    const DownedCandidate* apCandidates,
    size_t aCount) noexcept
{
    uint32_t best = 0;
    float bestDistSq = aMaxRangeUnits * aMaxRangeUnits;
    for (size_t i = 0; i < aCount; ++i)
    {
        const DownedCandidate& c = apCandidates[i];
        if (c.PlayerId == 0 || !c.IsDowned)
            continue;
        const float dx = c.X - aLocalX;
        const float dy = c.Y - aLocalY;
        const float dz = c.Z - aLocalZ;
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq <= bestDistSq)
        {
            bestDistSq = distSq;
            best = c.PlayerId;
        }
    }
    return best;
}

// ============================================================================
// Revive channel state machine
// ============================================================================
enum class ChannelAction : uint8_t
{
    None,   // nothing to send (idle, or a continue tick)
    Start,  // began (or retargeted) the channel -> send StartChannel(TargetPlayerId)
    Cancel, // released the key or lost the target -> send CancelChannel(TargetPlayerId)
};

struct ChannelUpdate
{
    ChannelAction Action{ChannelAction::None};
    uint32_t TargetPlayerId{0};
};

// The hold-E interactive revive channel. The client accumulates elapsed time for the progress
// bar and tells the server when a channel starts or is cancelled; the server is authoritative
// for when the revive actually completes (NotifyReviveState), which clears the channel via
// ClearIfTarget. Retargeting sends a fresh StartChannel for the new target (the server replaces
// the pending channel), exactly as the original service did.
struct ReviveChannel
{
    bool Active{false};
    uint32_t TargetPlayerId{0};
    double ElapsedSec{0.0};
    double DurationSec{4.0};

    ChannelUpdate Update(uint32_t aNearestTargetId, bool aKeyHeld, double aDeltaSec, double aConfiguredDurationSec) noexcept
    {
        if (aNearestTargetId != 0 && aKeyHeld)
        {
            if (!Active || TargetPlayerId != aNearestTargetId)
            {
                Active = true;
                TargetPlayerId = aNearestTargetId;
                ElapsedSec = 0.0;
                DurationSec = aConfiguredDurationSec;
                return {ChannelAction::Start, aNearestTargetId};
            }
            ElapsedSec += aDeltaSec;
            return {ChannelAction::None, TargetPlayerId};
        }

        if (Active)
        {
            const uint32_t previous = TargetPlayerId;
            Reset();
            return {ChannelAction::Cancel, previous};
        }

        return {};
    }

    void Reset() noexcept
    {
        Active = false;
        TargetPlayerId = 0;
        ElapsedSec = 0.0;
    }

    // Clears the channel only if it is currently aimed at aPlayerId (server confirmed the
    // revive/cancel, or the target went away). Returns whether the channel was cleared.
    bool ClearIfTarget(uint32_t aPlayerId) noexcept
    {
        if (Active && TargetPlayerId == aPlayerId)
        {
            Reset();
            return true;
        }
        return false;
    }

    [[nodiscard]] float Progress() const noexcept
    {
        if (!Active || DurationSec <= 0.05)
            return 0.0f;
        return static_cast<float>(std::clamp(ElapsedSec / DurationSec, 0.0, 1.0));
    }
};

// ============================================================================
// Post-revive heal
// ============================================================================
struct ReviveHeal
{
    float TargetHp{0.0f};
    float HealDelta{0.0f};
};

// Computes the HP to restore on revive: a fraction of the effective max (permanent + temporary
// modifier), floored so the player always leaves bleedout above 0. The heal delta never damages
// (if the player is already above the target, delta is 0).
[[nodiscard]] inline ReviveHeal ComputeReviveHeal(
    float aPermanentMax,
    float aTemporaryModifier,
    float aCurrentHp,
    float aHealthFraction) noexcept
{
    const float maxHp = std::max(10.0f, aPermanentMax + aTemporaryModifier);
    const float targetHp = std::max(1.0f, maxHp * std::clamp(aHealthFraction, 0.05f, 1.0f));
    const float healDelta = std::max(0.0f, targetHp - aCurrentHp);
    return {targetHp, healDelta};
}

// ============================================================================
// Post-revive grace window
// ============================================================================
[[nodiscard]] inline double ClampGraceSec(double aGraceSec) noexcept
{
    return std::clamp(aGraceSec, 0.0, 15.0);
}

} // namespace Downed
