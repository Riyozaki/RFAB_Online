#pragma once

#include <cstdint>

struct TESGlobal;

/**
 * @brief The RFAB experience pipeline, from the multiplayer side.
 *
 * RFAB keeps its own experience in the `PlayerXP` GlobalVariable and mirrors it
 * into the vanilla bar. The Papyrus source (MO2/mods/RFAB/scripts/source/
 * RFAB_XP_Handler.psc) is tiny and fully documents the award:
 *
 *   Function ModXP(int aiXP)
 *       PlayerXP.Mod(aiXP)                                  ; GlobalVariable f += aiXP
 *       VanilaXPGain(aiXP)                                  ; see below
 *   EndFunction
 *
 *   Function VanilaXPGain(int aiXPAmount)
 *       float _PlayerXP = Game.GetPlayerExperience() + aiXPAmount
 *       Game.SetPlayerExperience(_PlayerXP)
 *       if _PlayerXP >= Game.GetExperienceForLevel(Player.GetLevel()) && ShowLevelUp
 *           ShowLevelUp = false
 *           ShowLevelUp()
 *       endif
 *   EndFunction
 *
 * A Papyrus *function* cannot be invoked from native code without building an
 * `IFunction::Call` stack (the repo has no such path, and it cannot be tested
 * here), so `Award` performs exactly the two effects ModXP performs, in the same
 * order, with the same arithmetic. The interface is deliberately the same as
 * ModXP: one integer amount in, one global out. If a native dispatch becomes
 * available, only the body of `Award` has to change.
 *
 * Local experience keeps flowing through RFAB's own `ModXP` (the Papyrus event
 * OnKill/OnBookRead call it); this module is used for experience that comes
 * from *other* players, so that a shared award lands on every client the same
 * way RFAB itself would award it.
 */
namespace RfabXp
{
//! Cached `RFAB.esp` form 0xAD45BB (the RFAB XP GlobalVariable), or nullptr.
TESGlobal* GetGlobal() noexcept;

//! Current RFAB total, rounded and clamped at zero (the global is a float).
int32_t Read() noexcept;

struct AwardResult
{
    bool Ok{false};        //!< false when the RFAB global or the VM is unavailable
    bool LevelUp{false};   //!< the award crossed the vanilla level-up threshold
    int32_t Total{0};      //!< new RFAB total (0 when !Ok)
};

/**
 * @brief Applies `aiXP` the way RFAB_XP_Handler.ModXP does.
 *
 * Never throws, never asserts, safe to call before the player exists (it then
 * simply reports failure). A non-positive amount is a no-op that still reports
 * success.
 */
AwardResult Award(int32_t aAmount) noexcept;
} // namespace RfabXp
