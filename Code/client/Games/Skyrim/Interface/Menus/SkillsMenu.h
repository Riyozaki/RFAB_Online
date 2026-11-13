#pragma once

// StatsMenu ("Skills") support code. Whether StatsMenu is unpaused at all is decided once, in
// Interface/UI.cpp, by Compat::DecideMenuPausePolicy (common/Compat/MenuPausePolicy.h); these
// functions only provide its inputs and the patches it may ask for.

// CustomSkills / RFAB.dll present (module loaded or their files under Data/)? Call after SKSE has
// loaded its plugins.
bool HasPerkTreeModsPostSkse() noexcept;

// RFAB's plugins in the loaded plugin list (RFAB.esp, [RFAB] *.esp, Extended UI.esp)? False until
// ModManager is populated; used as late evidence that can only keep StatsMenu paused.
bool HasPerkTreePluginsLoaded() noexcept;

// Runtime in-memory guard for RFAB.dll's PerkPlacementMenuOpenCloseManager::ProcessEvent
// unbounded ActorValueList::actorValues loop (+0x3CFE3 BSTArray copy-ctor AV when index >= 164).
// Independent of pausing: applied whenever RFAB.dll is loaded. Works across unpatched RFAB.dll
// builds, future RFAB updates with shifted RVAs, and leaves already disk-patched binaries untouched.
bool ApplyRfabDllPerkPlacementBoundsFixPostSkse() noexcept;

// Vanilla StatsMenu input patches (ProcessMessage 51638 / CanProcess 51645) that make an UNPAUSED
// StatsMenu usable. Only when the menu pause policy asks for them.
void ApplyStatsMenuInputPatches() noexcept;
