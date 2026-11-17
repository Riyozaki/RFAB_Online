#pragma once
#include <Structs/HealthScale.h>
#include <Structs/ActorValues.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace Coop
{
struct EnemyHealthPlan
{
    HealthScale Scale;
    float Health{};
};
// The server linearizes scaling with signed HP mutations. Preserve the canonical health
// fraction at that boundary, NOT a fresh full heal. In-flight damage remains an absolute
// signed mutation after that boundary; handoff/reconnect must never call this on a stamp.
inline std::optional<EnemyHealthPlan> PlanEnemyHealth(uint64_t life, float maximum, float health) noexcept
{
    const double scaledMaximum = static_cast<double>(maximum) * 2.0;
    const double scaled = static_cast<double>(health) * 2.0;
    if (!life || !std::isfinite(maximum) || maximum <= 0.f || scaledMaximum > std::numeric_limits<float>::max() ||
        !std::isfinite(health) || health <= 0.f || !std::isfinite(scaled) || scaled > std::numeric_limits<float>::max()) return {};
    HealthScale scale{life, maximum, static_cast<float>(scaledMaximum)};
    if (!scale.IsValid()) return {};
    return EnemyHealthPlan{scale, static_cast<float>(scaled)};
}
// Health-only replies may overtake the frame in which a full scale snapshot is applied.
// Preserve included origin ACKs AND non-health AVs while keeping the latest canonical HP.
inline bool MergeScaleSnapshot(ActorValues& pending, const ActorValues& next)
{
    if (!pending.IsValid() || !next.IsValid() || !pending.Scale.Life || pending.Scale != next.Scale) return false;
    auto merged = pending;
    if (next.HealthRevision >= merged.HealthRevision)
    {
        merged.HealthRevision = next.HealthRevision;
        for (const auto& [id, value] : next.ActorValuesList) merged.ActorValuesList[id] = value;
        for (const auto& [id, value] : next.ActorMaxValuesList) merged.ActorMaxValuesList[id] = value;
    }
    for (const auto& [origin, sequence] : next.HealthSequences)
        merged.HealthSequences[origin] = std::max(merged.HealthSequences[origin], sequence);
    if (!merged.IsValid()) return false;
    pending = std::move(merged);
    return true;
}
struct EnemyEligibility
{
    bool Player{}, Allied{}, Summoned{}, Mount{}, Dead{}, Scripted{}, Hostile{};
    bool Allowed() const noexcept { return Hostile && !Player && !Allied && !Summoned && !Mount && !Dead && !Scripted; }
};
}
