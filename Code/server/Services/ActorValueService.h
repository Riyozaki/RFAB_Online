#pragma once

#include <Events/PacketEvent.h>
#include <CoopDamagePipeline.h>

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
        CoopDamagePipeline::ServerHitClaimKind aKind) const noexcept;
    void RecordPlayerDownedState(uint32_t aPlayerId, bool aIsDowned) const noexcept;

private:
    World& m_world;
    mutable CoopDamagePipeline::ServerCombatAuthorityLedger<64> m_combatAuthorityLedger{};

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
