#include <Services/SessionResyncService.h>

#include <Events/PlayerLeaveEvent.h>
#include <Game/Player.h>
#include <GameServer.h>
#include <Messages/NotifyResyncComplete.h>
#include <Messages/RequestResync.h>
#include <Services/CharacterService.h>
#include <Services/SaveSnapshotService.h>
#include <Services/StringCacheService.h>
#include <Services/WorldItemService.h>
#include <World.h>

#include <chrono>

namespace
{
uint64_t ResyncNowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

SessionResyncService::SessionResyncService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_requestConnection(aDispatcher.sink<PacketEvent<RequestResync>>().connect<&SessionResyncService::OnRequestResync>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&SessionResyncService::OnPlayerLeave>(this))
{
}

void SessionResyncService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (acEvent.pPlayer)
        m_budgetByPlayerId.erase(acEvent.pPlayer->GetId());
}

void SessionResyncService::OnRequestResync(const PacketEvent<RequestResync>& acMessage) noexcept
{
    using Session::Resync::Outcome;
    using Session::Resync::Scope;

    Player* pPlayer = acMessage.pPlayer;
    if (!pPlayer)
        return;

    const RequestResync& request = acMessage.Packet;

    NotifyResyncComplete complete{};
    complete.RequestId = request.RequestId;
    complete.RequestScope = request.RequestScope;
    complete.ServerId = request.ServerId;

    const auto finish = [&](Outcome aOutcome, uint32_t aSnapshots) {
        complete.Result = aOutcome;
        complete.SnapshotsSent = aSnapshots;
        pPlayer->Send(complete);
    };

    if (!request.RawScopeKnown || (request.RequestScope == Scope::kEntity && request.ServerId == Session::Resync::kNoServerId))
        return finish(Outcome::kInvalid, 0);

    if (!IsCoopFeatureEnabled(Session::Feature::kSelfHealingResync))
        return finish(Outcome::kDisabled, 0);

    // A world reload barrier rebuilds every observer itself (ResetWorldEntitiesForCheckpointReload +
    // ForceResyncObserverAfterRollback); answering now would send pre-reload state. The client
    // re-arms throttled requests and asks again after its cooldown. The string table and the dropped
    // items are session state the barrier does not rebuild (the rollback already restored and
    // broadcast the items): they are answered at once.
    const auto& saveCoordinator = m_world.GetSaveSnapshotService().GetCoordinator();
    const bool rebuiltByBarrier = request.RequestScope == Scope::kEntity || request.RequestScope == Scope::kAllEntities;
    if (rebuiltByBarrier && saveCoordinator.HasActiveReloadBarrier())
        return finish(Outcome::kThrottled, 0);

    const uint64_t nowMs = ResyncNowMs();
    auto& budget = m_budgetByPlayerId.try_emplace(pPlayer->GetId()).first->second;
    if (!budget.Admit(request.RequestScope, nowMs))
        return finish(Outcome::kThrottled, 0);

    ++m_requestsServed;
    switch (request.RequestScope)
    {
    case Scope::kEntity:
    {
        uint32_t snapshots = 0;
        const Outcome outcome = m_world.GetCharacterService().ResyncEntityForObserver(pPlayer, request.ServerId, snapshots);
        spdlog::info("[RFAB Resync] player {} entity {:X} ({}): {}", pPlayer->GetId(), request.ServerId,
                     Session::Resync::ToString(request.GapReason), Session::Resync::ToString(outcome));
        return finish(outcome, snapshots);
    }
    case Scope::kAllEntities:
    {
        const uint32_t snapshots = m_world.GetCharacterService().ResyncAllEntitiesForObserver(pPlayer);
        spdlog::info("[RFAB Resync] player {} whole session ({}): {} entities in range resent", pPlayer->GetId(),
                     Session::Resync::ToString(request.GapReason), snapshots);
        return finish(Outcome::kApplied, snapshots);
    }
    case Scope::kStringCache:
    {
        const uint32_t strings = m_world.ctx().at<StringCacheService>().ResendAllTo(pPlayer);
        spdlog::info("[RFAB Resync] player {} string table ({}): {} strings resent", pPlayer->GetId(),
                     Session::Resync::ToString(request.GapReason), strings);
        return finish(Outcome::kApplied, strings);
    }
    case Scope::kWorldItems:
    {
        // Answered even with Feature::kServerOwnedWorldItems off: items registered before a live
        // switch-off still exist and are still arbitrated.
        const uint32_t items = m_world.ctx().at<WorldItemService>().SendSnapshotTo(pPlayer);
        spdlog::info("[RFAB Resync] player {} dropped items ({}): snapshot with {} items", pPlayer->GetId(),
                     Session::Resync::ToString(request.GapReason), items);
        return finish(Outcome::kApplied, items);
    }
    }

    finish(Outcome::kInvalid, 0);
}
