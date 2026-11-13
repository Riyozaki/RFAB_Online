#pragma once

// Who keeps which menu paused while the co-op client runs (architecture step 5).
//
// Skyrim Together unpauses a short list of menus (inventory, magic, Tab...) so a player browsing
// a menu does not freeze their simulation while the others keep playing. Two modpack facts change
// that:
//
//   * Skyrim Souls RE already unpauses menus and does its own accounting (UI::numPausesGame,
//     MenuFlagEx::kUnpaused) in a wrapper around the menu creator. If Together also strips
//     kPausesGame, Souls' accounting breaks (Tab locks up, StatsMenu crashes). With Souls loaded,
//     Souls owns pausing and Together touches no menu flags and patches no menu input code.
//   * CustomSkills / RFAB.dll (RFAB's perk trees) need StatsMenu PAUSED: the skydome switch and
//     RFAB's PerkPlacementMenuOpenCloseManager race an unpaused world. Then StatsMenu is never
//     unpaused and its input code is never patched.
//
// Before this header the same decision was re-derived in three places (the menu creator wrapper,
// the vanilla StatsMenu patches and the Souls INI patcher), one of which also applied a code patch
// to RFAB.dll as a side effect of a predicate. Now it is one pure function, decided once after
// SKSE has loaded its plugins and logged; the only later input is plugin evidence that can only
// make StatsMenu stay paused, never the other way round.

#include <cstdint>
#include <string>
#include <string_view>

namespace Compat
{
struct MenuEnvironment
{
    bool SkyrimSoulsLoaded{false};       // SkyrimSoulsRE.dll is in the process
    bool PerkTreeModsPresent{false};     // CustomSkills.dll / RFAB.dll / their Data files / RFAB plugins
};

enum class MenuPauseOwner : uint8_t
{
    kSkyrimSouls, // Souls unpauses; Together never touches menu flags
    kTogether,    // Together unpauses its allow-list itself
};

struct MenuPausePolicy
{
    MenuPauseOwner Owner{MenuPauseOwner::kTogether};
    bool StatsMenuStaysPaused{false};
    // Vanilla input patches that make an unpaused menu usable; only when Together owns pausing.
    bool PatchStatsMenuInput{false};   // StatsMenu::ProcessMessage 51638 / CanProcess 51645
    bool PatchFavoritesInput{false};   // FavoritesMenu::CanProcess 50644

    /** May Together clear kPausesGame / freeze flags on this (allow-listed) menu? */
    [[nodiscard]] bool TogetherUnpauses(std::string_view aMenuName) const noexcept
    {
        if (Owner != MenuPauseOwner::kTogether)
            return false;
        if (aMenuName == "StatsMenu" && StatsMenuStaysPaused)
            return false;
        return true;
    }

    /**
     * RFAB's perk trees found after the decision (e.g. RFAB.esp seen once the plugin list loaded).
     * Only tightens: StatsMenu stays paused from now on. The StatsMenu input patch, if already
     * applied, stays applied - it only matters for an unpaused StatsMenu, which no longer happens.
     */
    bool NoteLatePerkTreeEvidence() noexcept
    {
        if (StatsMenuStaysPaused)
            return false;
        StatsMenuStaysPaused = true;
        return true;
    }

    [[nodiscard]] std::string Describe() const
    {
        std::string text = Owner == MenuPauseOwner::kSkyrimSouls ? "menus paused/unpaused by Skyrim Souls RE"
                                                                  : "menus unpaused by RFAB Online";
        text += StatsMenuStaysPaused ? "; StatsMenu stays paused (perk trees)" : "; StatsMenu unpaused";
        if (PatchStatsMenuInput)
            text += "; StatsMenu input patched";
        if (PatchFavoritesInput)
            text += "; FavoritesMenu input patched";
        return text;
    }
};

[[nodiscard]] constexpr MenuPausePolicy DecideMenuPausePolicy(const MenuEnvironment& acEnvironment) noexcept
{
    MenuPausePolicy policy{};
    if (acEnvironment.SkyrimSoulsLoaded)
    {
        policy.Owner = MenuPauseOwner::kSkyrimSouls;
        // What Souls does with StatsMenu is its INI's business; the co-op overlay keeps
        // bStatsMenu = false (RFAB's own value), so for reporting it stays paused.
        policy.StatsMenuStaysPaused = true;
        return policy;
    }
    policy.Owner = MenuPauseOwner::kTogether;
    policy.PatchFavoritesInput = true;
    policy.StatsMenuStaysPaused = acEnvironment.PerkTreeModsPresent;
    policy.PatchStatsMenuInput = !acEnvironment.PerkTreeModsPresent;
    return policy;
}
} // namespace Compat
