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
#include <Authority/Health.h>
#include <Authority/Hits.h>

namespace
{
constexpr uint32_t kHealthActorValueIndex = 24;
constexpr uint32_t kInfamyActorValueIndex = 37;
constexpr uint32_t kActorValueCount = 164;
constexpr float kMaxSanitizedActorValue = 100000.0f;

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
    Authority::HitKind aKind) const noexcept
{
    m_combatAuthorityLedger.RecordCombatClaim(aAttackerPlayerId, aTargetServerId, aKind, SteadyActorValueNowMs());
}

void ActorValueService::ReportAudit(uint64_t aNowMs) const noexcept
{
    using Authority::AuditEvent;

    if (!m_combatAudit.ShouldReport(aNowMs))
        return;

    spdlog::warn(
        "Combat authority refused: {} hit(s) from players not allowed to deal them, {} repeated "
        "swing(s), {} stale health reading(s) ({} sender(s) corrected). Totals since start: "
        "{}/{}/{}.",
        m_combatAudit.WindowCount(AuditEvent::kRejectedHit),
        m_combatAudit.WindowCount(AuditEvent::kDuplicateHit),
        m_combatAudit.WindowCount(AuditEvent::kStaleSnapshot),
        m_combatAudit.WindowCount(AuditEvent::kSnapshotCorrection),
        m_combatAudit.TotalCount(AuditEvent::kRejectedHit),
        m_combatAudit.TotalCount(AuditEvent::kDuplicateHit),
        m_combatAudit.TotalCount(AuditEvent::kStaleSnapshot));
}

