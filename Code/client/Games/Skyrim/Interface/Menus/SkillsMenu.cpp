
// The game calls it statsmenu.
#include <Interface/Menus/SkillsMenu.h>
#include "immersive_launcher/stubs/DllBlocklist.h"

#include <Windows.h>

void ApplyFallbackStatsMenuPatchesPostSkse() noexcept
{
    static bool s_checkedPostSkse = false;
    if (s_checkedPostSkse)
        return;
    s_checkedPostSkse = true;

    // Must run AFTER StartSKSE() loads SKSE plugins (never inside TiltedPhoques::Initializer at [BOOT 3.1/4]).
    // When SkyrimSoulsRE.dll, CustomSkills.dll, or RFAB.dll is loaded, never patch StatsMenu::ProcessMessage
    // or StatsMenu::CanProcess: RFAB's SkyrimSoulsRE.ini intentionally sets bStatsMenu = false so CustomSkills
    // and RFAB PerkPlacementMenuOpenCloseManager run safely on a paused StatsMenu.
    const bool hasSoulsRE = stubs::g_IsSoulsREActive || GetModuleHandleW(L"SkyrimSoulsRE.dll") != nullptr;
    const bool hasCustomSkillsOrRfab =
        GetModuleHandleW(L"CustomSkills.dll") != nullptr || GetModuleHandleW(L"RFAB.dll") != nullptr;
    if (hasSoulsRE || hasCustomSkillsOrRfab)
        return;

    // Vanilla fallback (only when neither SkyrimSoulsRE nor CustomSkills/RFAB is loaded):
    // StatsMenu::ProcessMessage = SE 51638 (0x8BF360)
    VersionDbPtr<uint8_t> ProcessMessage(51638);
    TiltedPhoques::Nop(ProcessMessage.Get() + 0x929, 6);
    TiltedPhoques::Nop(ProcessMessage.Get() + 0xAEC, 4);
    TiltedPhoques::Nop(ProcessMessage.Get() + 0xFBC, 2);

    // StatsMenu::CanProcess = SE 51645 (0x8C0B80)
    VersionDbPtr<uint8_t> controlPatch(51645);
    TiltedPhoques::Nop(controlPatch.Get() + 0x46, 4);
    TiltedPhoques::Nop(controlPatch.Get() + 0x4A, 2);
}

