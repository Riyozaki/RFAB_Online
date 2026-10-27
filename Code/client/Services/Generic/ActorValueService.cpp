#include <TiltedOnlinePCH.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include <Services/ActorValueService.h>
#include <World.h>
#include <Forms/ActorValueInfo.h>
#include <Games/Overrides.h>
#include <Games/References.h>
#include <Misc/BSFixedString.h>
#include <Components.h>

namespace
{
[[nodiscard]] uint64_t ClientAvSteadyNowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

#include <Events/UpdateEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/HealthChangeEvent.h>

#include <Messages/NotifyActorValueChanges.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/RequestDeathStateChange.h>

#include <Misc/ActorValueOwner.h>

ActorValueService::ActorValueService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_world.on_construct<LocalComponent>().connect<&ActorValueService::OnLocalComponentAdded>(this);
    m_world.on_destroy<LocalComponent>().connect<&ActorValueService::OnLocalComponentRemoved>(this);
    m_dispatcher.sink<DisconnectedEvent>().connect<&ActorValueService::OnDisconnected>(this);
    m_dispatcher.sink<ActorRemovedEvent>().connect<&ActorValueService::OnActorRemoved>(this);
    m_dispatcher.sink<UpdateEvent>().connect<&ActorValueService::OnUpdate>(this);
    m_dispatcher.sink<NotifyActorValueChanges>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_dispatcher.sink<NotifyActorMaxValueChanges>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_dispatcher.sink<HealthChangeEvent>().connect<&ActorValueService::OnHealthChange>(this);
    m_dispatcher.sink<NotifyHealthChangeBroadcast>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_dispatcher.sink<NotifyDeathStateChange>().connect<&ActorValueService::OnDeathStateChange>(this);
}

void ActorValueService::CreateActorValuesComponent(const entt::entity aEntity, Actor* apActor) noexcept
{
    auto& actorValuesComponent = m_world.emplace_or_replace<ActorValuesComponent>(aEntity);
    actorValuesComponent.FormId = apActor->formID;

    for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
    {
        float value = apActor->GetActorValue(i);
        actorValuesComponent.CurrentActorValues.ActorValuesList.insert({i, value});
        float maxValue = apActor->GetActorPermanentValue(i);
        actorValuesComponent.CurrentActorValues.ActorMaxValuesList.insert({i, maxValue});
    }
}

void ActorValueService::OnLocalComponentAdded(entt::registry& aRegistry, const entt::entity aEntity) noexcept
{
    const auto& formIdComponent = aRegistry.get<FormIdComponent>(aEntity);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (pActor != NULL)
    {
        auto& localComponent = aRegistry.get<LocalComponent>(aEntity);
        localComponent.IsDead = pActor->IsDead();
        localComponent.IsWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        // v3.0 Phase 4A: PERMANENT kHealth remains 100% native RFAB.esp MaxHP; enemy toughness is
        // applied by scaling incoming health damage (`1 / M`) in HookRestoreActorValue.
        CreateActorValuesComponent(aEntity, pActor);
    }
}

void ActorValueService::OnLocalComponentRemoved(entt::registry& aRegistry, const entt::entity aEntity) noexcept
{
    if (aRegistry.all_of<ActorValuesComponent>(aEntity))
        aRegistry.remove<ActorValuesComponent>(aEntity);
}

void ActorValueService::ResetForWorldReload() noexcept
{
    m_smallHealthChanges.clear();
    m_smallHealthAttackerIds.clear();
    m_lastLocalPredictedHitMs.clear();
    m_remoteHitImpactGate.Clear();
    m_world.clear<ActorValuesComponent>();
}

void ActorValueService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    (void)acEvent;
    ResetForWorldReload();
}

void ActorValueService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    const uint32_t formId = acEvent.FormId;

    if (!m_transport.IsConnected())
        return;

    if (const entt::entity entity = m_world.GetCharacterService().FindActorEntityByFormId(formId); entity != entt::null)
        m_world.remove<ActorValuesComponent>(entity);
}

void ActorValueService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    RunSmallHealthUpdates();
    RunDeathStateUpdates();
    RunActorValuesUpdates();
}

