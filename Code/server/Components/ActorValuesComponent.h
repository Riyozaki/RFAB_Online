#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/ActorValues.h>

#include <Authority/Health.h>

#include <algorithm>
#include <cmath>

struct ActorValuesComponent
{
    static constexpr uint32_t kHealthActorValueIndex = 24;
    static constexpr uint32_t kMaxHealthActorValueIndex = 24;

    ActorValues CurrentActorValues{};

    // Bumped every time this actor's health changes. Clients echo the revision they had
    // when they read their values, which is how the server tells a fresh absolute reading
    // from one that was sampled before the change it is about to undo.
    uint32_t HealthRevision{0};

    [[nodiscard]] bool TryGetHealth(float& aOut) const noexcept
    {
        const auto it = CurrentActorValues.ActorValuesList.find(kHealthActorValueIndex);
        if (it == CurrentActorValues.ActorValuesList.end() || !std::isfinite(it->second))
            return false;
        aOut = it->second;
        return true;
    }

    [[nodiscard]] bool TryGetMaxHealth(float& aOut) const noexcept
    {
        const auto it = CurrentActorValues.ActorMaxValuesList.find(kMaxHealthActorValueIndex);
        if (it == CurrentActorValues.ActorMaxValuesList.end() || !std::isfinite(it->second) || it->second <= 0.0f)
            return false;
        aOut = it->second;
        return true;
    }

    // The ONLY place authoritative health is written.
    //
    // Every caller - delta application, death, ownership handoff, saved value restore -
    // goes through here, because the value and the revision that describes it are one
    // piece of state. Writing the number without moving the revision is what let a stale
    // snapshot resurrect a dead actor; there is no longer a way to do that by hand.
    void SetHealth(float aTargetHp, float aMaxHealth = 0.0f) noexcept
    {
        float currentHp = 0.0f;
        if (!TryGetHealth(currentHp))
        {
            float storedMax = 0.0f;
            currentHp = TryGetMaxHealth(storedMax) ? storedMax : 0.0f;
        }

        float maxHealth = aMaxHealth;
        if (!(std::isfinite(maxHealth) && maxHealth > 0.0f))
            (void)TryGetMaxHealth(maxHealth);

        const auto written = Authority::ApplyAbsoluteHealth(currentHp, aTargetHp, maxHealth, HealthRevision);
        CurrentActorValues.ActorValuesList[kHealthActorValueIndex] = written.Value;
        HealthRevision = written.Revision;
    }
};
