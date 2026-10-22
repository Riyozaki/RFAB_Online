#include <algorithm>
#include <cmath>

#include <Components.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestDeathStateChange.h>
#include <Gameplay/ActorValueIds.h>
#include <Services/ActorValueService.h>
#include <World.h>
#include <GameServer.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>

ActorValueService::ActorValueService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateHealthConnection = aDispatcher.sink<PacketEvent<RequestActorValueChanges>>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_updateMaxValueConnection = aDispatcher.sink<PacketEvent<RequestActorMaxValueChanges>>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_updateDeltaHealthConnection = aDispatcher.sink<PacketEvent<RequestHealthChangeBroadcast>>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_deathStateConnection = aDispatcher.sink<PacketEvent<RequestDeathStateChange>>().connect<&ActorValueService::OnDeathStateChange>(this);
}

namespace
{
// Only the client that simulates an actor may change its values. Without this
// check any client could write into any actor's health, which is both a
// desync source ("fighting over NPC ownership") and a trivial cheat.
bool IsSenderOwnerOf(World& aWorld, entt::entity aEntity, Player* apPlayer) noexcept
{
    const auto* pOwner = aWorld.try_get<OwnerComponent>(aEntity);
    if (pOwner == nullptr)
        return false;

    return pOwner->GetOwner() == apPlayer;
}

// A NaN or infinity coming from a game bug (or a hacked client) would stick to
// the actor forever: every receiving client would clamp against NaN and end up
// with an unkillable or instantly dead actor. Such values are dropped.
bool IsSaneActorValue(float aValue) noexcept
{
    return std::isfinite(aValue) && std::fabs(aValue) < 1.0e9f;
}
} // namespace

void ActorValueService::OnActorValueChanges(const PacketEvent<RequestActorValueChanges>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!IsSenderOwnerOf(m_world, cEntity, acMessage.pPlayer))
    {
        spdlog::debug("{}: ignored actor value change for {:X} from a non-owner", __FUNCTION__, message.Id);
        return;
    }

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();

    auto it = actorValuesView.find(cEntity);

    if (it != actorValuesView.end())
    {
        auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
        for (auto& [id, value] : message.Values)
        {
            if (!IsSaneActorValue(value))
            {
                spdlog::warn("{}: dropped non-finite value {} for actor value {}", __FUNCTION__, value, id);
                continue;
            }

            actorValuesComponent.CurrentActorValues.ActorValuesList[id] = value;
        }
    }

    NotifyActorValueChanges notify;
    notify.Id = acMessage.Packet.Id;
    notify.Values = acMessage.Packet.Values;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnActorMaxValueChanges(const PacketEvent<RequestActorMaxValueChanges>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!IsSenderOwnerOf(m_world, cEntity, acMessage.pPlayer))
    {
        spdlog::debug("{}: ignored actor max value change for {:X} from a non-owner", __FUNCTION__, message.Id);
        return;
    }

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();

    auto it = actorValuesView.find(cEntity);

    if (it != actorValuesView.end())
    {
        auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);

        auto& values = actorValuesComponent.CurrentActorValues.ActorValuesList;
        auto& maxValues = actorValuesComponent.CurrentActorValues.ActorMaxValuesList;

        for (auto& [id, value] : message.Values)
        {
            if (!IsSaneActorValue(value) || value < 0.0f)
            {
                spdlog::warn("{}: dropped invalid max value {} for actor value {}", __FUNCTION__, value, id);
                continue;
            }

            maxValues[id] = value;

            // A maximum that shrank (a buff expired, gear was removed) must not
            // leave the stored current value above it: late joiners receive this
            // pair verbatim and would spawn with more health than the maximum.
            if (id == Gameplay::ActorValue::kHealth)
            {
                const auto currentIt = values.find(id);
                if (currentIt != values.end() && currentIt->second > value)
                    currentIt->second = value;
            }
        }
    }

    NotifyActorMaxValueChanges notify;
    notify.Id = message.Id;
    notify.Values = message.Values;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnHealthChangeBroadcast(const PacketEvent<RequestHealthChangeBroadcast>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!IsSenderOwnerOf(m_world, cEntity, acMessage.pPlayer))
    {
        spdlog::debug("{}: ignored health change for {:X} from a non-owner", __FUNCTION__, message.Id);
        return;
    }

    // DeltaHealth is a *delta*: damage arrives negative (see
    // Actor::HookDamageActor -> HealthChangeEvent(..., -realDamage)), healing
    // arrives positive, and every receiving client applies `current + delta`.
    // The server used to apply `current - delta`, i.e. damage increased the
    // server-side health that late joiners receive on spawn.
    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();

    auto it = actorValuesView.find(cEntity);

    if (it != actorValuesView.end())
    {
        auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);

        auto& values = actorValuesComponent.CurrentActorValues.ActorValuesList;
        const auto healthIt = values.find(Gameplay::ActorValue::kHealth);
        if (healthIt != values.end())
        {
            const float currentHealth = healthIt->second;
            float newHealth = currentHealth + message.DeltaHealth;

            // Clamp to the known maximum (if any) so that accumulated deltas can
            // never push the stored health above the actor's maximum, which is
            // what late joiners would then see.
            const auto maxIt = actorValuesComponent.CurrentActorValues.ActorMaxValuesList.find(Gameplay::ActorValue::kHealth);
            if (maxIt != actorValuesComponent.CurrentActorValues.ActorMaxValuesList.end() && maxIt->second > 0.f)
                newHealth = std::min(newHealth, maxIt->second);

            newHealth = std::max(newHealth, 0.f);

            healthIt->second = newHealth;
        }
    }

    NotifyHealthChangeBroadcast notify;
    notify.Id = message.Id;
    notify.DeltaHealth = message.DeltaHealth;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnDeathStateChange(const PacketEvent<RequestDeathStateChange>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!IsSenderOwnerOf(m_world, cEntity, acMessage.pPlayer))
    {
        spdlog::debug("{}: ignored death state change for {:X} from a non-owner", __FUNCTION__, message.Id);
        return;
    }

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();

    const auto it = characterView.find(cEntity);

    if (it != characterView.end())
    {
        auto& characterComponent = characterView.get<CharacterComponent>(*it);

        // The local player is never killed by the simulation (the client refuses
        // to kill players, see ActorValueService::OnHealthChangeBroadcast), so a
        // "my player died" packet is either a desync or an exploit.
        if (message.IsDead && characterComponent.IsPlayer())
        {
            spdlog::warn("{}: refusing to mark player character {:X} as dead", __FUNCTION__, message.Id);
            return;
        }

        characterComponent.SetDead(message.IsDead);
        spdlog::debug("Updating death state {:x}:{}", message.Id, message.IsDead);
    }

    NotifyDeathStateChange notify;
    notify.Id = message.Id;
    notify.IsDead = message.IsDead;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}
