#pragma once
#include <cstdint>
#include <TESObjectREFR.h>

// Engine references can unload before the runner executes. Carry identities only and resolve
// them again in ObjectService. ObjectToGet is also an id (base forms are process-lived, but the
// event does not need to own a native pointer).
struct ActivateEvent
{
    uint32_t ObjectId{}, ActivatorId{}, ObjectToGetId{};
    int32_t Count{};
    bool DefaultProcessing{};
    uint8_t Unk1{};
    TESObjectREFR::OpenState PreActivationOpenState{TESObjectREFR::kNone};
    bool ActivateFlag{};
};
