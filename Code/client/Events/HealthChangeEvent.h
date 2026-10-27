#pragma once

struct TESObjectREFR;

/**
 * @brief Dispatched when a local or remote actor's health changes locally.
 *
 * The exact hueristics for what hit is registered by who can be found in
 * the Actor::HookDamageActor() hook.
 */
struct HealthChangeEvent
{
    HealthChangeEvent(
        uint32_t aHitteeId,
        float aDeltaHealth,
        uint32_t aAttackerFormId = 0,
        float aStaggerMagnitude = 0.0f,
        float aStaggerDirection = 0.0f) noexcept
        : HitteeId(aHitteeId)
        , DeltaHealth(aDeltaHealth)
        , AttackerFormId(aAttackerFormId)
        , StaggerMagnitude(aStaggerMagnitude)
        , StaggerDirection(aStaggerDirection)
    {
    }

    uint32_t HitteeId{};
    float DeltaHealth{};
    uint32_t AttackerFormId{};
    float StaggerMagnitude{};
    float StaggerDirection{};
};
