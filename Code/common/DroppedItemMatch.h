#pragma once

// RFAB Online: choosing which temporary (0xFFxxxxxx) ground reference to despawn when a peer reports
// that it picked up a dropped item (Actor::DropOrPickUpObject, Count > 0).
//
// The protocol carries no identity for dropped references, so the client has to guess. Up to v4.0.0
// the guess was "nearest ref with the same base form within 2048 units (~29 m)", which happily deleted
// an unrelated stack - e.g. the local player's own identical potion/arrows lying a room away - and is
// one of the sources of item loss/duplication. The rule is now:
//   1. a ref whose stack count equals the picked-up count is strongly preferred;
//   2. such a ref must lie within kCountMatchRadius of the picker (activation range + remote lag);
//   3. a ref with a different count is only accepted inside the much tighter kFallbackRadius;
//   4. otherwise nothing is deleted (a leftover visual is far better than deleting a real item).

#include <cstdint>

namespace DroppedItemMatch
{
inline constexpr float kCountMatchRadius = 768.0f; // ~11 m: pickup reach + interpolation lag of the remote picker
inline constexpr float kFallbackRadius = 256.0f;   // ~3.6 m: practically "standing on it"

struct Candidate
{
    bool CountMatches{false};
    float DistanceSq{0.0f};
};

[[nodiscard]] constexpr bool IsEligible(const Candidate& acCandidate) noexcept
{
    const float cRadius = acCandidate.CountMatches ? kCountMatchRadius : kFallbackRadius;
    return acCandidate.DistanceSq >= 0.0f && acCandidate.DistanceSq < cRadius * cRadius;
}

[[nodiscard]] constexpr bool IsBetter(const Candidate& acCandidate, bool aHasBest, const Candidate& acBest) noexcept
{
    if (!IsEligible(acCandidate))
        return false;
    if (!aHasBest)
        return true;
    if (acCandidate.CountMatches != acBest.CountMatches)
        return acCandidate.CountMatches;
    return acCandidate.DistanceSq < acBest.DistanceSq;
}

[[nodiscard]] constexpr int32_t StackCountOrOne(bool aHasExtraCount, int32_t aExtraCount) noexcept
{
    return (aHasExtraCount && aExtraCount > 0) ? aExtraCount : 1;
}
} // namespace DroppedItemMatch
