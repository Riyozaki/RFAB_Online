#include <TiltedOnlinePCH.h>

#include <Services/SessionResyncService.h>

#include <Events/UpdateEvent.h>
#include <Forms/TESObjectCELL.h>
#include <Messages/NotifyResyncComplete.h>
#include <Messages/RequestResync.h>
#include <PlayerCharacter.h>
#include <Services/CharacterService.h>
#include <Services/TransportService.h>
#include <World.h>

#include <chrono>

namespace
{
constexpr double kPollIntervalSec = 0.25;
}

SessionResyncService::SessionResyncService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&SessionResyncService::OnUpdate>(this))
    , m_resyncCompleteConnection(aDispatcher.sink<NotifyResyncComplete>().connect<&SessionResyncService::OnResyncComplete>(this))
{
}

uint64_t SessionResyncService::NowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <class TWork> void SessionResyncService::OnMainThreadForThisSession(TWork&& aWork) noexcept
{
    if (World::IsMainThread())
    {
        aWork();
        return;
    }

    const Session::Token token = m_world.GetLifecycle().GetTokenAnyThread();
    m_world.GetRunner().Queue([this, token, work = std::forward<TWork>(aWork)]() mutable noexcept {
        if (m_world.GetLifecycle().IsStillCurrentSession(token))
            work();
    });
}

void SessionResyncService::NoteGap(Scope aScope, uint32_t aServerId, Reason aReason) noexcept
{
    if (!World::IsMainThread())
    {
        OnMainThreadForThisSession([this, aScope, aServerId, aReason]() noexcept { NoteGap(aScope, aServerId, aReason); });
        return;
    }

    if (!m_transport.IsOnline() || !World::IsFeatureEnabled(Session::Feature::kSelfHealingResync))
        return;

    if (m_ledger.NoteGap(aScope, aServerId, aReason, NowMs()))
    {
        spdlog::info("[RFAB Resync] gap detected: {} {:X} ({})", Session::Resync::ToString(aScope), aServerId,
                     Session::Resync::ToString(aReason));
    }
}

void SessionResyncService::NoteEntityKnown(uint32_t aServerId) noexcept
{
    OnMainThreadForThisSession([this, aServerId]() noexcept { m_ledger.NoteEntityKnown(aServerId); });
}

void SessionResyncService::NoteEntityRemoved(uint32_t aServerId) noexcept
{
    OnMainThreadForThisSession([this, aServerId]() noexcept { m_ledger.NoteEntityRemoved(aServerId, NowMs()); });
}

void SessionResyncService::NoteEntityIgnored(uint32_t aServerId) noexcept
{
    OnMainThreadForThisSession([this, aServerId]() noexcept { m_ledger.NoteEntityIgnored(aServerId, NowMs()); });
}

void SessionResyncService::NoteWorldReloadStarted() noexcept
{
    if (!World::IsMainThread())
    {
        OnMainThreadForThisSession([this]() noexcept { NoteWorldReloadStarted(); });
        return;
    }

    m_ledger.AbandonInFlight(NowMs());
    NoteGap(Scope::kAllEntities, Session::Resync::kNoServerId, Reason::kWorldPacketsDiscarded);
}

// IsOnline (authenticated session), not IsConnected (socket up): the server has no Player to answer
// before the AuthenticationResponse was accepted.
bool SessionResyncService::CanSendNow() const noexcept
{
    if (!m_transport.IsOnline() || !World::IsFeatureEnabled(Session::Feature::kSelfHealingResync))
        return false;

    const auto& characters = m_world.GetCharacterService();
    if (characters.IsEngineWorldLoading() || characters.IsSavePurgeActive())
        return false;

    // The answer is applied to the live world: wait until the local player stands in an attached,
    // loaded cell (the same predicate cell-detach isolation uses since v3.1.4).
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer && !pPlayer->IsStillLoading() && pPlayer->parentCell && pPlayer->parentCell->IsAttachedAndLoaded();
}

