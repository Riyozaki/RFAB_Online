#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Sync/HitImpact.h>
struct World;

struct ConnectedEvent;
struct DisconnectedEvent;
struct ActorRemovedEvent;
struct UpdateEvent;
struct HealthChangeEvent;

struct TransportService;
struct NotifyActorValueChanges;
struct NotifyActorMaxValueChanges;
struct NotifyHealthChangeBroadcast;
struct NotifyDeathStateChange;

struct Actor;
struct ActorValuesComponent;

/**
 * @brief Broadcast and apply actor values and death state.
 */
struct ActorValueService
{
public:
    ActorValueService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~ActorValueService() noexcept = default;

    TP_NOCOPYMOVE(ActorValueService);

    void ResetForWorldReload() noexcept;
    void NoteAuthoritativeHealthRevision(uint32_t aServerId, uint32_t aRevision) const noexcept;
    void ForgetHealthRevision(uint32_t aServerId) const noexcept;

private:
    enum ValueType : uint8_t
    {
        kValue,
        kMaxValue
    };

    void OnLocalComponentAdded(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnLocalComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnActorRemoved(const ActorRemovedEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnActorValueChanges(const NotifyActorValueChanges& acMessage) const noexcept;
    void OnActorMaxValueChanges(const NotifyActorMaxValueChanges& acMessage) const noexcept;
    void OnHealthChange(const HealthChangeEvent&) noexcept;
    void OnHealthChangeBroadcast(const NotifyHealthChangeBroadcast& acMessage) const noexcept;
    void OnDeathStateChange(const NotifyDeathStateChange& acEvent) const noexcept;

    void RunActorValuesUpdates() noexcept;
    void RunSmallHealthUpdates() noexcept;
    void RunDeathStateUpdates() noexcept;
    void BroadcastCombatHealthSnapshots() noexcept;

    void BroadcastActorValues() noexcept;
    void CreateActorValuesComponent(entt::entity aEntity, Actor* apActor) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    //! @brief Server ids and collected health changes.
    Map<uint32_t, float> m_smallHealthChanges;
    Map<uint32_t, uint32_t> m_smallHealthAttackerIds;
    mutable Map<uint32_t, uint64_t> m_lastLocalPredictedHitMs;

    //! @brief The newest health revision this client has applied, per server id. Sent back
    //!        with absolute value reports so the server can tell a fresh reading from one
    //!        that was taken before somebody else's hit landed.
    mutable Map<uint32_t, uint32_t> m_knownHealthRevisions;
    mutable Sync::HitImpactGate m_remoteHitImpactGate{};
};
