#pragma once

// Runtime check (must only be called AFTER SKSE plugins have loaded or during gameplay)
// that returns true whenever CustomSkills, Extended UI, or any version of RFAB is active
// so StatsMenu remains paused and its skydome / perk-tree hooks stay crash-free.
bool ShouldKeepStatsMenuPausedRuntime() noexcept;

// Runtime in-memory guard for RFAB.dll's PerkPlacementMenuOpenCloseManager::ProcessEvent
// unbounded ActorValueList::actorValues loop (+0x3CFE3 BSTArray copy-ctor AV when index >= 164).
// Works across unpatched RFAB.dll builds, future RFAB updates with shifted RVAs, and leaves
// already disk-patched RFAB.dll binaries untouched.
bool ApplyRfabDllPerkPlacementBoundsFixPostSkse() noexcept;

// Post-SKSE initialization entry point for vanilla fallback StatsMenu patches (no-op when RFAB/CustomSkills/SoulsRE is active).
void ApplyFallbackStatsMenuPatchesPostSkse() noexcept;
