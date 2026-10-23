#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

struct hkStepInfo
{
    bool UpdateDeltaTime(float aPhysicsDeltaTime, float aMovementDeltaTime = 0.f) noexcept
    {
        return TrySetDeltaTime(aMovementDeltaTime) ||
               TrySetDeltaTime(aPhysicsDeltaTime) ||
               TrySetDeltaTime(deltaTime) ||
               TrySetDeltaTime(1.f / 60.f);
    }

    float startTime;
    float endTime;
    float deltaTime;
    float invDeltaTime;

private:
    bool TrySetDeltaTime(float aDeltaTime) noexcept
    {
        if (!std::isfinite(aDeltaTime) || aDeltaTime <= 0.0001f)
            return false;

        const float clamped = std::clamp(aDeltaTime, 1.f / 240.f, 0.1f);
        startTime = (std::isfinite(endTime) && endTime >= 0.f && endTime < 86400.f) ? endTime : 0.f;
        endTime = startTime + clamped;
        deltaTime = clamped;
        invDeltaTime = 1.f / clamped;
        return true;
    }
};

static_assert(offsetof(hkStepInfo, deltaTime) == 0x8);
static_assert(offsetof(hkStepInfo, invDeltaTime) == 0xC);
static_assert(sizeof(hkStepInfo) == 0x10);
