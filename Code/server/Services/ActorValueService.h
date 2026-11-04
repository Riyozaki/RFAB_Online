#pragma once

#include <Events/PacketEvent.h>
#include <Authority/Audit.h>
#include <Authority/Hits.h>
struct World;
struct UpdateEvent;
struct PlayerLeaveEvent;
struct TransportService;
struct RequestActorValueChanges;
struct RequestActorMaxValueChanges;
struct RequestHealthChangeBroadcast;
struct RequestDeathStateChange;

/**
 * @brief Broadcasts changes in (max) actor values and updates them server side.
 */
struct ActorValueService
{
    ActorValueService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~ActorValueService() noexcept = default;

    TP_NOCOPYMOVE(ActorValueService);

    void RecordCombatClaim(
        uint32_t aAttackerPlayerId,
        uint32_t aTargetServerId,
        Authority::HitKind aKind) const noexcept;
    void RecordPlayerDownedState(uint32_t aPlayerId, bool aIsDowned) const noexcept;

    //! @brief Print everything the server has refused since it started.
    //!
    //! Exposed because the answer to "combat felt wrong" has to be something a server
    //! operator can actually fetch, not a debug build.
    void LogCombatAudit() const noexcept;

private:
    //! Say out loud what was just refused, at most as often as the policy allows.
    void ReportAudit(uint64_t aNowMs) const noexcept;

    World& m_world;
    mutable Authority::HitLedger<64> m_combatAuthorityLedger{};
    mutable Authority::CombatAudit m_combatAudit{};

    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) const noexcept;
    void OnActorValueChanges(const PacketEvent<RequestActorValueChanges>& acMessage) const noexcept;
    void OnActorMaxValueChanges(const PacketEvent<RequestActorMaxValueChanges>& acMessage) const noexcept;
    void OnHealthChangeBroadcast(const PacketEvent<RequestHealthChangeBroadcast>& acMessage) const noexcept;
    void OnDeathStateChange(const PacketEvent<RequestDeathStateChange>& acMessage) const noexcept;

    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_updateHealthConnection;
    entt::scoped_connection m_updateMaxValueConnection;
    entt::scoped_connection m_updateDeltaHealthConnection;
    entt::scoped_connection m_deathStateConnection;
};