void ActorValueService::BroadcastActorValues() noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent, ActorValuesComponent>();

    for (auto entity : view)
    {
        auto& formIdComponent = view.get<FormIdComponent>(entity);
        auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);

        if (!pActor)
            continue;

        auto& localComponent = view.get<LocalComponent>(entity);
        auto& actorValuesComponent = view.get<ActorValuesComponent>(entity);

        RequestActorValueChanges requestValueChanges;
        requestValueChanges.Id = localComponent.Id;
        RequestActorMaxValueChanges requestMaxValueChanges;
        requestMaxValueChanges.Id = localComponent.Id;

        bool isPlayer = pActor->GetExtension() && pActor->GetExtension()->IsPlayer();

        for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
        {
            if ((isPlayer && i == ActorValueInfo::kDragonSouls) || i == ActorValueInfo::kInfamy)
                continue;
            
            float newValue = pActor->GetActorValue(i);
            float oldValue = actorValuesComponent.CurrentActorValues.ActorValuesList[i];
            if (newValue != oldValue)
            {
                requestValueChanges.Values.insert({i, newValue});
                actorValuesComponent.CurrentActorValues.ActorValuesList[i] = newValue;
            }

            float newMaxValue = pActor->GetActorPermanentValue(i);
            float oldMaxValue = actorValuesComponent.CurrentActorValues.ActorMaxValuesList[i];
            if (newMaxValue != oldMaxValue)
            {
                requestMaxValueChanges.Values.insert({i, newMaxValue});
                actorValuesComponent.CurrentActorValues.ActorMaxValuesList[i] = newMaxValue;
            }
        }

        if (requestValueChanges.Values.size() > 0)
        {
            m_transport.Send(requestValueChanges);
        }

        if (requestMaxValueChanges.Values.size() > 0)
        {
            m_transport.Send(requestMaxValueChanges);
        }
    }
}

void ActorValueService::OnHealthChange(const HealthChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    const entt::entity hitteeEntity = m_world.GetCharacterService().FindActorEntityByFormId(acEvent.HitteeId);
    if (hitteeEntity == entt::null)
    {
        spdlog::debug("Health change event form id component not found, form id: {:X}", acEvent.HitteeId);
        return;
    }

    if (auto* pWaitAssign = m_world.try_get<WaitingForAssignmentComponent>(hitteeEntity))
    {
        pWaitAssign->PendingHealthDelta += acEvent.DeltaHealth;
        spdlog::debug(
            "{}: actor {:X} still WaitingForAssignment, accumulated deferred health delta {:.2f}",
            __FUNCTION__,
            acEvent.HitteeId,
            pWaitAssign->PendingHealthDelta);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(hitteeEntity);
    if (!serverIdRes.has_value())
    {
        spdlog::debug("{}: skipping health delta on unassigned entity {:X}", __FUNCTION__, acEvent.HitteeId);
        return;
    }

    uint32_t serverId = serverIdRes.value();

    uint32_t sourceServerId = 0;
    if (acEvent.AttackerFormId != 0)
    {
        const entt::entity attackerEntity = m_world.GetCharacterService().FindActorEntityByFormId(acEvent.AttackerFormId);
        if (attackerEntity != entt::null)
        {
            if (const auto srcRes = Utils::GetServerId(attackerEntity); srcRes.has_value())
                sourceServerId = srcRes.value();
        }
    }

    if (m_world.all_of<RemoteComponent>(hitteeEntity) && acEvent.DeltaHealth < 0.0f)
    {
        if (m_lastLocalPredictedHitMs.size() > 256)
            m_lastLocalPredictedHitMs.clear();
        m_lastLocalPredictedHitMs[serverId] = ClientAvSteadyNowMs();
    }

    const bool hasImmediateImpact = std::isfinite(acEvent.StaggerMagnitude) && acEvent.StaggerMagnitude > 0.05f;
    if (!hasImmediateImpact && acEvent.DeltaHealth > -1.0f && acEvent.DeltaHealth < 1.0f)
    {
        if (m_smallHealthChanges.find(serverId) == m_smallHealthChanges.end())
            m_smallHealthChanges[serverId] = acEvent.DeltaHealth;
        else
            m_smallHealthChanges[serverId] += acEvent.DeltaHealth;

        if (sourceServerId != 0)
            m_smallHealthAttackerIds[serverId] = sourceServerId;
        return;
    }

    float totalDelta = acEvent.DeltaHealth;
    if (const auto smallIt = m_smallHealthChanges.find(serverId); smallIt != m_smallHealthChanges.end())
    {
        totalDelta += smallIt->second;
        m_smallHealthChanges.erase(smallIt);
        if (sourceServerId == 0)
        {
            if (const auto srcIt = m_smallHealthAttackerIds.find(serverId); srcIt != m_smallHealthAttackerIds.end())
                sourceServerId = srcIt->second;
        }
        m_smallHealthAttackerIds.erase(serverId);
    }

    RequestHealthChangeBroadcast requestHealthChange;
    requestHealthChange.Id = serverId;
    requestHealthChange.DeltaHealth = totalDelta;
    requestHealthChange.SourceServerId = sourceServerId;
    requestHealthChange.StaggerMagnitude = hasImmediateImpact ? acEvent.StaggerMagnitude : 0.0f;
    requestHealthChange.StaggerDirection = hasImmediateImpact ? acEvent.StaggerDirection : 0.0f;

    m_transport.Send(requestHealthChange);

    spdlog::debug("Sent out delta health through collection: {:X}:{:f}", serverId, totalDelta);
}

void ActorValueService::BroadcastCombatHealthSnapshots() noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent, ActorValuesComponent>();
    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor || pActor->IsStillLoading() || pActor->IsDeleted() || pActor->IsDead() || !pActor->IsInCombat())
            continue;

        const auto* pExt = pActor->GetExtension();
        if (pExt && pExt->IsPlayer())
            continue;

        auto& localComponent = view.get<LocalComponent>(entity);
        auto& actorValuesComponent = view.get<ActorValuesComponent>(entity);

        const float newHp = pActor->GetActorValue(ActorValueInfo::kHealth);
        const float oldHp = actorValuesComponent.CurrentActorValues.ActorValuesList[ActorValueInfo::kHealth];
        if (std::abs(newHp - oldHp) > 0.5f)
        {
            actorValuesComponent.CurrentActorValues.ActorValuesList[ActorValueInfo::kHealth] = newHp;
            RequestActorValueChanges req;
            req.Id = localComponent.Id;
            req.Values.insert({ActorValueInfo::kHealth, newHp});
            m_transport.Send(req);
        }
    }
}

