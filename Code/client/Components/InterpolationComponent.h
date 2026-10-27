#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <PlaybackClock.h>
#include <Structs/AnimationVariables.h>

struct TESObjectCELL;

struct InterpolationComponent
{
    struct TimePoint
    {
        uint64_t Tick{};
        glm::vec3 Position{};
        glm::vec3 Rotation{};
        AnimationVariables Variables{};
        float Direction{};

        TimePoint() = default;
        TimePoint(const TimePoint&) = default;
        TimePoint& operator=(const TimePoint&) = default;
    };

    List<TimePoint> TimePoints;
    glm::vec3 Position{};
    PlaybackClock Clock{};
    float LastHorizontalSpeed{0.0f};
    TESObjectCELL* TrackedParentCell{nullptr};
    int32_t TrackedParentGridX{0};
    int32_t TrackedParentGridY{0};
    bool HasTrackedParentGrid{false};
    uint64_t LastSeamlessRehomeTick{0};
};
