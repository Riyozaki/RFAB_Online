#pragma once

#include <common/Session/Resync.h>

#include <vector>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct NotifyResyncComplete;

/**
 * Client half of the self-healing contract (architecture step 1, common/Session/Resync.h).
 *
 * Services that detect lost state report it here instead of trying to repair it themselves:
 *   - CharacterService: spawn dropped while the engine loads, movement for an unknown server id,
 *     spawns it deliberately refused (missing plugin), removals (tombstones);
 *   - StringCacheService: rejected string-table delta;
 *   - CoordinatedSaveService: in-session world reload discarded the paused world packets.
 * This service owns the decision of WHEN to ask the server (deduplicated, rate limited, deferred
 * while loading) and the bookkeeping of the answers. Main thread only; calls from other threads
 * are marshalled through the RunnerService.
 */
struct SessionResyncService
{
    using Scope = Session::Resync::Scope;
    using Reason = Session::Resync::Reason;

    SessionResyncService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~SessionResyncService() noexcept = default;

    TP_NOCOPYMOVE(SessionResyncService);

    void NoteGap(Scope aScope, uint32_t aServerId, Reason aReason) noexcept;
    void NoteEntityKnown(uint32_t aServerId) noexcept;
    void NoteEntityRemoved(uint32_t aServerId) noexcept;
    void NoteEntityIgnored(uint32_t aServerId) noexcept;
    // In-session world reload: replies in flight were discarded with the paused queue and every
    // world-scoped packet received during the load is gone - re-arm and ask for the whole session.
    void NoteWorldReloadStarted() noexcept;

    [[nodiscard]] const Session::Resync::ClientStats& GetStats() const noexcept { return m_ledger.GetStats(); }

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnResyncComplete(const NotifyResyncComplete& acMessage) noexcept;

    [[nodiscard]] bool CanSendNow() const noexcept;
    [[nodiscard]] static uint64_t NowMs() noexcept;
    void LogStats(const char* acpWhen) const noexcept;

    World& m_world;
    TransportService& m_transport;
    Session::Resync::ClientLedger m_ledger{};
    std::vector<Session::Resync::Request> m_outgoing{};
    double m_pollAccumulatorSec{0.0};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_resyncCompleteConnection;
};
