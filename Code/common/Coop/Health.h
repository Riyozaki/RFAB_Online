#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace Coop
{
// Health has ONE network write path: sequenced signed mutations -> canonical absolute replies.
// Periodic AV snapshots must not write health. A snapshot captured before a remote hit would
// otherwise undo it, even if both packets carry the same ownership epoch.
inline std::optional<float> AddHealth(float aHealth, float aDelta) noexcept
{
    const double result = static_cast<double>(aHealth) + aDelta;
    if (!std::isfinite(aHealth) || !std::isfinite(aDelta) || !std::isfinite(result) ||
        result > std::numeric_limits<float>::max())
        return std::nullopt;
    // Do not cap at permanent AV: temporary RFAB buffs can legitimately raise effective max HP.
    return static_cast<float>(std::max(0.0, result));
}

class HealthPrediction
{
public:
    struct Mutation { uint64_t Sequence; float Delta; };
    static constexpr size_t kMaxPending = 256;

    void Reset(uint32_t aEpoch, uint64_t aRevision, float aHealth) noexcept
    {
        Epoch = aEpoch;
        Revision = aRevision;
        Canonical = Observed = std::isfinite(aHealth) ? std::max(0.f, aHealth) : 0.f;
        NextSequence = 1;
        Pending.clear();
    }

    // Control snapshots can overtake a health reply. Same-epoch adoption must preserve both
    // unsent local observations and the monotonic outgoing sequence, and retire only operations
    // explicitly included in the snapshot. Older epochs/revisions never rewind native HP.
    std::optional<float> Adopt(uint32_t aEpoch, uint64_t aRevision, float aHealth, uint64_t aOwnAcknowledgement = 0) noexcept
    {
        if (!aEpoch || !std::isfinite(aHealth) || aEpoch < Epoch)
            return std::nullopt;
        if (aEpoch != Epoch)
            Reset(aEpoch, aRevision, aHealth);
        else
            Accept(aRevision, aHealth, aOwnAcknowledgement);
        return Observed;
    }

    std::optional<Mutation> Queue(float aDelta) noexcept
    {
        if (Epoch == 0 || !std::isfinite(aDelta) || aDelta == 0.f || Pending.size() >= kMaxPending || NextSequence == 0)
            return std::nullopt;
        Mutation mutation{NextSequence++, aDelta};
        Pending.push_back(mutation);
        return mutation;
    }

    // The owner samples actual HP (including script/DOT changes), not an estimated damage formula.
    // Call BEFORE applying incoming health, so unsampled local damage cannot disappear in a reply.
    std::optional<Mutation> Observe(float aEngineHealth) noexcept
    {
        if (!std::isfinite(aEngineHealth))
            return std::nullopt;
        aEngineHealth = std::max(0.f, aEngineHealth);
        const auto mutation = Queue(aEngineHealth - Observed);
        if (mutation || aEngineHealth == Observed)
            Observed = aEngineHealth;
        return mutation;
    }

    float Accept(uint64_t aRevision, float aHealth, uint64_t aOwnAcknowledgement) noexcept
    {
        if (std::isfinite(aHealth) && aRevision >= Revision)
        {
            Revision = aRevision;
            Canonical = std::max(0.f, aHealth);
        }
        Pending.erase(std::remove_if(Pending.begin(), Pending.end(),
            [aOwnAcknowledgement](const auto& entry) { return entry.Sequence <= aOwnAcknowledgement; }), Pending.end());
        Observed = Project();
        return Observed;
    }

    float Project() const noexcept
    {
        float value = Canonical;
        for (const auto& mutation : Pending)
        {
            const auto next = AddHealth(value, mutation.Delta);
            if (next)
                value = *next;
        }
        return value;
    }

    uint32_t Epoch{};
    uint64_t Revision{};
    uint64_t NextSequence{1};
    float Canonical{};
    float Observed{};
    std::vector<Mutation> Pending;
};
} // namespace Coop
