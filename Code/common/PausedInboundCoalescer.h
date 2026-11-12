#pragma once

// RFAB Online: lossless handling of movement/animation packets received while the local
// simulation is paused (StatsMenu perk tree, MapMenu, Journal, Console ...).
//
// The server sends `ServerReferencesMoveRequest` UNRELIABLY when it only carries positions, but
// RELIABLY as soon as one of the updates carries animation `ActionEvents` (draw weapon, attack,
// sit, sneak ...). Up to v4.0.0 every move packet was dropped while paused, so every reliable
// animation action that arrived during a menu was lost for good and remote actors stayed in the
// wrong graph state (weapon sheathed/drawn, stuck in furniture, ...) until their next action.
//
// Instead, all move packets received while paused are folded into ONE pending request:
// - per entity the newest movement wins (it is a snapshot, older ones are worthless);
// - per entity every ActionEvent is kept in arrival order (they are causal), bounded to the
//   newest kMaxCoalescedActionsPerEntity so a multi-minute pause stays O(entities) in memory.
//   AnimationSystem already trims stale backlog (>16 actions, >2 s old) and latches furniture
//   exits, so replaying the bounded tail on unpause converges on the sender's final state.
// The pending request is dispatched once, AFTER the queued reliable packets, so spawns/removals
// that happened during the pause are applied before the movement that refers to them.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

// Self-contained: the encoding headers below normally rely on EncodingPch.h, which consumers such
// as the Windows TPTests target do not force-include.
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <Messages/ServerReferencesMoveRequest.h>

namespace PausedInbound
{
inline constexpr size_t kMaxCoalescedActionsPerEntity = 64;

inline void MergeMoveRequest(ServerReferencesMoveRequest& aPending, bool& aHasPending, const ServerReferencesMoveRequest& acIncoming) noexcept
{
    if (!aHasPending)
    {
        aPending.Updates.clear();
        aPending.Tick = acIncoming.Tick;
        aHasPending = true;
    }

    // Unreliable position-only packets may be reordered against each other; never let an older
    // snapshot overwrite a newer one. Reliable packets (the ones with actions) arrive in order.
    const bool cIncomingIsNewer = acIncoming.Tick >= aPending.Tick;

    for (const auto& [serverId, update] : acIncoming.Updates)
    {
        const bool cExisted = aPending.Updates.find(serverId) != aPending.Updates.end();
        auto& merged = aPending.Updates[serverId];

        if (!cExisted)
        {
            merged = update;
        }
        else
        {
            if (cIncomingIsNewer)
                merged.UpdatedMovement = update.UpdatedMovement;

            merged.ActionEvents.insert(merged.ActionEvents.end(), update.ActionEvents.begin(), update.ActionEvents.end());
        }

        if (merged.ActionEvents.size() > kMaxCoalescedActionsPerEntity)
        {
            const auto cExcess = merged.ActionEvents.size() - kMaxCoalescedActionsPerEntity;
            merged.ActionEvents.erase(merged.ActionEvents.begin(), merged.ActionEvents.begin() + static_cast<std::ptrdiff_t>(cExcess));
        }
    }

    if (cIncomingIsNewer)
        aPending.Tick = acIncoming.Tick;
}
} // namespace PausedInbound