void ActorValueService::RunSmallHealthUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    if (!m_smallHealthChanges.empty())
    {
        for (auto& value : m_smallHealthChanges)
        {
            if (std::abs(value.second) <= 0.0005f)
                continue;

            RequestHealthChangeBroadcast requestHealthChange;
            requestHealthChange.Id = value.first;
            requestHealthChange.DeltaHealth = value.second;
            if (const auto srcIt = m_smallHealthAttackerIds.find(value.first); srcIt != m_smallHealthAttackerIds.end())
                requestHealthChange.SourceServerId = srcIt->second;

            m_transport.Send(requestHealthChange);

            spdlog::debug("Sent out delta health through timer, {:X}:{:f}", value.first, value.second);
        }

        m_smallHealthChanges.clear();
        m_smallHealthAttackerIds.clear();
    }

    BroadcastCombatHealthSnapshots();
}

void ActorValueService::RunDeathStateUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto localView = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : localView)
    {
        const auto& formIdComponent = localView.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        auto& localComponent = localView.get<LocalComponent>(entity);

        bool isDead = pActor->IsDead();
        if (isDead != localComponent.IsDead)
        {
            localComponent.IsDead = isDead;

            RequestDeathStateChange requestChange;
            requestChange.Id = localComponent.Id;
            requestChange.IsDead = isDead;

            m_transport.Send(requestChange);
        }
    }
}

void ActorValueService::RunActorValuesUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    BroadcastActorValues();
}

