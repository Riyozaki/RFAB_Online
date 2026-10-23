
#include <Interface/Menus/MapMenu.h>

static TiltedPhoques::Initializer s_init(
    []()
    {
        // Disabled because the mapmenu in first person breaks.
        // SE 1.5.97 offset from SkyrimSoulsRE v2.1.3 (Offsets::Menus::MapMenu::LocalMapUpdaterFunc = SE 52225):
#if 0
        VersionDbPtr<void*> hookLoc(52225);
        TiltedPhoques::Nop(hookLoc.GetPtrU() + 0x53, 4);
        TiltedPhoques::Nop(hookLoc.GetPtrU() + 0x9D, 2);
        TiltedPhoques::Nop(hookLoc.GetPtrU() + 0x9F, 1);
#endif
    });
