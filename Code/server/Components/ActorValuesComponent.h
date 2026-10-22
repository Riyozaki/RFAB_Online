#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/ActorValues.h>

struct ActorValuesComponent
{
    ActorValues CurrentActorValues{};

    // Unscaled (vanilla) maximum health of this actor, captured the first time
    // the server sees it. Party-size health scaling multiplies this value and
    // never mutates it, so the operation stays exact and reversible
    // (see HealthScalingService).
    // <= 0 means "not captured yet".
    float BaseMaxHealth{0.0f};
};
