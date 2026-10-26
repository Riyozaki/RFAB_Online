#pragma once

#include <Games/Primitives.h>
#include <Havok/hkStepInfo.h>

struct bhkCharacterController : NiRefObject
{
    bool UpdateStepTiming(float aMovementDeltaTime = 0.f) noexcept;
    void ResetTeleportPhysicsState(float aNewZ) noexcept;

    uint8_t pad10[0x80 - 0x10];
    hkStepInfo stepInfo;       // 0x80
    float outVelocity[4];      // 0x90
    float initialVelocity[4];  // 0xA0
    float velocityMod[4];      // 0xB0
    uint8_t padC0[0x240 - 0xC0];
    float fallStartHeight;     // 0x240
    float fallTime;            // 0x244
};

static_assert(offsetof(bhkCharacterController, stepInfo) == 0x80);
static_assert(offsetof(bhkCharacterController, stepInfo) + offsetof(hkStepInfo, deltaTime) == 0x88);
static_assert(offsetof(bhkCharacterController, stepInfo) + offsetof(hkStepInfo, invDeltaTime) == 0x8C);
static_assert(offsetof(bhkCharacterController, outVelocity) == 0x90);
static_assert(offsetof(bhkCharacterController, initialVelocity) == 0xA0);
static_assert(offsetof(bhkCharacterController, velocityMod) == 0xB0);
static_assert(offsetof(bhkCharacterController, fallStartHeight) == 0x240);
static_assert(offsetof(bhkCharacterController, fallTime) == 0x244);
