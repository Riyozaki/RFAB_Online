#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/ActorValues.h>

struct ActorValuesComponent
{
    ActorValues CurrentActorValues{};

    // Bumped every time this actor's health goes down. Clients echo the revision they had
    // when they read their values, which is how the server tells a fresh absolute reading
    // from one that was sampled before the hit it is about to undo.
    uint32_t HealthRevision{0};
};
