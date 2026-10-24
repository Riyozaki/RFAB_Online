#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>

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

    /**
     * @brief Records the native unscaled 1x engine permanent health before remote SetActorValues mutates it,
     *        or divides a dynamic 0xFFxxxxxx actor's scaled remote health cleanly without 1/3 or 9x drift.
     */
    void CaptureNativeBaseMaxHealth(Actor* apActor) noexcept;
    void SeedUnscaledMaxHealthFromRemote(uint32_t aFormId, float aScaledMaxHealth) noexcept;

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

    void BroadcastActorValues() noexcept;
    void CreateActorValuesComponent(entt::entity aEntity, Actor* apActor) noexcept;
    void ApplyHealthScaling() noexcept;
    void ApplyHealthMultiplier(Actor* apActor, ActorValuesComponent& aComponent, float aMultiplier) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    //! @brief Server ids and collected health changes.
    Map<uint32_t, float> m_smallHealthChanges;
    //! @brief Exact unscaled 1.0x base max health per non-player actor formId (prevents 3^K drift across ownership transfers).
    Map<uint32_t, float> m_unscaledMaxHealth;
};
