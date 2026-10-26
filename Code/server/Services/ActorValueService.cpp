#include <algorithm>
#include <chrono>
#include <cmath>

#include <Events/PacketEvent.h>
#include <GameServer.h>
#include <Services/ActorValueService.h>
#include <Services/EncounterService.h>
#include <Services/RfabProgressionService.h>
#include <World.h>
#include <Components.h>
#include <common/RfabModpackCompat.h>

#include <Messages/RequestActorValueChanges.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/NotifyDeathStateChange.h>

namespace
{
constexpr uint32_t kHealthActorValueIndex = 24;
constexpr uint32_t kInfamyActorValueIndex = 37;
constexpr float kMaxSanitizedActorValue = 100000.0f;
constexpr uint64_t kExternalDamageReconcileWindowMs = 650ULL;

uint64_t SteadyActorValueNowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
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

    NotifyActorValueChanges notify;
    notify.Id = acMessage.Packet.Id;

    const auto* pCharComp = m_world.try_get<CharacterComponent>(*it);
    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    const uint64_t nowMs = SteadyActorValueNowMs();
    for (const auto& [id, value] : message.Values)
    {
        if (id == kInfamyActorValueIndex || !std::isfinite(value))
            continue;

        const float sanitized = std::clamp(value, -kMaxSanitizedActorValue, kMaxSanitizedActorValue);

        if (id == kHealthActorValueIndex && pCharComp && !pCharComp->IsPlayer() && !pCharComp->IsDead())
        {
            const auto curIt = actorValuesComponent.CurrentActorValues.ActorValuesList.find(kHealthActorValueIndex);
            const auto maxIt = actorValuesComponent.CurrentActorValues.ActorMaxValuesList.find(kHealthActorValueIndex);
            const float currentHp = (curIt != actorValuesComponent.CurrentActorValues.ActorValuesList.end()) ? curIt->second : 0.f;
            const float maxHp = (maxIt != actorValuesComponent.CurrentActorValues.ActorMaxValuesList.end()) ? maxIt->second : 0.f;
            const bool isBossOrHighHp = pCharComp->IsDragon() || (maxHp >= 800.0f);
            const auto& encounterCoord = m_world.GetEncounterService().GetCoordinator();
            const bool encounterActiveWithLivingAllies =
                encounterCoord.IsEncounterActiveWithLivingParticipants() ||
                (acMessage.pPlayer && encounterCoord.IsPlayerDowned(acMessage.pPlayer->GetId()));

            if (RfabCompat::ShouldBlockFalseBossResetHeal(
                    encounterActiveWithLivingAllies,
                    isBossOrHighHp,
                    currentHp,
                    maxHp,
                    sanitized))
            {
                NotifyHealthChangeBroadcast rollbackOwnerHp{};
                rollbackOwnerHp.Id = message.Id;
                rollbackOwnerHp.DeltaHealth = currentHp - sanitized;
                if (acMessage.pPlayer && rollbackOwnerHp.DeltaHealth < -0.05f)
                    acMessage.pPlayer->Send(rollbackOwnerHp);
                continue;
            }

            if (curIt != actorValuesComponent.CurrentActorValues.ActorValuesList.end() &&
                actorValuesComponent.LastExternalDamageAtMs != 0 &&
                nowMs >= actorValuesComponent.LastExternalDamageAtMs &&
                (nowMs - actorValuesComponent.LastExternalDamageAtMs) <= kExternalDamageReconcileWindowMs &&
                sanitized > currentHp + 0.5f)
            {
                // Suppress stale periodic owner HP snapshots that departed before a peer's
                // RequestHealthChangeBroadcast hit reached the owner client.
                continue;
            }
        }

        actorValuesComponent.CurrentActorValues.ActorValuesList[id] = sanitized;
        notify.Values[id] = sanitized;
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
    if (it == actorValuesView.end())
        return;

    // Reject stale max-value snapshots from an ex-owner right after ownership transfer.
    const auto& ownerComponent = actorValuesView.get<OwnerComponent>(*it);
    if (ownerComponent.GetOwner() != acMessage.pPlayer)
        return;

    NotifyActorMaxValueChanges notify;
    notify.Id = message.Id;

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    for (const auto& [id, value] : message.Values)
    {
        if (id == kInfamyActorValueIndex || !std::isfinite(value))
            continue;

        const float sanitized = std::clamp(value, 0.0f, kMaxSanitizedActorValue);
        actorValuesComponent.CurrentActorValues.ActorMaxValuesList[id] = sanitized;
        notify.Values[id] = sanitized;
    }

    if (notify.Values.empty())
        return;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnHealthChangeBroadcast(const PacketEvent<RequestHealthChangeBroadcast>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    if (!std::isfinite(message.DeltaHealth) || message.DeltaHealth == 0.f)
        return;

    const float sanitizedDelta = std::clamp(message.DeltaHealth, -kMaxSanitizedActorValue, kMaxSanitizedActorValue);

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();
    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));
    if (it == actorValuesView.end())
        return;

    const auto& ownerComponent = actorValuesView.get<OwnerComponent>(*it);
    const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(*it);
    const bool isDragon = pCharacterComponent && pCharacterComponent->IsDragon();

