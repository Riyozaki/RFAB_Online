#include <Components.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestDeathStateChange.h>
#include <Services/ActorValueService.h>
#include <Services/CoopService.h>
#include <World.h>
#include <GameServer.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>
#include <cmath>
#include <Coop/Health.h>

namespace
{
// Skyrim ActorValue kInfamy (61) is repurposed by Requiem/RFAB scripts for local state;
// avoid syncing NaN/Inf values or mutating shared actor values with non-finite floats.
constexpr uint32_t kInfamyActorValueId = 61;
} // namespace

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

    if (it == actorValuesView.end() || !actorValuesView.get<OwnerComponent>(*it).IsCurrentOwner(acMessage.pPlayer, message.OwnershipEpoch))
        return;

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    NotifyActorValueChanges notify;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Id = acMessage.Packet.Id;
    for (const auto& [id, value] : message.Values)
    {
        if (id >= 164 || id == 24 || id == kInfamyActorValueId || !std::isfinite(value))
            continue;
        actorValuesComponent.CurrentActorValues.ActorValuesList[id] = value;
        notify.Values[id] = value;
    }

    if (notify.Values.empty())
        return;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnActorMaxValueChanges(const PacketEvent<RequestActorMaxValueChanges>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();

    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));

    if (it == actorValuesView.end() || !actorValuesView.get<OwnerComponent>(*it).IsCurrentOwner(acMessage.pPlayer, message.OwnershipEpoch))
        return;

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    NotifyActorMaxValueChanges notify;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Id = message.Id;
    for (const auto& [id, value] : message.Values)
    {
        if (id >= 164 || id == kInfamyActorValueId || !std::isfinite(value) ||
            (id == 24 && (value < 0.f || actorValuesComponent.CurrentActorValues.Scale.Life)))
            continue;
        actorValuesComponent.CurrentActorValues.ActorMaxValuesList[id] = value;
        notify.Values[id] = value;
    }

    if (notify.Values.empty())
        return;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnHealthChangeBroadcast(const PacketEvent<RequestHealthChangeBroadcast>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    if (!acMessage.pPlayer || !std::isfinite(message.DeltaHealth) || message.Sequence == 0 || message.OwnershipEpoch == 0)
        return;

    const auto entity = static_cast<entt::entity>(message.Id);
    const auto view = m_world.view<ActorValuesComponent, OwnerComponent, CharacterComponent, CellIdComponent>();
    if (!view.contains(entity))
        return;
    const auto& owner = view.get<OwnerComponent>(entity);
    const auto& character = view.get<CharacterComponent>(entity);
    if (owner.OwnershipEpoch != message.OwnershipEpoch ||
        !acMessage.pPlayer->GetCellComponent().IsInRange(view.get<CellIdComponent>(entity), character.IsDragon()))
        return;
    // A player's health belongs to that player. Spell/PvP effects are observed on its own client.
    if (character.IsPlayer() && owner.GetOwner() != acMessage.pPlayer)
        return;

    auto& component = view.get<ActorValuesComponent>(entity);
    auto& values = component.CurrentActorValues;
    if (component.HealthEpoch != message.OwnershipEpoch)
    {
        component.HealthEpoch = message.OwnershipEpoch;
        values.HealthSequences.clear();
    }
    if (values.HealthSequences.size() >= 32 && !values.HealthSequences.contains(acMessage.pPlayer->GetId()))
        return;
    auto& sequence = values.HealthSequences[acMessage.pPlayer->GetId()];
    float delta = 0.f;
    if (message.Sequence > sequence)
    {
        const auto next = Coop::AddHealth(values.ActorValuesList[24], message.DeltaHealth);
        if (next && !m_world.ctx().at<CoopService>().IsDowned(message.Id))
        {
            delta = *next - values.ActorValuesList[24];
            values.ActorValuesList[24] = *next;
            ++values.HealthRevision;
        }
        sequence = message.Sequence; // rejected numeric operations are acknowledged as no-ops too
    }

    NotifyHealthChangeBroadcast notify{};
    notify.Id = message.Id;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Sequence = sequence;
    notify.SourcePlayerId = acMessage.pPlayer->GetId();
    notify.Revision = values.HealthRevision;
    notify.Health = values.ActorValuesList[24];
    notify.DeltaHealth = delta;
    notify.Scale = values.Scale;
    // Include the origin. Its local prediction is retired by this acknowledgement, not added twice.
    GameServer::Get()->SendToPlayersInRange(notify, entity, nullptr);

}

void ActorValueService::OnDeathStateChange(const PacketEvent<RequestDeathStateChange>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();

    const auto it = characterView.find(static_cast<entt::entity>(message.Id));

    if (it == characterView.end() || !characterView.get<OwnerComponent>(*it).IsCurrentOwner(acMessage.pPlayer, message.OwnershipEpoch))
        return;

    auto& characterComponent = characterView.get<CharacterComponent>(*it);
    if (characterComponent.IsPlayer() && m_world.ctx().at<CoopService>().Manages(message.Id))
        return;
    characterComponent.SetDead(message.IsDead);
    spdlog::debug("Updating death state {:x}:{}", message.Id, message.IsDead);

    NotifyDeathStateChange notify;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Id = message.Id;
    notify.IsDead = message.IsDead;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}