void ActorValueService::OnHealthChangeBroadcast(const NotifyHealthChangeBroadcast& acMessage) const noexcept
{
    if (acMessage.DeltaHealth != 0.0f)
        m_world.GetCharacterService().SyncRemoteCachedHealthDelta(acMessage.Id, acMessage.DeltaHealth);

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id);
    if (!pActor || pActor->IsStillLoading() || pActor->IsDeleted())
        return;

    if (acMessage.DeltaHealth != 0.0f && std::isfinite(acMessage.DeltaHealth))
    {
        ScopedActorValueOverride avOverride;
        // Let Skyrim's native `actorValueOwner.ForceCurrent(ForceMode::DAMAGE, ...)` cap the DAMAGE modifier at 0.0f
        // (`Base + Permanent + Temporary`) rather than clamping to `GetActorPermanentValue(kHealth)`, which would chop
        // active RFAB `TEMPORARY` Fortify Health buffs (`Transmute Muscles`, `Beef Stew`, `Arkay`, `Elixir of Vitality`)!
        const float newHealth = std::clamp(pActor->GetActorValue(ActorValueInfo::kHealth) + acMessage.DeltaHealth, 0.0f, 100000.0f);
        pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, newHealth);
    }

    const float health = pActor->GetActorValue(ActorValueInfo::kHealth);
    ActorExtension* pExtension = pActor->GetExtension();
    if (!pActor->IsDead() && health <= 0.f)
    {
        if (pExtension && !pExtension->IsPlayer())
        {
            if (pExtension->IsRemote())
            {
                // Non-owner peer: suppress duplicate local RFAB_XP_Alias.psc award via kInfamy = -1.
                ScopedActorValueOverride avOverride;
                pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, -1.0f);
                pActor->Kill(nullptr);
            }
            else
            {
                // Authoritative owner of this NPC when a party ally landed the final blow (joint kill!):
                // credit PlayerCharacter::Get() as killer WITHOUT setting kInfamy = -1 so RFAB_XP_Alias.psc
                // awards exact 1x RFAB kill XP here and SyncRfabExperienceRequest shares it with the whole party!
                pActor->Kill(PlayerCharacter::Get());
            }
        }
        return;
    }

    // Propagate physical aggro wakeup and native Requiem/RFAB poise/bash stagger onto the authoritative owner's live AI!
    if (pExtension && pExtension->IsLocal() && !pExtension->IsPlayer() && !pActor->IsDead() && health > 0.0f)
    {
        if (acMessage.SourceServerId != 0 && (acMessage.DeltaHealth < 0.0f || acMessage.StaggerMagnitude > 0.05f))
        {
            if (Actor* pAttacker = Utils::GetByServerId<Actor>(acMessage.SourceServerId);
                pAttacker && !pActor->IsInCombat())
            {
                pActor->StartCombatEx(pAttacker);
            }
        }

        const float acceptedStagger = m_remoteHitImpactGate.TryConsumeStagger(
            acMessage.Id,
            acMessage.StaggerMagnitude,
            pActor->IsGhost() || pActor->IsDead(),
            ClientAvSteadyNowMs());
        if (acceptedStagger > 0.05f && pActor->GetNiNode() && pActor->animationGraphHolder.IsReady())
        {
            pActor->InterruptCast(false);
            BSFixedString staggerMagVar("staggerMagnitude");
            BSFixedString staggerDirVar("staggerDirection");
            BSFixedString staggerStartEvt("staggerStart");
            pActor->animationGraphHolder.SetVariableFloat(&staggerMagVar, acceptedStagger);
            pActor->animationGraphHolder.SetVariableFloat(
                &staggerDirVar,
                std::clamp(std::isfinite(acMessage.StaggerDirection) ? acMessage.StaggerDirection : 0.0f, 0.0f, 0.9999f));
            pActor->SendAnimationEvent(&staggerStartEvt);
        }
    }
}

