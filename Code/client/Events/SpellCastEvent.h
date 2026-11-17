#pragma once
#include <Games/Magic/MagicSystem.h>
#include <cstdint>

// Snapshot the caster identity and casting properties inside the engine callback, not its
// transient ActorMagicCaster*. A spell can finish or its caster unload before the next frame.
struct SpellCastEvent
{
    uint32_t CasterId{}, SpellId{}, DesiredTargetID{};
    MagicSystem::CastingSource Source{};
    bool DualCasting{};
};
