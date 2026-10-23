#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/ActionEvent.h>

struct LocalComponent
{
    LocalComponent(uint32_t aId, uint64_t aOwnershipEpoch = 0) noexcept
        : Id(aId)
        , OwnershipEpoch(aOwnershipEpoch)
    {
    }

    uint32_t Id;
    uint64_t OwnershipEpoch = 0;
    ActionEvent CurrentAction;
    bool IsDead = false;
    bool IsWeaponDrawn = false;
};
