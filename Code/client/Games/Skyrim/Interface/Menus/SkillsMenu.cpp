
// The game calls it statsmenu.
#include <Interface/Menus/SkillsMenu.h>
#include "immersive_launcher/stubs/DllBlocklist.h"

#include <Windows.h>

static TiltedPhoques::Initializer s_skillsMenuInit(
    []()
    {
        // If SkyrimSoulsRE.dll (v2.1.x for SE 1.5.97) is loaded in the modpack (e.g. RFAB),
        // it installs these exact StatsMenu patches itself — do not patch the same bytes twice.
        if (stubs::g_IsSoulsREActive || GetModuleHandleW(L"SkyrimSoulsRE.dll") != nullptr)
            return;

        // Verified against SkyrimSoulsRE v2.1.3 (src/Menus/StatsMenuEx.cpp) for Skyrim SE 1.5.97:
        // StatsMenu::ProcessMessage = SE 51638 (0x8BF360)
        VersionDbPtr<uint8_t> ProcessMessage(51638);
        // Fix for menu not appearing (+0x929, 6 bytes)
        TiltedPhoques::Nop(ProcessMessage.Get() + 0x929, 6);
        // Prevent setting kFreezeFrameBackground flag (+0xAEC, 4 bytes)
        TiltedPhoques::Nop(ProcessMessage.Get() + 0xAEC, 4);
        // Keep the menu updated (+0xFBC, 2 bytes)
        TiltedPhoques::Nop(ProcessMessage.Get() + 0xFBC, 2);

        // StatsMenu::CanProcess = SE 51645 (0x8C0B80)
        // Fix for controls not working (+0x46 4 bytes, +0x4A 2 bytes)
        VersionDbPtr<uint8_t> controlPatch(51645);
        TiltedPhoques::Nop(controlPatch.Get() + 0x46, 4);
        TiltedPhoques::Nop(controlPatch.Get() + 0x4A, 2);
    });
