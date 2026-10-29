#pragma once

/**
 * @brief In-game activation (i.e. interacting with an object).
 *
 * Stores pure scalar FormIDs rather than raw `TESObjectREFR*` / `Actor*` / `TESBoundObject*`
 * pointers so deleting/harvesting an object inside `RealActivate` or unloading its cell before
 * `RunnerService::OnUpdate` drains on `WinMain` can never cause a use-after-free.
 */
struct ActivateEvent
{
    ActivateEvent(
        uint32_t aObjectId,
        uint32_t aActivatorId,
        uint32_t aObjectToGetId,
        int32_t aCount,
        bool aDefaultProcessing,
        uint8_t aUnk1,
        TESObjectREFR::OpenState aPreActivationOpenState,
        bool aActivateFlag = false) noexcept
        : ObjectId(aObjectId)
        , ActivatorId(aActivatorId)
        , ObjectToGetId(aObjectToGetId)
        , Count(aCount)
        , DefaultProcessing(aDefaultProcessing)
        , Unk1(aUnk1)
        , PreActivationOpenState(aPreActivationOpenState)
        , ActivateFlag(aActivateFlag)
    {
    }

    uint32_t ObjectId{0};
    uint32_t ActivatorId{0};
    uint32_t ObjectToGetId{0};
    int32_t Count{0};
    bool DefaultProcessing{false};
    uint8_t Unk1{0};
    TESObjectREFR::OpenState PreActivationOpenState{TESObjectREFR::kNone};
    bool ActivateFlag{false};
};
