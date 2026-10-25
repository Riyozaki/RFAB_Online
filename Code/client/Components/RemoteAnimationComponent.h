#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

struct RemoteAnimationComponent
{
    List<ActionEvent> TimePoints;
    ActionEvent LastRanAction;
    ActionEvent LastProcessedAction;
    uint32_t ReplayCount{0};
    bool ResetAnimationGraphForReplay{false};
    bool PendingFurnitureGraphExit{false};
    bool InRemoteFurnitureIdle{false};
    uint64_t LastSitSleepNormalizeTick{0};
};
