#include <algorithm>
#include <chrono>
#include <cmath>

#include <Events/PacketEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <GameServer.h>
#include <Services/ActorValueService.h>
#include <Services/EncounterService.h>
#include <Services/PartyService.h>
#include <Services/RfabProgressionService.h>
#include <Setting.h>
#include <World.h>
#include <Components.h>
#include <common/RfabModpackCompat.h>

extern Console::Setting<bool> bEnablePvp;

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
    m_playerLeaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&ActorValueService::OnPlayerLeave>(this);
    m_updateHealthConnection = aDispatcher.sink<PacketEvent<RequestActorValueChanges>>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_updateMaxValueConnection = aDispatcher.sink<PacketEvent<RequestActorMaxValueChanges>>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_updateDeltaHealthConnection = aDispatcher.sink<PacketEvent<RequestHealthChangeBroadcast>>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_deathStateConnection = aDispatcher.sink<PacketEvent<RequestDeathStateChange>>().connect<&ActorValueService::OnDeathStateChange>(this);
}

void ActorValueService::RecordCombatClaim(
    uint32_t aAttackerPlayerId,
    uint32_t aTargetServerId,
    CoopDamagePipeline::ServerHitClaimKind aKind) const noexcept
{
    m_combatAuthorityLedger.RecordCombatClaim(aAttackerPlayerId, aTargetServerId, aKind, SteadyActorValueNowMs());
}

void ActorValueService::RecordPlayerDownedState(uint32_t aPlayerId, bool aIsDowned) const noexcept
{
    m_combatAuthorityLedger.RecordPlayerDownedState(aPlayerId, aIsDowned, SteadyActorValueNowMs());
}