void ActorValueService::OnActorValueChanges(const NotifyActorValueChanges& acMessage) const noexcept
{
    m_world.GetCharacterService().SyncRemoteCachedActorValues(acMessage.Id, acMessage.Values, /*aIsMaxValues=*/false);

    const entt::entity entity = m_world.GetCharacterService().FindEntityByServerId(acMessage.Id);
    if (entity == entt::null || !m_world.all_of<FormIdComponent>(entity))
        return;

    const bool isLocalOwner = m_world.all_of<LocalComponent>(entity);
    const bool isRemoteActor = m_world.all_of<RemoteComponent>(entity);
    if (!isLocalOwner && !isRemoteActor)
        return;

    auto& formIdComponent = m_world.get<FormIdComponent>(entity);
    Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor || pActor->IsStillLoading() || pActor->IsDeleted())
        return;

    ScopedActorValueOverride avOverride;

    // Authoritative server rollback path (`SaveSnapshotService` / false boss-heal rollback) targeting the
    // owning client of a non-player NPC: apply `kHealth` directly so checkpoint/boss HP rollbacks stick!
    if (isLocalOwner)
    {
        if (const auto* pExt = pActor->GetExtension(); pExt && !pExt->IsPlayer())
        {
            const auto hpIt = acMessage.Values.find(ActorValueInfo::kHealth);
            if (hpIt != acMessage.Values.end())
                pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, hpIt->second);
        }
        return;
    }

    const uint64_t nowMs = ClientAvSteadyNowMs();
    uint64_t lastPredHitMs = 0;
    if (const auto predIt = m_lastLocalPredictedHitMs.find(acMessage.Id); predIt != m_lastLocalPredictedHitMs.end())
        lastPredHitMs = predIt->second;

    for (const auto& [key, value] : acMessage.Values)
    {
        // Syncing dragon souls triggers "Dragon soul collected" event.
        // Never overwrite locally computed movement/attack speed multipliers (SpeedMult = 30, WeaponSpeedMult = 85,
        // LeftWeaponSpeedMult = 132) or kInfamy (37, used by RFAB_XP_Alias.psc as duplicate-XP suppression marker):
        // SetActorValue mutates the base AV, compounding with RFAB.dll SpeedLimitHandler / AdjustmentHandler / StaminaSystemEventSink
        // and poisoning swing speed or kill XP on ownership handoff!
        if (key == ActorValueInfo::kDragonSouls ||
            key == ActorValueInfo::kSpeedMult ||
            key == ActorValueInfo::kInfamy ||
            key == ActorValueInfo::kWeaponSpeedMult ||
            key == ActorValueInfo::kLeftWeaponSpeedMult)
        {
            continue;
        }

        if (key == ActorValueInfo::kHealth)
        {
            const auto* pExt = pActor->GetExtension();
            const bool isRemoteNonPlayer = pExt && pExt->IsRemote() && !pExt->IsPlayer();
            const float localHp = pActor->GetActorValue(ActorValueInfo::kHealth);
            if (CoopDamagePipeline::ShouldReconcileRemoteActorHealthSnapshot(
                    isRemoteNonPlayer,
                    pActor->IsDead(),
                    localHp,
                    value,
                    nowMs,
                    lastPredHitMs))
            {
                const float targetHp = std::clamp(value, 0.0f, 100000.0f);
                pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, targetHp);
            }
            continue;
        }

        spdlog::debug("Actor value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        if (key >= ActorValueInfo::kDamageResist && key <= ActorValueInfo::kMagicResist)
        {
            pActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, key, value);
            continue;
        }
        pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, key, value);
    }
}

void ActorValueService::OnActorMaxValueChanges(const NotifyActorMaxValueChanges& acMessage) const noexcept
{
    m_world.GetCharacterService().SyncRemoteCachedActorValues(acMessage.Id, acMessage.Values, /*aIsMaxValues=*/true);

    const entt::entity entity = m_world.GetCharacterService().FindEntityByServerId(acMessage.Id);
    if (entity == entt::null || !m_world.all_of<FormIdComponent, RemoteComponent>(entity))
        return;

    auto& formIdComponent = m_world.get<FormIdComponent>(entity);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor || pActor->IsStillLoading() || pActor->IsDeleted())
        return;

    ScopedActorValueOverride avOverride;

    for (const auto& [key, value] : acMessage.Values)
    {
        if (key == ActorValueInfo::kDragonSouls ||
            key == ActorValueInfo::kSpeedMult ||
            key == ActorValueInfo::kInfamy ||
            key == ActorValueInfo::kWeaponSpeedMult ||
            key == ActorValueInfo::kLeftWeaponSpeedMult)
        {
            continue;
        }

        spdlog::debug("Actor max value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        pActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, key, value);
    }
}

void ActorValueService::OnDeathStateChange(const NotifyDeathStateChange& acMessage) const noexcept
{
    m_world.GetCharacterService().SyncRemoteCachedDeathState(acMessage.Id, acMessage.IsDead);

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id);
    if (!pActor || pActor->IsStillLoading() || pActor->IsDeleted())
        return;

    ActorExtension* pExtension = pActor->GetExtension();
    // Players should never be killed
    if (!pExtension || pExtension->IsPlayer())
        return;

    // Only apply remote death kills to non-owned actors unless resurrecting an owned actor during checkpoint rollback!
    if (acMessage.IsDead && pExtension->IsLocal())
        return;

    const bool localDead = pActor->IsDead();
    if (localDead != acMessage.IsDead)
    {
        spdlog::info(
            "[RFAB DeathSync] Reconciling actor {:X} (serverId={:X}, localDead={} -> serverDead={})",
            pActor->formID,
            acMessage.Id,
            localDead,
            acMessage.IsDead);

        if (acMessage.IsDead)
        {
            ScopedActorValueOverride avOverride;
            pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, -1.0f);
            pActor->Kill();
        }
        else
        {
            pActor->ResurrectInPlace();
            const entt::entity entity = m_world.GetCharacterService().FindActorEntityByFormId(pActor->formID);
            if (entity != entt::null)
            {
                if (auto* pLocal = m_world.try_get<LocalComponent>(entity))
                    pLocal->IsDead = false;
            }
        }
    }
}
