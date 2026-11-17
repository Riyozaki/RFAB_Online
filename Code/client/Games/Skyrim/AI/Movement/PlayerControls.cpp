
#include "AI/Movement/PlayerControls.h"

#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>
#include <Services/InteractionInput.h>

struct InputEvent;

PlayerControls* PlayerControls::GetInstance()
{
    POINTER_SKYRIMSE(PlayerControls*, s_instance, 514706);
    return *(s_instance.Get());
}

void PlayerControls::SetActivateEnabled(bool abFlag)
{
    TP_THIS_FUNCTION(TSetActivateEnabled, void, PlayerControls, bool);
    POINTER_SKYRIMSE(TSetActivateEnabled, setActivateEnabled, 41266);
    TiltedPhoques::ThisCall(setActivateEnabled, this, abFlag);
}

void PlayerControls::SetCamSwitch(bool aSet) noexcept
{
    Data.remapMode = aSet;
}

BSInputEnableManager* BSInputEnableManager::Get()
{
    POINTER_SKYRIMSE(BSInputEnableManager*, s_instance, 514705);
    return *(s_instance.Get());
}

bool BSInputEnableManager::IsMovementControlsEnabled() noexcept
{
    using TIsMovementControlsEnabled = bool();
    POINTER_SKYRIMSE(TIsMovementControlsEnabled, s_isMovementControlsEnabled, 54852);
    return s_isMovementControlsEnabled.Get()();
}

void BSInputEnableManager::EnableOtherEvent(int32_t aFlags, bool aEnable, bool aUnk2)
{
    TP_THIS_FUNCTION(TSetEnabled, void, BSInputEnableManager, int32_t, bool, bool);
    POINTER_SKYRIMSE(TSetEnabled, setEnabled, 67245);
    TiltedPhoques::ThisCall(setEnabled, this, aFlags, aEnable, aUnk2);
}

// static TiltedPhoques::Initializer s_Init([]() { TiltedPhoques::Put<uint8_t>(0x14072E490, 0xCC); });

TP_THIS_FUNCTION(TProcessInputEvent, uint32_t, PlayerControls, InputEvent* const* apEvents, void* apSource);
static TProcessInputEvent* RealProcessInputEvent = nullptr;

static uint32_t TP_MAKE_THISCALL(HookProcessInputEvent, PlayerControls, InputEvent* const* apEvents, void* apSource)
{
    InteractionInput::Observe(apEvents ? *apEvents : nullptr);
    // An actor-local atomic gate, not EnablePlayerControls/DisablePlayerControls: never
    // overwrite RFAB/quest input-layer state when a coordinated recovery releases our gate.
    auto* player = PlayerCharacter::Get();
    if (player && player->GetExtension()->CoopInputBlocked.load(std::memory_order_relaxed))
    {
        InputEvent* noEvents = nullptr;
        return TiltedPhoques::ThisCall(RealProcessInputEvent, apThis, &noEvents, apSource);
    }
    UI* pUI = UI::Get();
    if (pUI && UI::ShouldUnpauseMenus() && pUI->IsUnpausedMenuOpen())
    {
        InputEvent* pNoEvents = nullptr;
        return TiltedPhoques::ThisCall(RealProcessInputEvent, apThis, &pNoEvents, apSource);
    }

    return TiltedPhoques::ThisCall(RealProcessInputEvent, apThis, apEvents, apSource);
}

static TiltedPhoques::Initializer s_playerControlsHooks(
    []()
    {
        POINTER_SKYRIMSE(TProcessInputEvent, s_processInputEvent, 41288);

        RealProcessInputEvent = s_processInputEvent.Get();

        TP_HOOK(&RealProcessInputEvent, HookProcessInputEvent);
    });
