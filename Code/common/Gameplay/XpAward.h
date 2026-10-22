#pragma once

#include <cstdint>

// Arithmetic shared by the client XP award path.
//
// RFAB awards experience in Papyrus:
//
//   Function ModXP(int aiXP)          ; MO2/mods/RFAB/scripts/source/RFAB_XP_Handler.psc
//       PlayerXP.Mod(aiXP)            ; GlobalVariable, stored as a float
//       VanilaXPGain(aiXP)            ; Game.SetPlayerExperience(GetPlayerExperience() + aiXP)
//   EndFunction
//
// The multiplayer path has to reproduce exactly these two effects on *remote*
// experience, and it has to survive garbage input (a hacked or buggy client, a
// float global that drifted). The pure part lives here so it can be tested
// without Skyrim.
namespace Gameplay::XpAward
{
//! A Papyrus GlobalVariable is a float: integers above 2^24 cannot be stored
//! exactly. 10 000 000 is comfortably below that and far above any real total.
inline constexpr int32_t kMaxTotalXp = 10'000'000;

//! Largest single award accepted from the network (matches the client cap).
inline constexpr int32_t kMaxSingleAward = 1'000'000;

struct Result
{
    int32_t Previous{0};  //!< total before the award
    int32_t Total{0};     //!< total after the award, clamped
    int32_t Applied{0};   //!< what actually had to be added (may be < requested)
    bool Clamped{false};  //!< true when the total hit kMaxTotalXp
    bool Ignored{false};  //!< true when the award was not applied at all
};

/**
 * @brief Computes the new experience total after an award.
 *
 * Negative or zero awards are ignored (the vanilla pipeline never subtracts),
 * awards above kMaxSingleAward are ignored rather than truncated, and the total
 * is clamped to kMaxTotalXp.
 */
Result Apply(int32_t aCurrentTotal, int32_t aAward) noexcept;

/**
 * @brief Whether a level-up notification should be shown.
 * @param aTotal new experience total
 * @param aLevelUpThreshold Game.GetExperienceForLevel(level); <= 0 disables
 */
bool ShouldAnnounceLevelUp(int32_t aTotal, float aLevelUpThreshold) noexcept;

} // namespace Gameplay::XpAward