    if (const auto* pTargetCell = m_world.try_get<CellIdComponent>(*it))
    {
        if (!acMessage.pPlayer || !pTargetCell->IsInRange(acMessage.pPlayer->GetCellComponent(), isDragon))
            return;
    }

    if (pCharacterComponent)
    {
        // Reject late regen/heal or damage packets against an already-dead non-player NPC.
        if (!pCharacterComponent->IsPlayer() && pCharacterComponent->IsDead())
            return;
    }

    if (sanitizedDelta < 0.f &&
        ownerComponent.GetOwner() != acMessage.pPlayer &&
        m_world.GetRfabProgressionService().GetLedger().IsBossGhost(message.Id))
    {
        spdlog::debug("Blocked non-owner damage {:.1f} on invulnerable boss ghost {:x}", sanitizedDelta, message.Id);
        return;
    }

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    if (sanitizedDelta < 0.f && ownerComponent.GetOwner() != acMessage.pPlayer)
        actorValuesComponent.LastExternalDamageAtMs = SteadyActorValueNowMs();

    const float currentHealth = actorValuesComponent.CurrentActorValues.ActorValuesList[kHealthActorValueIndex];
    float updatedHealth = std::max(0.f, (std::isfinite(currentHealth) ? currentHealth : 0.f) + sanitizedDelta);

    const auto maxIt = actorValuesComponent.CurrentActorValues.ActorMaxValuesList.find(kHealthActorValueIndex);
    const float maxHealth = (maxIt != actorValuesComponent.CurrentActorValues.ActorMaxValuesList.end() && std::isfinite(maxIt->second))
        ? maxIt->second
        : 0.f;
    if (maxHealth > 0.f)
    {
        updatedHealth = std::min(updatedHealth, maxHealth);
    }

    if (sanitizedDelta > 0.f && pCharacterComponent && !pCharacterComponent->IsPlayer() && !pCharacterComponent->IsDead())
    {
        const bool isBossOrHighHp = pCharacterComponent->IsDragon() || (maxHealth >= 800.0f);
        const auto& encounterCoord = m_world.GetEncounterService().GetCoordinator();
        const bool encounterActiveWithLivingAllies =
            encounterCoord.IsEncounterActiveWithLivingParticipants() ||
            (acMessage.pPlayer && encounterCoord.IsPlayerDowned(acMessage.pPlayer->GetId()));

        if (RfabCompat::ShouldBlockFalseBossResetHeal(
                encounterActiveWithLivingAllies,
                isBossOrHighHp,
                currentHealth,
                maxHealth,
                updatedHealth))
        {
            NotifyHealthChangeBroadcast rollbackOwnerHp{};
            rollbackOwnerHp.Id = message.Id;
            rollbackOwnerHp.DeltaHealth = -sanitizedDelta;
            if (acMessage.pPlayer)
                acMessage.pPlayer->Send(rollbackOwnerHp);
            return;
        }
    }

    actorValuesComponent.CurrentActorValues.ActorValuesList[kHealthActorValueIndex] = updatedHealth;

    NotifyHealthChangeBroadcast notify;
    notify.Id = message.Id;
    notify.DeltaHealth = sanitizedDelta;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnDeathStateChange(const PacketEvent<RequestDeathStateChange>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();
    const auto it = characterView.find(static_cast<entt::entity>(message.Id));
    if (it == characterView.end())
        return;

    auto& characterComponent = characterView.get<CharacterComponent>(*it);
    const auto& ownerComponent = characterView.get<OwnerComponent>(*it);

    if (const auto* pTargetCell = m_world.try_get<CellIdComponent>(*it))
    {
        if (!acMessage.pPlayer || !pTargetCell->IsInRange(acMessage.pPlayer->GetCellComponent(), characterComponent.IsDragon()))
            return;
    }

    // Any peer in range may deal the killing blow (IsDead == true) unless the boss is currently invulnerable/ghosted,
    // whereas resurrecting/respawning an already-dead NPC (IsDead == false) is accepted strictly from the owner.
    if (message.IsDead &&
        ownerComponent.GetOwner() != acMessage.pPlayer &&
        m_world.GetRfabProgressionService().GetLedger().IsBossGhost(message.Id))
    {
        spdlog::debug("Ignored non-owner death packet on invulnerable boss ghost {:x}", message.Id);
        return;
    }

    if (!message.IsDead && characterComponent.IsDead() && ownerComponent.GetOwner() != acMessage.pPlayer)
    {
        spdlog::debug("Ignored stale non-owner resurrection on dead actor {:x}", message.Id);
        return;
    }

    if (characterComponent.IsDead() == message.IsDead)
        return;

    characterComponent.SetDead(message.IsDead);

    if (message.IsDead)
    {
        if (auto* pActorValues = m_world.try_get<ActorValuesComponent>(*it))
            pActorValues->CurrentActorValues.ActorValuesList[kHealthActorValueIndex] = 0.f;
        m_world.GetRfabProgressionService().GetLedger().ClearBossGhostState(message.Id);
    }

    spdlog::debug("Updating death state {:x}:{}", message.Id, message.IsDead);

    NotifyDeathStateChange notify;
    notify.Id = message.Id;
    notify.IsDead = message.IsDead;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}
