#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

struct WaitingForAssignmentComponent
{
    uint32_t Cookie{};
    bool DirtyInventory{false};
    float PendingHealthDelta{0.0f};
};
