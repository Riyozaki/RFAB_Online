#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/ActorValues.h>

struct ActorValuesComponent
{
    ActorValues CurrentActorValues{};

    // Form id of the game actor, used to restore values on disconnect.
    uint32_t FormId{0};

    // Unscaled maximum health, captured before the server applied party-size
    // health scaling. 0 means "never scaled".
    float BaseMaxHealth{0.0f};
};
