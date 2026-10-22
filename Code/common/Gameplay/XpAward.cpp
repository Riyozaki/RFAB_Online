#include <Gameplay/XpAward.h>

namespace Gameplay::XpAward
{
Result Apply(int32_t aCurrentTotal, int32_t aAward) noexcept
{
    Result result{};

    // A negative or nonsensical stored total is treated as zero instead of
    // propagating a negative experience bar into the UI.
    result.Previous = aCurrentTotal > 0 ? aCurrentTotal : 0;
    if (result.Previous > kMaxTotalXp)
    {
        // A corrupted global (or a save from a future version) must not turn
        // into a negative "applied" delta.
        result.Previous = kMaxTotalXp;
        result.Clamped = true;
    }
    result.Total = result.Previous;

    if (aAward <= 0 || aAward > kMaxSingleAward)
    {
        result.Ignored = true;
        return result;
    }

    const int64_t wanted = static_cast<int64_t>(result.Previous) + static_cast<int64_t>(aAward);
    if (wanted >= kMaxTotalXp)
    {
        result.Total = kMaxTotalXp;
        result.Applied = kMaxTotalXp > result.Previous ? kMaxTotalXp - result.Previous : 0;
        result.Clamped = true;
        return result;
    }

    result.Total = static_cast<int32_t>(wanted);
    result.Applied = aAward;
    return result;
}

bool ShouldAnnounceLevelUp(int32_t aTotal, float aLevelUpThreshold) noexcept
{
    if (aLevelUpThreshold <= 0.0f)
        return false;

    // The vanilla check is `_PlayerXP >= Game.GetExperienceForLevel(level)`.
    return static_cast<float>(aTotal) >= aLevelUpThreshold;
}

} // namespace Gameplay::XpAward
