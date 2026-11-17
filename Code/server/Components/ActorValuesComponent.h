#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/ActorValues.h>

struct ActorValuesComponent
{
    ActorValues CurrentActorValues{};
    // Per-origin reliable sequence, scoped to OwnershipEpoch. Never copied from a client snapshot.
    uint32_t HealthEpoch{};
};
