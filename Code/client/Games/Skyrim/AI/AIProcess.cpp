#include "AIProcess.h"

bhkCharacterController* AIProcess::GetCharController() noexcept
{
    if (!middleProcess)
        return nullptr;

    // SE 1.5.97 has no distinct Address Library ID for AE 39856 (AIProcess::GetCharController).
    // MiddleHighProcessData::charController (NiPointer<bhkCharacterController>) is at +0x250.
    return *reinterpret_cast<bhkCharacterController* const*>(reinterpret_cast<const uint8_t*>(middleProcess) + 0x250);
}

void AIProcess::SetHeadTrackTarget(Actor* apOwner, const NiPoint3& acTarget) noexcept
{
    TP_THIS_FUNCTION(TSetHeadTrackTarget, void, AIProcess, Actor*, const NiPoint3&);
    POINTER_SKYRIMSE(TSetHeadTrackTarget, setHeadTrackTarget, 38850);
    TiltedPhoques::ThisCall(setHeadTrackTarget, this, apOwner, acTarget);
}

void AIProcess::KnockExplosion(Actor* apActor, const NiPoint3* aSourceLocation, float afMagnitude)
{
    TP_THIS_FUNCTION(TKnockExplosion, void, AIProcess, Actor*, const NiPoint3*, float);
    POINTER_SKYRIMSE(TKnockExplosion, knockExplosion, 38858);
    TiltedPhoques::ThisCall(knockExplosion, this, apActor, aSourceLocation, afMagnitude);
}
