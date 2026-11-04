#pragma once

/**
 * How a remote actor should be moved across an exterior cell border.
 *
 * Picking between a smooth position update, a seamless cell rehome and a full teleport is
 * a world-streaming decision, not a combat one. Getting it wrong produces the 3D flicker
 * and the MoveTo storms at 4096-unit boundaries.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace WorldMotion
{

// ============================================================================
// 7. Seamless Exterior Cell-Border Motion Policy (v3.0 Phase 4B)
// ============================================================================
// Prevents visible 3D flicker, Havok `ResetTeleportPhysics` velocity resets, and 60-144 Hz `MoveTo`
// storms when remote players, summons, or NPCs walk/fight across `4096`-unit exterior cell borders:
//   - `kSmoothForcePosition`: smooth `Actor::ForcePosition` (`SetPosition`) with zero `InternalMoveTo`
//     whenever `parentCell` is attached inside the inner `3x3` loaded core (`Chebyshev <= 1`) around
//     the local player's `centerGrid`.
//   - `kSeamlessCellRehome`: lightweight `MoveToCellSeamless` (`InternalMoveTo` WITHOUT `ResetTeleportPhysics`)
//     only when `parentCell` reaches the outer `5x5` ring (`Chebyshev >= 2`) before ring-3 unload,
//     guarded by a `128`-unit border hysteresis band and `1000 ms` cooldown so border combat never ping-pongs.
//   - `kTeleportMoveTo`: full `MoveTo` (with `ResetTeleportPhysics`) only on true teleports (`> 2048` units)
//     or when `parentCell` is detached / invalid / in another worldspace.
enum class ExteriorCellMotionMode : uint8_t
{
    kSmoothForcePosition = 0,
    kSeamlessCellRehome = 1,
    kTeleportMoveTo = 2,
};

[[nodiscard]] inline float DistanceFromCellBorder4096(float aWorldCoord) noexcept
{
    if (!std::isfinite(aWorldCoord))
        return 0.0f;
    constexpr float kCellSize = 4096.0f;
    float local = std::fmod(aWorldCoord, kCellSize);
    if (local < 0.0f)
        local += kCellSize;
    return std::min(local, kCellSize - local);
}

[[nodiscard]] inline ExteriorCellMotionMode EvaluateExteriorCellMotion(
    bool aStepExceedsTeleportSnap,
    bool aHasValidAttachedParentCell,
    bool aHasLoadedDestCell,
    bool aParentEqualsDestCell,
    int32_t aParentGridX,
    int32_t aParentGridY,
    int32_t aCenterGridX,
    int32_t aCenterGridY,
    float aPosX,
    float aPosY,
    uint64_t aNowTick,
    uint64_t aLastSeamlessRehomeTick,
    float aBorderHysteresisUnits = 128.0f,
    uint64_t aRehomeCooldownMs = 1000ULL) noexcept
{
    if (!aHasLoadedDestCell)
        return ExteriorCellMotionMode::kSmoothForcePosition;

    if (aStepExceedsTeleportSnap || !aHasValidAttachedParentCell)
        return ExteriorCellMotionMode::kTeleportMoveTo;

    if (aParentEqualsDestCell)
        return ExteriorCellMotionMode::kSmoothForcePosition;

    const int32_t dx = (aParentGridX >= aCenterGridX) ? (aParentGridX - aCenterGridX) : (aCenterGridX - aParentGridX);
    const int32_t dy = (aParentGridY >= aCenterGridY) ? (aParentGridY - aCenterGridY) : (aCenterGridY - aParentGridY);
    const int32_t parentRingDist = std::max(dx, dy);

    // While `parentCell` sits in the inner 3x3 core (`ring <= 1`), it is at least 2 full grid shifts
    // away from unloading (`ring >= 3`), so smooth `ForcePosition` needs zero `InternalMoveTo` calls.
    if (parentRingDist <= 1)
        return ExteriorCellMotionMode::kSmoothForcePosition;

    const float borderDist = std::min(DistanceFromCellBorder4096(aPosX), DistanceFromCellBorder4096(aPosY));
    if (borderDist < aBorderHysteresisUnits)
        return ExteriorCellMotionMode::kSmoothForcePosition;

    if (aLastSeamlessRehomeTick != 0 && aNowTick < aLastSeamlessRehomeTick + aRehomeCooldownMs)
        return ExteriorCellMotionMode::kSmoothForcePosition;

    return ExteriorCellMotionMode::kSeamlessCellRehome;
}

} // namespace WorldMotion
