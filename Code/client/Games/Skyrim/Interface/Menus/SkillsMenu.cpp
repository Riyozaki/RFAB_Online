#include <Games/BytePatchGuard.h>

// The game calls it statsmenu.
#include <Interface/Menus/SkillsMenu.h>

static TiltedPhoques::Initializer s_skillsMenuInit(
    []()
    {
        // https://github.com/Vermunds/SkyrimSoulsRE/blob/master/src/Menus/StatsMenuEx.cpp
        // Hoooks from souls RE
        // Fix for menu not appearing
        VersionDbPtr<uint8_t> ProcessMessage(52510);
        if (CanApplyInsideFunctionPatch("SkillsMenu/ProcessMessage freeze frame"))
        {
            TiltedPhoques::Nop(ProcessMessage.Get() + 0x84E, 6);
            // Prevent setting kFreezeFrameBackground flag
            TiltedPhoques::Nop(ProcessMessage.Get() + 0xA10, 4);
            // Keep the menu updated
            TiltedPhoques::Nop(ProcessMessage.Get() + 0x1040, 2);
        }

        // Fix for controls not working
        VersionDbPtr<uint8_t> controlPatch(52518);
        if (CanApplyInsideFunctionPatch("SkillsMenu/controls"))
        {
            TiltedPhoques::Nop(controlPatch.Get() + 0x46, 4);
            TiltedPhoques::Nop(controlPatch.Get() + 0x4A, 2);
        }
    });