void ActorValueService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) const noexcept
{
    if (acEvent.pPlayer)
        m_combatAuthorityLedger.ForgetPlayer(acEvent.pPlayer->GetId());
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
            const float currentHp = (curIt != actorValuesComponent.CurrentActorValues.ActorValuesList.end()) ? curIt->second : 0.f;

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
    if (!std::isfinite(message.DeltaHealth))
        return;

    const float sanitizedStagger = std::isfinite(message.StaggerMagnitude)
        ? std::clamp(message.StaggerMagnitude, 0.0f, 1.0f)
        : 0.0f;
    const float sanitizedDir = std::isfinite(message.StaggerDirection)
        ? std::clamp(message.StaggerDirection, 0.0f, 0.9999f)
        : 0.0f;

    if (message.DeltaHealth == 0.f && sanitizedStagger <= 0.05f)
        return;

    const float sanitizedDelta = std::clamp(message.DeltaHealth, -kMaxSanitizedActorValue, kMaxSanitizedActorValue);

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();
    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));
    if (it == actorValuesView.end())
        return;

    const auto& ownerComponent = actorValuesView.get<OwnerComponent>(*it);
    const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(*it);
    const bool isDragon = pCharacterComponent && pCharacterComponent->IsDragon();
    const bool isOwner = (ownerComponent.GetOwner() == acMessage.pPlayer);
    const uint64_t nowMs = SteadyActorValueNowMs();

    bool isSenderInRange = (acMessage.pPlayer != nullptr);
    if (const auto* pTargetCell = m_world.try_get<CellIdComponent>(*it))
    {
        isSenderInRange = acMessage.pPlayer && pTargetCell->IsInRange(acMessage.pPlayer->GetCellComponent(), isDragon);
        if (!isSenderInRange)
            return;
    }

    if (pCharacterComponent)
    {
        // Reject late regen/heal or damage packets against an already-dead non-player NPC.
        if (!pCharacterComponent->IsPlayer() && pCharacterComponent->IsDead())
            return;
    }

    uint32_t sanitizedSourceId = 0;
    if (message.SourceServerId != 0 && acMessage.pPlayer)
    {
        const auto srcEntity = static_cast<entt::entity>(message.SourceServerId);
        if (m_world.valid(srcEntity))
        {
            if (const auto* pSrcOwner = m_world.try_get<OwnerComponent>(srcEntity);
                pSrcOwner && pSrcOwner->GetOwner() == acMessage.pPlayer)
            {
                sanitizedSourceId = message.SourceServerId;
            }
        }
    }

    if (!isOwner)
    {
        const uint32_t senderId = acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0;
        const bool isSenderDowned = senderId != 0 &&
            m_world.GetEncounterService().GetCoordinator().IsPlayerDowned(senderId);
        m_combatAuthorityLedger.RecordPlayerDownedState(senderId, isSenderDowned, nowMs);

        bool areCoopPartyAllies = false;
        if (pCharacterComponent && acMessage.pPlayer &&
            (pCharacterComponent->IsPlayer() || pCharacterComponent->IsPlayerSummon()))
        {
            Player* pTargetOwner = ownerComponent.GetOwner();
            if (pTargetOwner && pTargetOwner != acMessage.pPlayer)
            {
                const auto* pParty = m_world.GetPartyService().GetPlayerParty(acMessage.pPlayer);
                if (pParty && std::find(pParty->Members.begin(), pParty->Members.end(), pTargetOwner) != pParty->Members.end())
                    areCoopPartyAllies = true;
            }
        }

        const bool isPvpEnabled = static_cast<bool>(bEnablePvp);
        const bool isTargetDead = pCharacterComponent && pCharacterComponent->IsDead();
        const float authDelta = (sanitizedDelta != 0.0f) ? sanitizedDelta : -0.01f;
        if (!m_combatAuthorityLedger.ValidateNonOwnerHealthDelta(
                senderId,
                message.Id,
                authDelta,
                isSenderInRange,
                isSenderDowned,
                isTargetDead,
                areCoopPartyAllies,
                isPvpEnabled,
                nowMs))
        {
            spdlog::debug(
                "Rejected unauthorized non-owner health delta {:.1f} on {:x} from player {} (downed={}, partyAlly={})",
                sanitizedDelta,
                message.Id,
                senderId,
                isSenderDowned,
                areCoopPartyAllies);
            return;
        }

        if (m_world.GetRfabProgressionService().GetLedger().IsBossGhost(message.Id))
        {
            spdlog::debug("Blocked non-owner damage {:.1f} on invulnerable boss ghost {:x}", sanitizedDelta, message.Id);
            return;
        }
    }

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    if ((sanitizedDelta < 0.f || sanitizedStagger > 0.05f) && !isOwner)
        actorValuesComponent.LastExternalDamageAtMs = nowMs;

    const auto maxIt = actorValuesComponent.CurrentActorValues.ActorMaxValuesList.find(kHealthActorValueIndex);
    const float maxHealth = (maxIt != actorValuesComponent.CurrentActorValues.ActorMaxValuesList.end() && std::isfinite(maxIt->second))
        ? maxIt->second
        : 0.f;
    const auto curIt = actorValuesComponent.CurrentActorValues.ActorValuesList.find(kHealthActorValueIndex);
    const float currentHealth = (curIt != actorValuesComponent.CurrentActorValues.ActorValuesList.end() && std::isfinite(curIt->second))
        ? curIt->second
        : maxHealth;

    const auto applied = CoopDamagePipeline::ApplyAuthoritativeServerHealthDelta(
        currentHealth,
        maxHealth,
        sanitizedDelta);

    actorValuesComponent.CurrentActorValues.ActorValuesList[kHealthActorValueIndex] = applied.UpdatedHealth;

    NotifyHealthChangeBroadcast notify;
    notify.Id = message.Id;
    notify.DeltaHealth = applied.ClampedBroadcastDelta;
    notify.SourceServerId = sanitizedSourceId;
    notify.StaggerMagnitude = (!isOwner) ? sanitizedStagger : 0.0f;
    notify.StaggerDirection = (!isOwner) ? sanitizedDir : 0.0f;

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