void ActorValueService::LogCombatAudit() const noexcept
{
    using Authority::AuditEvent;

    auto pOut = spdlog::get("ConOut");
    if (!pOut)
        return;

    if (!m_combatAudit.AnythingRecorded())
    {
        pOut->info("Combat authority has refused nothing since this server started.");
        return;
    }

    pOut->info("Combat authority, since this server started:");
    pOut->info("  hits from players not allowed to deal them : {}", m_combatAudit.TotalCount(AuditEvent::kRejectedHit));
    pOut->info("  the same swing arriving more than once     : {}", m_combatAudit.TotalCount(AuditEvent::kDuplicateHit));
    pOut->info("  health readings that arrived out of date   : {}", m_combatAudit.TotalCount(AuditEvent::kStaleSnapshot));
    pOut->info("  senders told the authoritative value       : {}", m_combatAudit.TotalCount(AuditEvent::kSnapshotCorrection));
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
    const entt::entity targetEntity = static_cast<entt::entity>(message.Id);
    if (!acMessage.pPlayer || !m_world.valid(targetEntity))
        return;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();
    auto it = actorValuesView.find(targetEntity);
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
    for (const auto& [id, value] : message.Values)
    {
        if (id >= kActorValueCount || id == kInfamyActorValueIndex || !std::isfinite(value))
            continue;

        const float sanitized = std::clamp(value, -kMaxSanitizedActorValue, kMaxSanitizedActorValue);

        if (id == kHealthActorValueIndex && pCharComp && !pCharComp->IsPlayer())
        {
            if (pCharComp->IsDead())
            {
                // A corpse's health is not something a client reports: death fixed it at
                // zero and moved the revision. Letting the value through here is how a body
                // gets its health back - the sender is an owner whose reading was sampled
                // before the killing blow, and its revision is already out of date, so the
                // write would be refused a few lines lower if it ever reached the
                // classifier. It does not: the whole block used to be skipped for a dead
                // actor, and the value was assigned straight into the component.
                //
                // Answering with the truth (not silence) matters just as much: without the
                // correction the sender's revision would never advance and every later
                // reading of its own would be refused as well.
                NotifyActorValueChanges correction;
                correction.Id = message.Id;
                correction.HealthRevision = actorValuesComponent.HealthRevision;
                float currentHp = 0.0f;
                (void)actorValuesComponent.TryGetHealth(currentHp);
                correction.Values[kHealthActorValueIndex] = currentHp;

                acMessage.pPlayer->Send(correction);

                m_combatAudit.Record(Authority::AuditEvent::kStaleSnapshot);
                m_combatAudit.Record(Authority::AuditEvent::kSnapshotCorrection);
                ReportAudit(SteadyActorValueNowMs());
                continue;
            }

            const auto curIt = actorValuesComponent.CurrentActorValues.ActorValuesList.find(kHealthActorValueIndex);

            if (curIt != actorValuesComponent.CurrentActorValues.ActorValuesList.end())
            {
                const float currentHp = curIt->second;

                // An owner restating health it read before someone else's hit landed would
                // hand that health back. The verdict comes from the revision the sender
                // echoed, not from how long ago the hit was.
                const auto verdict = Authority::ClassifyHealthSnapshot(
                    currentHp, sanitized, actorValuesComponent.HealthRevision, message.HealthRevision);

                if (verdict == Authority::SnapshotVerdict::kStaleIncrease)
                {
                    // Refusing in silence would leave this client behind for good: its
                    // revision would never advance and every later reading would be
                    // refused too. Answer with what is actually true.
                    NotifyActorValueChanges correction;
                    correction.Id = message.Id;
                    correction.HealthRevision = actorValuesComponent.HealthRevision;
                    correction.Values[kHealthActorValueIndex] = currentHp;
                    acMessage.pPlayer->Send(correction);

                    m_combatAudit.Record(Authority::AuditEvent::kStaleSnapshot);
                    m_combatAudit.Record(Authority::AuditEvent::kSnapshotCorrection);
                    ReportAudit(SteadyActorValueNowMs());
                    continue;
                }

                if (verdict == Authority::SnapshotVerdict::kMalformed)
                    continue;
            }

            // One writer for health: the value and the revision that describes it move
            // together, so a reading sampled before this moment can no longer pass the
            // classifier as current.
            float maxHealth = 0.0f;
            (void)actorValuesComponent.TryGetMaxHealth(maxHealth);
            actorValuesComponent.SetHealth(sanitized, maxHealth);

            float storedHealth = sanitized;
            (void)actorValuesComponent.TryGetHealth(storedHealth);
            notify.Values[id] = storedHealth;
            continue;
        }

        actorValuesComponent.CurrentActorValues.ActorValuesList[id] = sanitized;
        notify.Values[id] = sanitized;
    }

    if (notify.Values.empty())
        return;

    notify.HealthRevision = actorValuesComponent.HealthRevision;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnActorMaxValueChanges(const PacketEvent<RequestActorMaxValueChanges>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const entt::entity targetEntity = static_cast<entt::entity>(message.Id);
    if (!acMessage.pPlayer || !m_world.valid(targetEntity))
        return;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();
    auto it = actorValuesView.find(targetEntity);
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
        if (id >= kActorValueCount || id == kInfamyActorValueIndex || !std::isfinite(value))
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
    const entt::entity targetEntity = static_cast<entt::entity>(message.Id);
    if (!acMessage.pPlayer || !m_world.valid(targetEntity) || !std::isfinite(message.DeltaHealth))
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
    auto it = actorValuesView.find(targetEntity);
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

    const uint32_t senderId = acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0;

    if (!isOwner)
    {
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
            m_combatAudit.Record(Authority::AuditEvent::kRejectedHit);
            ReportAudit(nowMs);
            return;
        }

        if (m_world.GetRfabProgressionService().GetLedger().IsBossGhost(message.Id))
        {
            spdlog::debug("Blocked non-owner damage {:.1f} on invulnerable boss ghost {:x}", sanitizedDelta, message.Id);
            return;
        }
    }

    // One swing, one application: the same packet arriving twice must not cost the target
    // twice. Checked after the rules, so a refused hit does not burn its id.
    if (m_combatAuthorityLedger.IsDuplicateHit(senderId, message.Id, message.HitId))
    {
        spdlog::debug("Ignored duplicate hit {} on {:x} from player {}", message.HitId, message.Id, senderId);
        m_combatAudit.Record(Authority::AuditEvent::kDuplicateHit);
        ReportAudit(nowMs);
        return;
    }

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);

    const auto maxIt = actorValuesComponent.CurrentActorValues.ActorMaxValuesList.find(kHealthActorValueIndex);
    const float maxHealth = (maxIt != actorValuesComponent.CurrentActorValues.ActorMaxValuesList.end() && std::isfinite(maxIt->second))
        ? maxIt->second
        : 0.f;
    const auto curIt = actorValuesComponent.CurrentActorValues.ActorValuesList.find(kHealthActorValueIndex);
    const float currentHealth = (curIt != actorValuesComponent.CurrentActorValues.ActorValuesList.end() && std::isfinite(curIt->second))
        ? curIt->second
        : maxHealth;

    const auto applied = Authority::ApplyHealthDelta(
        currentHealth,
        std::max(maxHealth, currentHealth),
        sanitizedDelta);

    // Health changed, so the revision moves with it - in the one function allowed to
    // write it. Every absolute reading sampled before this moment is now out of date and
    // says so by carrying the previous number.
    actorValuesComponent.SetHealth(applied.UpdatedHealth, maxHealth);

    NotifyHealthChangeBroadcast notify;
    notify.Id = message.Id;
    notify.DeltaHealth = applied.ClampedBroadcastDelta;
    notify.SourceServerId = sanitizedSourceId;
    notify.StaggerMagnitude = (!isOwner) ? sanitizedStagger : 0.0f;
    notify.StaggerDirection = (!isOwner) ? sanitizedDir : 0.0f;
    notify.HealthRevision = actorValuesComponent.HealthRevision;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnDeathStateChange(const PacketEvent<RequestDeathStateChange>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const entt::entity targetEntity = static_cast<entt::entity>(message.Id);
    if (!acMessage.pPlayer || !m_world.valid(targetEntity))
        return;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();
    const auto it = characterView.find(targetEntity);
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
        {
            // Death is an absolute write like any other, and it is the one that used to
            // lose the race: without a revision bump, a snapshot sampled just before the
            // killing blow still matched and handed the corpse its health back.
            pActorValues->SetHealth(0.0f);
        }
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
