
#include "AI/Movement/PlayerControls.h"

PlayerControls* PlayerControls::GetInstance()
{
    POINTER_SKYRIMSE(PlayerControls*, s_instance, 514706);
    return *(s_instance.Get());
}

void PlayerControls::SetEnabled(bool abFlag)
{
    TP_THIS_FUNCTION(TSetEnabled, void, PlayerControls, bool);
    POINTER_SKYRIMSE(TSetEnabled, setEnabled, 41266);
    TiltedPhoques::ThisCall(setEnabled, this, abFlag);
}

void PlayerControls::SetCamSwitch(bool aSet) noexcept
{
    Data.remapMode = aSet;
}

bool PlayerControls::IsMovementControlsEnabled() noexcept
{
    using TIsMovementControlsEnabled = bool();
    POINTER_SKYRIMSE(TIsMovementControlsEnabled, s_isMovementControlsEnabled, 54852);
    return s_isMovementControlsEnabled.Get()();
}

BSInputEnableManager* BSInputEnableManager::Get()
{
    POINTER_SKYRIMSE(BSInputEnableManager*, s_instance, 514705);
    return *(s_instance.Get());
}

void BSInputEnableManager::EnableOtherEvent(int32_t aFlags, bool aEnable, bool aUnk2)
{
    TP_THIS_FUNCTION(TSetEnabled, void, BSInputEnableManager, int32_t, bool, bool);
    POINTER_SKYRIMSE(TSetEnabled, setEnabled, 67245);
    TiltedPhoques::ThisCall(setEnabled, this, aFlags, aEnable, aUnk2);
}

// static TiltedPhoques::Initializer s_Init([]() { TiltedPhoques::Put<uint8_t>(0x14072E490, 0xCC); });