void SessionResyncService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    m_pollAccumulatorSec += acEvent.Delta;
    if (m_pollAccumulatorSec < kPollIntervalSec)
        return;
    m_pollAccumulatorSec = 0.0;

    m_outgoing.clear();
    if (m_ledger.Poll(NowMs(), CanSendNow(), m_outgoing) == 0)
        return;

    for (const auto& request : m_outgoing)
    {
        RequestResync message{};
        message.RequestId = request.RequestId;
        message.RequestScope = request.RequestScope;
        message.ServerId = request.ServerId;
        message.GapReason = request.GapReason;
        m_transport.Send(message);

        spdlog::info("[RFAB Resync] request #{}: {} {:X} ({})", request.RequestId,
                     Session::Resync::ToString(request.RequestScope), request.ServerId,
                     Session::Resync::ToString(request.GapReason));
    }
}

void SessionResyncService::OnResyncComplete(const NotifyResyncComplete& acMessage) noexcept
{
    const bool matched = m_ledger.OnComplete(acMessage.RequestId, acMessage.Result, NowMs());
    if (!matched)
    {
        spdlog::debug("[RFAB Resync] late completion #{} ignored", acMessage.RequestId);
        return;
    }

    if (acMessage.Result == Session::Resync::Outcome::kApplied || acMessage.Result == Session::Resync::Outcome::kEntityGone)
    {
        spdlog::info("[RFAB Resync] request #{} {} ({} snapshots)", acMessage.RequestId,
                     Session::Resync::ToString(acMessage.Result), acMessage.SnapshotsSent);
    }
    else
    {
        spdlog::warn("[RFAB Resync] request #{} for {} {:X} answered '{}'", acMessage.RequestId,
                     Session::Resync::ToString(acMessage.RequestScope), acMessage.ServerId,
                     Session::Resync::ToString(acMessage.Result));
    }
}

void SessionResyncService::OnSessionBegin(const Session::Transition&) noexcept
{
    m_pollAccumulatorSec = 0.0;
}

void SessionResyncService::OnSessionEnd(const Session::Transition&) noexcept
{
    LogStats("session end");
    m_ledger.Reset();
    m_outgoing.clear();
}

void SessionResyncService::OnWorldLoadBegin(const Session::Transition& acTransition) noexcept
{
    // World packets queued during the pause are discarded with the reload and spawns arriving during
    // the load are dropped: ask for the whole session once the load is over (sent only when the player
    // stands in a loaded cell again). Offline loads have no session to resync.
    if (acTransition.Online)
        NoteWorldReloadStarted();
}

void SessionResyncService::LogStats(const char* acpWhen) const noexcept
{
    const auto& stats = m_ledger.GetStats();
    if (stats.RequestsSent == 0 && stats.GapsAbsorbed == 0)
        return;

    spdlog::info("[RFAB Resync] {}: requests={} completed={} timedOut={} throttled={} escalations={} quarantined={} "
                 "ownershipConflicts={} absorbed={} | gaps: worldLoad={} unknownEntity={} stringCache={} reload={} digest={}",
                 acpWhen, stats.RequestsSent, stats.Completed, stats.TimedOut, stats.Throttled, stats.Escalations,
                 stats.Quarantined, stats.OwnershipConflicts, stats.GapsAbsorbed,
                 stats.GapsAccepted[static_cast<uint8_t>(Reason::kSpawnDroppedDuringWorldLoad)],
                 stats.GapsAccepted[static_cast<uint8_t>(Reason::kUnknownEntityInMovement)],
                 stats.GapsAccepted[static_cast<uint8_t>(Reason::kStringCacheRejected)],
                 stats.GapsAccepted[static_cast<uint8_t>(Reason::kWorldPacketsDiscarded)],
                 stats.GapsAccepted[static_cast<uint8_t>(Reason::kDigestMismatch)]);
}
