#pragma once

#include <Games/Primitives.h>
#include <cmath>
#include <Havok/hkStepInfo.h>

struct bhkCharacterController : NiRefObject
{
    bool UpdateStepTiming(float aMovementDeltaTime = 0.f) noexcept;

    uint8_t pad10[0x80 - 0x10];
    hkStepInfo stepInfo;
    // Layout reference: CommonLibSSE-NG b93280e832f263dbef44e44cbe2936622a02f91a,
    // bhkCharacterController::surfaceInfo + hkpCharacterContext::currentState (SE layout).
    uint8_t pad90[0x1A0 - 0x90];
    uint32_t supportedState;
    uint8_t pad1A4[0x1B0 - 0x1A4];
    float supportNormal[4];
    float supportVelocity[4];
    float surfaceDistanceExcess;
    bool surfaceIsDynamic;
    uint8_t pad1D5[0x200 - 0x1D5];
    uint32_t characterState;
    uint8_t pad204[0x218 - 0x204];
    uint32_t characterFlags;

    bool IsSafeStaticGround() const noexcept
    {
        return supportedState == 2 && characterState == 0 && (characterFlags & (1u << 8)) &&
            !(characterFlags & (1u << 13)) && !surfaceIsDynamic &&
            std::isfinite(supportNormal[2]) && supportNormal[2] > 0.85f &&
            std::isfinite(surfaceDistanceExcess) && std::abs(surfaceDistanceExcess) < 0.1f &&
            std::isfinite(supportVelocity[0]) && std::isfinite(supportVelocity[1]) && std::isfinite(supportVelocity[2]) &&
            std::abs(supportVelocity[0]) + std::abs(supportVelocity[1]) + std::abs(supportVelocity[2]) < 0.01f;
    }
};

static_assert(offsetof(bhkCharacterController, stepInfo) == 0x80);
static_assert(offsetof(bhkCharacterController, stepInfo) + offsetof(hkStepInfo, deltaTime) == 0x88);
static_assert(offsetof(bhkCharacterController, stepInfo) + offsetof(hkStepInfo, invDeltaTime) == 0x8C);

static_assert(offsetof(bhkCharacterController, supportedState) == 0x1A0);
static_assert(offsetof(bhkCharacterController, supportNormal) == 0x1B0);
static_assert(offsetof(bhkCharacterController, characterState) == 0x200);
static_assert(offsetof(bhkCharacterController, characterFlags) == 0x218);
