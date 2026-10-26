#pragma once

#include <cstdint>
#include <Games/Magic/MagicSystem.h>

/**
 * @brief Dispatched when a spell has been cast locally.
 * Stores pure scalar snapshot values rather than a raw ActorMagicCaster* so unloading/deleting
 * an actor before RunnerService::OnUpdate drains the queue can never cause a use-after-free.
 */
struct SpellCastEvent
{
    SpellCastEvent(
        uint32_t aCasterFormId,
        MagicSystem::CastingSource aCastingSource,
        bool aIsDualCasting,
        uint32_t aSpellId,
        uint32_t aDesiredTargetID) noexcept
        : CasterFormId(aCasterFormId)
        , CastingSource(aCastingSource)
        , IsDualCasting(aIsDualCasting)
        , SpellId(aSpellId)
        , DesiredTargetID(aDesiredTargetID)
    {
    }

    uint32_t CasterFormId{0};
    MagicSystem::CastingSource CastingSource{MagicSystem::CastingSource::LEFT_HAND};
    bool IsDualCasting{false};
    uint32_t SpellId{0};
    uint32_t DesiredTargetID{0};
};
