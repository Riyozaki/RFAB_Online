#include <algorithm>

#include <Events/PacketEvent.h>
#include <GameServer.h>
#include <Services/ActorValueService.h>
#include <World.h>
#include <Components.h>

#include <Messages/RequestActorValueChanges.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/NotifyDeathStateChange.h>

#include <algorithm>

namespace
{
constexpr uint32_t kHealthActorValueIndex = 24;
}

ActorValueService::ActorValueService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateHealthConnection = aDispatcher.sink<PacketEvent<RequestActorValueChanges>>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_updateMaxValueConnection = aDispatcher.sink<PacketEvent<RequestActorMaxValueChanges>>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_updateDeltaHealthConnection = aDispatcher.sink<PacketEvent<RequestHealthChangeBroadcast>>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_deathStateConnection = aDispatcher.sink<PacketEvent<RequestDeathStateChange>>().connect<&ActorValueService::OnDeathStateChange>(this);
}

void ActorValueService::OnActorValueChanges(const PacketEvent<RequestActorValueChanges>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();
    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));
    if (it == actorValuesView.end())
        return;

    // Reject stale periodic snapshots from an ex-owner right after ownership transfer.
    const auto& ownerComponent = actorValuesView.get<OwnerComponent>(*it);
    if (ownerComponent.GetOwner() != acMessage.pPlayer)
        return;

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    for (auto& [id, value] : message.Values)
    {
        actorValuesComponent.CurrentActorValues.ActorValuesList[id] = value;
    }

    NotifyActorValueChanges notify;
    notify.Id = acMessage.Packet.Id;
    notify.Values = acMessage.Packet.Values;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnActorMaxValueChanges(const PacketEvent<RequestActorMaxValueChanges>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();
    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));
    if (it == actorValuesView.end())
        return;

    // Reject stale max-value snapshots from an ex-owner right after ownership transfer.
    const auto& ownerComponent = actorValuesView.get<OwnerComponent>(*it);
    if (ownerComponent.GetOwner() != acMessage.pPlayer)
        return;

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    for (auto& [id, value] : message.Values)
    {
        actorValuesComponent.CurrentActorValues.ActorMaxValuesList[id] = value;
    }

    NotifyActorMaxValueChanges notify;
    notify.Id = message.Id;
    notify.Values = message.Values;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnHealthChangeBroadcast(const PacketEvent<RequestHealthChangeBroadcast>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();
    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));

    if (it != actorValuesView.end())
    {
        auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
        const float currentHealth = actorValuesComponent.CurrentActorValues.ActorValuesList[kHealthActorValueIndex];
        float updatedHealth = std::max(0.f, currentHealth + message.DeltaHealth);

        const auto maxIt = actorValuesComponent.CurrentActorValues.ActorMaxValuesList.find(kHealthActorValueIndex);
        if (maxIt != actorValuesComponent.CurrentActorValues.ActorMaxValuesList.end() && maxIt->second > 0.f)
            updatedHealth = std::min(updatedHealth, maxIt->second);

        actorValuesComponent.CurrentActorValues.ActorValuesList[kHealthActorValueIndex] = updatedHealth;
    }

    NotifyHealthChangeBroadcast notify;
    notify.Id = message.Id;
    notify.DeltaHealth = message.DeltaHealth;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnDeathStateChange(const PacketEvent<RequestDeathStateChange>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();
    const auto it = characterView.find(static_cast<entt::entity>(message.Id));

    if (it != characterView.end())
    {
        auto& characterComponent = characterView.get<CharacterComponent>(*it);
        characterComponent.SetDead(message.IsDead);

        if (message.IsDead)
        {
            if (auto* pActorValues = m_world.try_get<ActorValuesComponent>(*it))
                pActorValues->CurrentActorValues.ActorValuesList[kHealthActorValueIndex] = 0.f;
        }

        spdlog::debug("Updating death state {:x}:{}", message.Id, message.IsDead);
    }

    NotifyDeathStateChange notify;
    notify.Id = message.Id;
    notify.IsDead = message.IsDead;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}
