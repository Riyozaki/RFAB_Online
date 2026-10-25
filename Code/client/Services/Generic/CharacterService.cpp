#include "Forms/TESObjectCELL.h"
#include "Forms/TESWorldSpace.h"
#include "Services/PapyrusService.h"
#include <Services/PartyService.h>

#include <Services/CharacterService.h>
#include <Services/QuestService.h>
#include <Services/RfabAdapterService.h>
#include <Services/TransportService.h>

#include <Games/References.h>
#include <Games/Misc/SubtitleManager.h>

#include <Forms/ActorValueInfo.h>
#include <Forms/TESGlobal.h>
#include <Forms/TESNPC.h>
#include <Forms/TESQuest.h>
#include <Misc/ActorValueOwner.h>

#include <BranchInfo.h>
#include <Components.h>

#include <Systems/InterpolationSystem.h>
#include <Systems/AnimationSystem.h>
#include <Systems/CacheSystem.h>
#include <Systems/FaceGenSystem.h>

#include <Events/ActorAddedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/MountEvent.h>
#include <Events/InitPackageEvent.h>
#include <Events/BeastFormChangeEvent.h>
#include <Events/AddExperienceEvent.h>
#include <Events/DialogueEvent.h>
#include <Events/SubtitleEvent.h>
#include <Events/MoveActorEvent.h>
#include <Events/PartyJoinedEvent.h>

#include <Structs/ActionEvent.h>
#include <Messages/CancelAssignmentRequest.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifySpawnData.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/MountRequest.h>
#include <Messages/NotifyMount.h>
#include <Messages/NewPackageRequest.h>
#include <Messages/NotifyNewPackage.h>
#include <Messages/RequestRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/SyncExperienceRequest.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/SyncRfabExperienceRequest.h>
#include <Messages/NotifySyncRfabExperience.h>
#include <Messages/DialogueRequest.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/SubtitleRequest.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/NotifyRelinquishControl.h>

#include <World.h>
#include <Games/TES.h>
#include <Games/Skyrim/Interface/UI.h>

#include <cmath>

extern thread_local bool g_forceAnimation;

namespace
{
constexpr uint32_t cMaxRfabXpDelta = 1'000'000;

bool IsNetworkCreatedActor(const Actor* apActor) noexcept
{
    if (!apActor || apActor->formID == 0x14)
        return false;

    const auto* pExt = apActor->GetExtension();
    if (pExt && (pExt->IsCreatedByNetwork() || pExt->IsRemotePlayer()))
        return true;

    return ((apActor->formID & 0xFF000000u) == 0xFF000000u) &&
           apActor->baseForm != nullptr &&
           ((apActor->baseForm->formID & 0xFF000000u) == 0xFF000000u);
}

void PurgeDynamicActorCleanly(Actor* apActor) noexcept
{
    if (!apActor || apActor->formID == 0x14)
        return;

    apActor->UnsetChanged(0xFFFFFFFF);
    apActor->SetSkipSaveFlag(true);
    if (apActor->baseForm && (apActor->baseForm->formID & 0xFF000000u) == 0xFF000000u)
        apActor->baseForm->UnsetChanged(0xFFFFFFFF);

    if (auto* pExt = apActor->GetExtension())
    {
        pExt->SetRemote(false);
        pExt->SetDownedPlayer(false);
    }

    // NEVER call synchronous DisableImpl() right before Delete():
    // DisableImpl() immediately detaches MovementControllerNPC's Actor* owner pointer (RCX = 0),
    // while Delete() queues a deferred Disable+Delete task into BSTaskPool (SE 36016) that then
    // crashes at SkyrimSE.exe+0x6EDFC2 (SE 40691, [RCX + 0x1F0] Actor::race) when flushed.
    if ((apActor->formID & 0xFF000000u) == 0xFF000000u)
        apActor->Delete();
}

void SanitizeActorCombatStateForLocalOwnership(Actor* apActor) noexcept
{
    if (!apActor || apActor->formID == 0x14 || apActor->IsDead())
        return;

    // Clear stuck transient ActorState bits copied mid-action while the NPC was Remote:
    // - flags1 bits 28..31 (meleeAttackState) & bits 25..27 (knockState)
    // - flags2 bits 10..11 (recoil) & bit 13 (staggered)
    apActor->actorState.flags1 &= ~(0xF0000000u | 0x0E000000u);
    apActor->actorState.flags2 &= ~(0x00000C00u | 0x00002000u);

    // Normalize stuck intermediate weapon draw/sheathe transition states (1/2 -> 3 Drawn, 4/5 -> 0 Sheathed)
    // so CombatBehaviorMelee/Ranged never waits forever for a WeapEquip_Out event from the prior owner.
    const uint32_t weaponState = (apActor->actorState.flags2 >> 5) & 0x7u;
    if (weaponState == 1u || weaponState == 2u)
        apActor->actorState.flags2 = (apActor->actorState.flags2 & ~(0x7u << 5)) | (3u << 5);
    else if (weaponState == 4u || weaponState == 5u)
        apActor->actorState.flags2 &= ~(0x7u << 5);

    // Repair poisoned near-zero or negative WeaponSpeedMult left over from prior modifier drift.
    for (uint32_t avId : {static_cast<uint32_t>(ActorValueInfo::kWeaponSpeedMult),
                          static_cast<uint32_t>(ActorValueInfo::kLeftWeaponSpeedMult)})
    {
        const float curSpeed = apActor->GetActorValue(avId);
        if (curSpeed < 0.0f || (curSpeed > 0.0f && curSpeed < 0.25f))
        {
            apActor->SetActorValue(avId, 0.0f);
            const float afterBaseReset = apActor->GetActorValue(avId);
            if (afterBaseReset < 0.0f || (afterBaseReset > 0.0f && afterBaseReset < 0.25f))
                apActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, avId, 1.0f);
        }
    }

    // Ensure living NPCs that become locally owned never retain a negative kInfamy (-1.0f) marker
    // from an earlier non-owner hit, so RFAB_XP_Alias::OnActorKilled always awards full kill XP.
    if (apActor->GetActorValue(ActorValueInfo::kInfamy) < 0.0f)
    {
        ScopedActorValueOverride avOverride;
        apActor->SetActorValue(ActorValueInfo::kInfamy, 0.0f);
        apActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, 0.0f);
    }

    if (apActor->animationGraphHolder.IsReady())
    {
        const bool prevForce = g_forceAnimation;
        g_forceAnimation = true;

        bool flagVal = false;
        BSFixedString isAttackingVar("IsAttacking");
        if (apActor->animationGraphHolder.GetVariableBool(&isAttackingVar, &flagVal) && flagVal)
        {
            apActor->animationGraphHolder.SetVariableBool(&isAttackingVar, false);
            BSFixedString attackStopEvt("attackStop");
            apActor->SendAnimationEvent(&attackStopEvt);
        }

        flagVal = false;
        BSFixedString isStaggeringVar("IsStaggering");
        if (apActor->animationGraphHolder.GetVariableBool(&isStaggeringVar, &flagVal) && flagVal)
        {
            apActor->animationGraphHolder.SetVariableBool(&isStaggeringVar, false);
            BSFixedString staggerStopEvt("staggerStop");
            apActor->SendAnimationEvent(&staggerStopEvt);
        }

        flagVal = false;
        BSFixedString isRecoilingVar("IsRecoiling");
        if (apActor->animationGraphHolder.GetVariableBool(&isRecoilingVar, &flagVal) && flagVal)
        {
            apActor->animationGraphHolder.SetVariableBool(&isRecoilingVar, false);
            BSFixedString recoilStopEvt("recoilStop");
            apActor->SendAnimationEvent(&recoilStopEvt);
        }

        g_forceAnimation = prevForce;
    }
}

void KillActorSuppressingDuplicateXp(Actor* apActor) noexcept
{
    if (!apActor)
        return;

    apActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, -1.0f);
    apActor->Kill();
}

void AssignRemoteSummonOwner(World& aWorld, Actor* apSummon, uint32_t aOwnerPlayerId) noexcept
{
    if (!apSummon)
        return;

    apSummon->SetIgnoreFriendlyHit(true);
    if (auto* pExt = apSummon->GetExtension())
        pExt->SetPlayerSummonFlag(true);

    if (aOwnerPlayerId != 0)
    {
        auto playerView = aWorld.view<FormIdComponent, PlayerComponent>();
        for (const auto playerEntity : playerView)
        {
            if (playerView.get<PlayerComponent>(playerEntity).Id != aOwnerPlayerId)
                continue;

            const uint32_t ownerFormId = playerView.get<FormIdComponent>(playerEntity).Id;
            if (ownerFormId != 0 && ownerFormId != 0x14)
            {
                if (Actor* pRemoteOwner = Cast<Actor>(TESForm::GetById(ownerFormId)))
                {
                    apSummon->SetCommandingActor(pRemoteOwner->GetHandle());
                    return;
                }
            }
        }
    }
}

TESGlobal* GetRfabXpGlobal() noexcept
{
    return World::Get().GetRfabAdapterService().GetRfabXpGlobal();
}

int32_t ReadRfabXp(TESGlobal* apGlobal) noexcept
{
    return RfabAdapterService::ReadRfabXp(apGlobal);
}

bool ApplyRfabExperience(TESGlobal* apRfabXp, int32_t aDelta, int32_t& aNewExperience) noexcept
{
    (void)apRfabXp;
    return World::Get().GetRfabAdapterService().ApplyDualPoolExperienceDelta(aDelta, aNewExperience);
}
} // namespace

CharacterService::CharacterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_referenceAddedConnection = m_dispatcher.sink<ActorAddedEvent>().connect<&CharacterService::OnActorAdded>(this);
    m_referenceRemovedConnection = m_dispatcher.sink<ActorRemovedEvent>().connect<&CharacterService::OnActorRemoved>(this);

    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&CharacterService::OnUpdate>(this);
    m_actionConnection = m_dispatcher.sink<ActionEvent>().connect<&CharacterService::OnActionEvent>(this);

    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&CharacterService::OnConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&CharacterService::OnDisconnected>(this);

    m_assignCharacterConnection = m_dispatcher.sink<AssignCharacterResponse>().connect<&CharacterService::OnAssignCharacter>(this);
    m_characterSpawnConnection = m_dispatcher.sink<CharacterSpawnRequest>().connect<&CharacterService::OnCharacterSpawn>(this);
    m_referenceMovementSnapshotConnection = m_dispatcher.sink<ServerReferencesMoveRequest>().connect<&CharacterService::OnReferencesMoveRequest>(this);
    m_factionsConnection = m_dispatcher.sink<NotifyFactionsChanges>().connect<&CharacterService::OnFactionsChanges>(this);
    m_ownershipTransferConnection = m_dispatcher.sink<NotifyOwnershipTransfer>().connect<&CharacterService::OnOwnershipTransfer>(this);
    m_removeCharacterConnection = m_dispatcher.sink<NotifyRemoveCharacter>().connect<&CharacterService::OnRemoveCharacter>(this);
    m_remoteSpawnDataReceivedConnection = m_dispatcher.sink<NotifySpawnData>().connect<&CharacterService::OnRemoteSpawnDataReceived>(this);

    m_mountConnection = m_dispatcher.sink<MountEvent>().connect<&CharacterService::OnMountEvent>(this);
    m_notifyMountConnection = m_dispatcher.sink<NotifyMount>().connect<&CharacterService::OnNotifyMount>(this);

    m_initPackageConnection = m_dispatcher.sink<InitPackageEvent>().connect<&CharacterService::OnInitPackageEvent>(this);
    m_newPackageConnection = m_dispatcher.sink<NotifyNewPackage>().connect<&CharacterService::OnNotifyNewPackage>(this);

    m_notifyRespawnConnection = m_dispatcher.sink<NotifyRespawn>().connect<&CharacterService::OnNotifyRespawn>(this);
    m_beastFormChangeConnection = m_dispatcher.sink<BeastFormChangeEvent>().connect<&CharacterService::OnBeastFormChange>(this);

    m_addExperienceEventConnection = m_dispatcher.sink<AddExperienceEvent>().connect<&CharacterService::OnAddExperienceEvent>(this);
    m_syncExperienceConnection = m_dispatcher.sink<NotifySyncExperience>().connect<&CharacterService::OnNotifySyncExperience>(this);
    m_syncRfabExperienceConnection = m_dispatcher.sink<NotifySyncRfabExperience>().connect<&CharacterService::OnNotifySyncRfabExperience>(this);

    m_dialogueEventConnection = m_dispatcher.sink<DialogueEvent>().connect<&CharacterService::OnDialogueEvent>(this);
    m_dialogueSyncConnection = m_dispatcher.sink<NotifyDialogue>().connect<&CharacterService::OnNotifyDialogue>(this);

    m_subtitleEventConnection = m_dispatcher.sink<SubtitleEvent>().connect<&CharacterService::OnSubtitleEvent>(this);
    m_subtitleSyncConnection = m_dispatcher.sink<NotifySubtitle>().connect<&CharacterService::OnNotifySubtitle>(this);

    m_actorTeleportConnection = m_dispatcher.sink<NotifyActorTeleport>().connect<&CharacterService::OnNotifyActorTeleport>(this);

    m_relinquishConnection = m_dispatcher.sink<NotifyRelinquishControl>().connect<&CharacterService::OnNotifyRelinquishControl>(this);

    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&CharacterService::OnPartyJoinedEvent>(this);
}

void CharacterService::DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept
{
    m_world.remove<FaceGenComponent, InterpolationComponent, RemoteAnimationComponent, RemoteComponent, CacheComponent, WaitingFor3D, PlayerComponent>(aEntity);
    m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
}

bool CharacterService::TakeOwnership(const uint32_t acFormId, const uint32_t acServerId, const entt::entity acEntity, uint64_t aOwnershipEpoch) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(acFormId));
    if (!pActor)
    {
        spdlog::error("Cannot find actor to take control over, form id: {:X}, server id: {:X}", acFormId, acServerId);
        return false;
    }

    ActorExtension* pExtension = pActor->GetExtension();
    if (pExtension->IsRemotePlayer())
    {
        spdlog::error("Cannot take control over remote player actor, form id: {:X}, server id: {:X}", acFormId, acServerId);
        return false;
    }

    if (pActor->IsPlayerSummon())
    {
        spdlog::error("Cannot take control over remote player summon, form id: {:X}, server id: {:X}", acFormId, acServerId);
        return false;
    }

    if (aOwnershipEpoch != 0 && !m_ownershipEpochTracker.AcceptGrant(acServerId, aOwnershipEpoch))
    {
        spdlog::debug("Ignored stale TakeOwnership grant for {:X} (epoch {})", acServerId, aOwnershipEpoch);
        return false;
    }

    pExtension->SetRemote(false);
    SanitizeActorCombatStateForLocalOwnership(pActor);
    pExtension->SetNakedDeadline();

    const uint64_t effectiveEpoch = aOwnershipEpoch != 0 ? aOwnershipEpoch : m_ownershipEpochTracker.GetEpoch(acServerId);
    m_cachedRemoteSpawns.erase(acServerId);
    m_world.emplace_or_replace<LocalComponent>(acEntity, acServerId, effectiveEpoch);
    m_world.emplace_or_replace<LocalAnimationComponent>(acEntity);
    DeleteRemoteEntityComponents(acEntity);

    RequestOwnershipClaim request;
    request.ServerId = acServerId;
    request.NewActorData = BuildActorData(pActor);

    m_transport.Send(request);

    return true;
}

void CharacterService::DeleteTempActor(const uint32_t aFormId) noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (!pActor || aFormId == 0x14)
        return;

    if (IsNetworkCreatedActor(pActor))
    {
        PurgeDynamicActorCleanly(pActor);
        spdlog::info("\tDeleted network actor {:X}", aFormId);
    }
    else if (auto* pExt = pActor->GetExtension())
    {
        pExt->SetRemote(false);
        pExt->SetDownedPlayer(false);
        SanitizeActorCombatStateForLocalOwnership(pActor);
    }
}

void CharacterService::OnActorAdded(const ActorAddedEvent& acEvent) noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId));

    if (acEvent.FormId == 0x14 && pActor)
    {
        pActor->GetExtension()->SetPlayer(true);
    }
    else if (pActor && (acEvent.FormId & 0xFF000000) != 0xFF000000)
    {
        m_world.GetActorValueService().CaptureNativeBaseMaxHealth(pActor);
    }

    entt::entity entity = entt::null;

    const auto view = m_world.view<RemoteComponent>();
    const auto it = std::find_if(
        std::begin(view), std::end(view),
        [&acEvent, view](entt::entity candidate)
        {
            auto& remoteComponent = view.get<RemoteComponent>(candidate);
            return remoteComponent.CachedRefId == acEvent.FormId;
        });

    if (it != std::end(view))
    {
        if (pActor)
            pActor->GetExtension()->SetRemote(true);

        entity = *it;
    }
    else
    {
        const auto existingFormView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        const auto existingIt = std::find_if(
            std::begin(existingFormView), std::end(existingFormView),
            [&acEvent, existingFormView](entt::entity candidate)
            {
                return existingFormView.get<FormIdComponent>(candidate).Id == acEvent.FormId;
            });

        entity = (existingIt != std::end(existingFormView)) ? *existingIt : m_world.create();
    }

    m_world.emplace_or_replace<FormIdComponent>(entity, acEvent.FormId);
    m_world.emplace_or_replace<EarlyAnimationBufferComponent>(entity);

    // Defer server assignment by 2 frames for newly spawned LOCAL temporary 0xFFxxxxxx actors so
    // SummonCreatureEffect / ReanimateEffect / RFAB Papyrus summon scripts finish setting commandingActor,
    // IsPlayerTeammate, resurrected state, and custom stats before RequestServerAssignment snapshots them!
    if (acEvent.FormId != 0x14 &&
        (acEvent.FormId & 0xFF000000) == 0xFF000000 &&
        pActor &&
        !pActor->GetExtension()->IsRemote() &&
        !m_world.all_of<RemoteComponent>(entity))
    {
        m_pendingLocalTempSpawns[entity] = 2;
        return;
    }

    ProcessNewEntity(entity);
}

void CharacterService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    auto view = m_world.view<FormIdComponent>();
    const auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.FormId](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == formId; });

    if (entityIt == view.end())
    {
        if ((acEvent.FormId & 0xFF000000) == 0xFF000000)
            spdlog::debug("Dynamic actor {:X} already removed from form ids map", acEvent.FormId);
        else
            spdlog::error("Actor to remove not found in form ids map {:X}", acEvent.FormId);
        return;
    }

    const auto cId = *entityIt;
    m_pendingLocalTempSpawns.erase(cId);

    const uint32_t removedFormId = view.get<FormIdComponent>(cId).Id;
    CancelServerAssignment(cId, removedFormId);

    m_world.remove<EarlyAnimationBufferComponent>(cId);

    if (m_world.all_of<FormIdComponent>(cId))
        m_world.remove<FormIdComponent>(cId);

    if (m_world.orphan(cId))
        m_world.destroy(cId);

    spdlog::info("Actor removed, form id: {:X}", acEvent.FormId);
}

void CharacterService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    if (!m_pendingLocalTempSpawns.empty())
    {
        Vector<entt::entity> readyEntities;
        Vector<entt::entity> staleEntities;
        for (auto it = m_pendingLocalTempSpawns.begin(); it != m_pendingLocalTempSpawns.end(); ++it)
        {
            const entt::entity pendingEntity = it->first;
            if (!m_world.valid(pendingEntity) || !m_world.all_of<FormIdComponent>(pendingEntity))
            {
                staleEntities.push_back(pendingEntity);
                continue;
            }

            if (it.value() > 1)
            {
                --it.value();
                continue;
            }

            readyEntities.push_back(pendingEntity);
            staleEntities.push_back(pendingEntity);
        }

        for (const entt::entity e : staleEntities)
            m_pendingLocalTempSpawns.erase(e);

        for (const entt::entity e : readyEntities)
        {
            if (!m_world.valid(e) || !m_world.all_of<FormIdComponent>(e))
                continue;
            const uint32_t formId = m_world.get<FormIdComponent>(e).Id;
            Actor* pPendingActor = Cast<Actor>(TESForm::GetById(formId));
            if (!pPendingActor || pPendingActor->IsDisabled())
                continue;
            ProcessNewEntity(e);
        }
    }

    RunSpawnUpdates();
    RunLocalUpdates();
    RunFactionsUpdates();
    if (!m_savePurgeActive && m_pendingPostSaveRemoteRestore)
        RestorePurgedRemoteActorsAfterSave();

    RunRemoteUpdates();
    RunExperienceUpdates();
    RunRfabExperienceUpdates();
    ApplyCachedWeaponDraws(acUpdateEvent);
}

void CharacterService::OnConnected(const ConnectedEvent& acConnectedEvent) noexcept
{
    m_savePurgeActive = false;
    m_engineWorldLoadInProgress = false;
    m_pendingPostSaveRemoteRestore = false;
    m_pendingLocalTempSpawns.clear();
    m_cachedRemoteSpawns.clear();
    NotifySaveLoadReset();

    // Go through all the forms that were previously detected.
    // Delete ONLY genuine network-created clones (never native TESLevCharacter 0xFFxxxxxx dungeon enemies!).
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    Vector<entt::entity> entities(view.begin(), view.end());

    for (auto entity : entities)
    {
        if (!m_world.valid(entity))
            continue;

        const uint32_t formId = m_world.get<FormIdComponent>(entity).Id;
        Actor* pActor = Cast<Actor>(TESForm::GetById(formId));
        if (!pActor)
        {
            m_world.destroy(entity);
            continue;
        }

        if (IsNetworkCreatedActor(pActor))
        {
            PurgeDynamicActorCleanly(pActor);
            m_world.destroy(entity);
            continue;
        }

        if (auto* pExt = pActor->GetExtension())
        {
            pExt->SetRemote(false);
            pExt->SetDownedPlayer(false);
        }

        ProcessNewEntity(entity);
    }
}

void CharacterService::OnDisconnected(const DisconnectedEvent& acDisconnectedEvent) noexcept
{
    if (m_engineWorldLoadInProgress)
        return;

    PurgeNetworkActorsOnLiveDisconnect();
}

void CharacterService::BeginEngineWorldLoad() noexcept
{
    m_engineWorldLoadInProgress = true;
    DetachAllActorsForEngineLoad();
}

void CharacterService::EndEngineWorldLoad() noexcept
{
    m_engineWorldLoadInProgress = false;
}

void CharacterService::PreSavePurgeDynamicActors() noexcept
{
    m_savePurgeActive = true;
    m_pendingPostSaveRemoteRestore = false;

    // Non-destructively mark all dynamic 0xFFxxxxxx multiplayer actors/references so Skyrim SE's
    // TESObjectREFR::CheckSaveGame (unk10 = 0xFFFF) and BGSSaveLoadGame::saveLoadChanges
    // (UnsetChanged(0xFFFFFFFF)) completely omit them from .ess WITHOUT deleting live remote players
    // or respawning 0xFFxxxxxx TESNPC/Actor forms off-thread after save.
    auto allForms = m_world.view<FormIdComponent>();
    for (auto entity : allForms)
    {
        const uint32_t formId = allForms.get<FormIdComponent>(entity).Id;
        if (formId == 0x14 || (formId & 0xFF000000) != 0xFF000000)
            continue;

        TESForm* pForm = TESForm::GetById(formId);
        if (!pForm)
            continue;

        if (Actor* pActor = Cast<Actor>(pForm))
        {
            if (IsNetworkCreatedActor(pActor) || pActor->GetExtension()->IsRemote() || m_world.all_of<RemoteComponent>(entity))
            {
                pActor->SetSkipSaveFlag(true);
                pActor->UnsetChanged(0xFFFFFFFF);
                if (pActor->baseForm && ((pActor->baseForm->formID & 0xFF000000) == 0xFF000000))
                    pActor->baseForm->UnsetChanged(0xFFFFFFFF);
            }
        }
        else if (m_world.all_of<ObjectComponent>(entity))
        {
            if (TESObjectREFR* pRef = Cast<TESObjectREFR>(pForm))
            {
                pRef->SetSkipSaveFlag(true);
                pRef->UnsetChanged(0xFFFFFFFF);
            }
        }
    }
}

void CharacterService::SchedulePostSaveRemoteRestore() noexcept
{
    m_savePurgeActive = false;
    m_pendingPostSaveRemoteRestore = false;
}

void CharacterService::RestorePurgedRemoteActorsAfterSave() noexcept
{
    m_pendingPostSaveRemoteRestore = false;
}

void CharacterService::DetachAllActorsForEngineLoad() noexcept
{
    // CRITICAL FOR SKYRIM SE 1.5.97 SAVE-LOAD STABILITY (+0x6EDFC2):
    // BGSSaveLoadGame::LoadGame natively tears down all loaded cells and destroys every 0xFFxxxxxx form.
    // NEVER invoke DisableImpl() or Delete() here: queueing BSTaskPool Disable/Delete tasks right before
    // LoadGame causes ~MovementControllerNPC to dereference a detached Actor* (RCX = 0, [RCX + 0x1F0]).
    m_savePurgeActive = false;
    m_pendingPostSaveRemoteRestore = false;
    m_pendingLocalTempSpawns.clear();
    m_cachedRemoteSpawns.clear();
    m_weaponDrawUpdates.clear();
    NotifySaveLoadReset();

    auto actorView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    Vector<entt::entity> actorEntities(actorView.begin(), actorView.end());
    for (entt::entity entity : actorEntities)
    {
        const uint32_t formId = actorView.get<FormIdComponent>(entity).Id;
        if (Actor* pActor = Cast<Actor>(TESForm::GetById(formId)))
        {
            if (auto* pExt = pActor->GetExtension())
            {
                pExt->SetRemote(false);
                pExt->SetDownedPlayer(false);
                if (formId != 0x14)
                    pExt->SetPlayer(false);
            }

            if (IsNetworkCreatedActor(pActor))
            {
                pActor->SetSkipSaveFlag(true);
                pActor->UnsetChanged(0xFFFFFFFF);
                if (pActor->baseForm && (pActor->baseForm->formID & 0xFF000000u) == 0xFF000000u)
                    pActor->baseForm->UnsetChanged(0xFFFFFFFF);
            }
        }

        if (m_world.valid(entity))
            m_world.destroy(entity);
    }

    m_world.clear<
        WaitingFor3D,
        WaitingForAssignmentComponent,
        EarlyAnimationBufferComponent,
        CacheComponent,
        FaceGenComponent,
        PlayerComponent,
        LocalComponent,
        LocalAnimationComponent,
        RemoteComponent,
        InterpolationComponent,
        RemoteAnimationComponent>();

    m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
}

void CharacterService::PurgeNetworkActorsOnLiveDisconnect() noexcept
{
    m_savePurgeActive = false;
    m_pendingPostSaveRemoteRestore = false;
    m_pendingLocalTempSpawns.clear();
    m_cachedRemoteSpawns.clear();
    m_weaponDrawUpdates.clear();
    NotifySaveLoadReset();

    auto actorView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    Vector<entt::entity> entitiesToDestroy;
    for (entt::entity entity : actorView)
    {
        const uint32_t formId = actorView.get<FormIdComponent>(entity).Id;
        Actor* pActor = Cast<Actor>(TESForm::GetById(formId));
        if (!pActor)
        {
            entitiesToDestroy.push_back(entity);
            continue;
        }

        if (IsNetworkCreatedActor(pActor))
        {
            PurgeDynamicActorCleanly(pActor);
            entitiesToDestroy.push_back(entity);
        }
        else if (auto* pExt = pActor->GetExtension())
        {
            const bool wasRemote = pExt->IsRemote();
            pExt->SetRemote(false);
            pExt->SetDownedPlayer(false);
            if (wasRemote)
                SanitizeActorCombatStateForLocalOwnership(pActor);
        }
    }

    auto orphanRemoteView = m_world.view<RemoteComponent>(entt::exclude<FormIdComponent>);
    for (entt::entity entity : orphanRemoteView)
        entitiesToDestroy.push_back(entity);

    for (entt::entity entity : entitiesToDestroy)
    {
        if (m_world.valid(entity))
            m_world.destroy(entity);
    }

    m_world.clear<
        WaitingFor3D,
        WaitingForAssignmentComponent,
        EarlyAnimationBufferComponent,
        CacheComponent,
        FaceGenComponent,
        PlayerComponent,
        LocalComponent,
        LocalAnimationComponent,
        RemoteComponent,
        InterpolationComponent,
        RemoteAnimationComponent>();

    m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
}

void CharacterService::PurgeAllDynamicActorsForWorldLoad() noexcept
{
    DetachAllActorsForEngineLoad();
}

void CharacterService::NotifySaveLoadReset() noexcept
{
    m_cachedExperience = 0.f;
    m_rfabExperienceInitialized = false;
    m_localQuestXpGainedInWindow = 0;
    m_lastAppliedRemoteXpAmount = 0;
    m_creditToRefundOnLocalQuestXp = 0;
    m_ownershipEpochTracker.Clear();
}

void CharacterService::NotifyRemoteQuestStageApplied() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    m_remoteQuestXpWindowUntil = now + 4000ms;

    if (m_lastAppliedRemoteXpAmount > 0 && (now - m_lastAppliedRemoteXpTime) <= 3000ms)
    {
        m_creditToRefundOnLocalQuestXp += m_lastAppliedRemoteXpAmount;
        m_lastAppliedRemoteXpAmount = 0;
    }
}

void CharacterService::OnAssignCharacter(const AssignCharacterResponse& acMessage) noexcept
{
    spdlog::info(__FUNCTION__ ": Received for cookie {:X}, server id {:X}", acMessage.Cookie, acMessage.ServerId);

    auto view = m_world.view<WaitingForAssignmentComponent>();
    const auto itor = std::find_if(std::begin(view), std::end(view), [view, cookie = acMessage.Cookie](auto entity) { return view.get<WaitingForAssignmentComponent>(entity).Cookie == cookie; });

    if (itor == std::end(view))
    {
        spdlog::warn("Never found requested cookie: {}", acMessage.Cookie);
        return;
    }

    const auto cEntity = *itor;

    m_world.remove<WaitingForAssignmentComponent>(cEntity);
#if (!IS_MASTER)
    m_world.remove<ReplayedActionsDebugComponent>(cEntity);
#endif

    const auto formIdComponent = m_world.try_get<FormIdComponent>(cEntity);
    if (!formIdComponent)
    {
        spdlog::error(__FUNCTION__ ": form id component doesn't exist, cookie: {:X}", acMessage.Cookie);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent->Id));
    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": actor not found, form id: {:X}", formIdComponent->Id);
        m_world.destroy(cEntity);
        return;
    }

    // TODO: how could this possibly trigger?
    // it's kinda interfering with my WaitingFor3D code
    if (acMessage.PlayerId != 0)
    {
        m_world.emplace_or_replace<PlayerComponent>(cEntity, acMessage.PlayerId);
        m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
    }

    if (acMessage.Owner)
    {
        m_ownershipEpochTracker.AcceptGrant(acMessage.ServerId, acMessage.OwnershipEpoch);
        spdlog::info(__FUNCTION__ ": received local actor, form id: {:X}, {}", pActor->formID, pActor->baseForm ? pActor->baseForm->GetName() : "<null>");

        // When taking ownership of an actor already tracked on the server (e.g. a party leader
        // entering a dungeon room where another party member already damaged, looted, or killed an NPC),
        // apply the server's authoritative state BEFORE constructing LocalComponent so we never
        // heal or resurrect wounded/dead enemies!
        if (acMessage.PlayerId == 0 && !acMessage.AllActorValues.ActorValuesList.empty())
        {
            pActor->SetActorValues(acMessage.AllActorValues);
            pActor->SetActorInventory(acMessage.CurrentInventory);

            if (pActor->IsDead() != acMessage.IsDead)
            {
                if (acMessage.IsDead)
                    KillActorSuppressingDuplicateXp(pActor);
                else
                    spdlog::info("Keeping local actor {:X} dead instead of Respawn() to prevent leveled re-roll duplication", pActor->formID);
            }

            m_weaponDrawUpdates[pActor->formID] = {acMessage.IsWeaponDrawn};
            MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);
        }

        m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId, acMessage.OwnershipEpoch);
        auto& localAnimationComponent = m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);

        pActor->GetExtension()->SetRemote(false);
        SanitizeActorCombatStateForLocalOwnership(pActor);

        if (auto* pEarlyAnimComponent = m_world.try_get<EarlyAnimationBufferComponent>(cEntity))
        {
            for (const auto& action : pEarlyAnimComponent->Actions)
            {
                localAnimationComponent.Append(action);
            }
        }
        m_world.remove<EarlyAnimationBufferComponent>(cEntity);
    }
    else
    {
        m_ownershipEpochTracker.AcceptRelinquish(acMessage.ServerId, acMessage.OwnershipEpoch);
        spdlog::info(__FUNCTION__ ": received remote actor, form id: {:X}, isweapondrawn: {}, {}", pActor->formID, acMessage.IsWeaponDrawn, pActor->baseForm ? pActor->baseForm->GetName() : "<null>");

        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, formIdComponent->Id);

        pActor->GetExtension()->SetRemote(true);

        m_world.remove<EarlyAnimationBufferComponent>(cEntity);
        InterpolationSystem::Setup(m_world, cEntity);
        AnimationSystem::Setup(m_world, cEntity);
        AnimationSystem::AddActionsForReplay(m_world.get<RemoteAnimationComponent>(cEntity), acMessage.ActionsToReplay);

#if (!IS_MASTER)
        m_world.emplace_or_replace<ReplayedActionsDebugComponent>(cEntity, acMessage.ActionsToReplay);
#endif

        if (const auto maxHealthIt = acMessage.AllActorValues.ActorMaxValuesList.find(ActorValueInfo::kHealth);
            maxHealthIt != acMessage.AllActorValues.ActorMaxValuesList.end())
        {
            m_world.GetActorValueService().SeedUnscaledMaxHealthFromRemote(pActor->formID, maxHealthIt->second);
        }

        pActor->SetActorValues(acMessage.AllActorValues);
        pActor->SetActorInventory(acMessage.CurrentInventory);

        if (pActor->IsDead() != acMessage.IsDead)
            acMessage.IsDead ? KillActorSuppressingDuplicateXp(pActor) : pActor->Respawn();

        m_weaponDrawUpdates[pActor->formID] = {acMessage.IsWeaponDrawn};

        MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);
    }
    pActor->GetExtension()->SetNakedDeadline();
}

void CharacterService::OnCharacterSpawn(const CharacterSpawnRequest& acMessage) const noexcept
{
    auto remoteView = m_world.view<RemoteComponent>();
    const auto remoteItor = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ServerId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteItor != std::end(remoteView))
    {
        // If this entity is currently an unloaded out-of-grid WaitingFor3D placeholder (no FormIdComponent / CachedRefId == 0),
        // replace it cleanly so fresh cell-transition / teleport / respawn CharacterSpawnRequest packets materialize immediately!
        if (m_world.all_of<FormIdComponent>(*remoteItor))
        {
            spdlog::debug("Character with remote id {:X} is already spawned.", acMessage.ServerId);
            return;
        }

        m_world.destroy(*remoteItor);
    }

    PlayerCharacter* pLocalPlayer = PlayerCharacter::Get();
    auto stageDeferredRemoteSpawn = [&]()
    {
        m_cachedRemoteSpawns[acMessage.ServerId] = acMessage;
        const entt::entity deferredEntity = m_world.create();
        m_world.emplace_or_replace<RemoteComponent>(deferredEntity, acMessage.ServerId, 0u);
        auto& interp = InterpolationSystem::Setup(m_world, deferredEntity);
        interp.Position = acMessage.Position;
        AnimationSystem::Setup(m_world, deferredEntity);
        AnimationSystem::AddActionsForReplay(m_world.get<RemoteAnimationComponent>(deferredEntity), acMessage.ActionsToReplay);
        m_world.emplace_or_replace<WaitingFor3D>(deferredEntity, acMessage);
    };

    if (m_savePurgeActive || !pLocalPlayer || !pLocalPlayer->parentCell)
    {
        // Local player is mid-save or mid-cell-transition / loading screen (`parentCell == nullptr`):
        // stage remote spawns into WaitingFor3D (CachedRefId = 0, no FormIdComponent) instead of
        // calling Actor::Create / MoveTo(nullptr), so RunSpawnUpdates() materializes them once the cell loads.
        stageDeferredRemoteSpawn();
        return;
    }

    Actor* pActor = nullptr;

    std::optional<entt::entity> entity;

    // Custom forms
    if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        entity = m_world.create();

        if (acMessage.BaseId != GameId{})
        {
            const auto cNpcId = World::Get().GetModSystem().GetGameId(acMessage.BaseId);
            if (cNpcId == 0)
            {
                m_world.destroy(*entity);
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod, base: {:X}:{:X}, form: {:X}:{:X}", acMessage.BaseId.BaseId, acMessage.BaseId.ModId, acMessage.FormId.BaseId, acMessage.FormId.ModId);
                return;
            }

            pNpc = Cast<TESNPC>(TESForm::GetById(cNpcId));
            if (!pNpc)
            {
                m_world.destroy(*entity);
                spdlog::error("Form {:X} is not a TESNPC, skipping remote spawn", cNpcId);
                return;
            }
            if (!acMessage.AppearanceBuffer.empty())
                pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            if ((pNpc->formID & 0xFF000000) != 0xFF000000)
                pNpc->UnsetChanged(0xFFFFFFFF);
        }
        else
        {
            // Players and npcs with temporary ref ids and base ids (usually random events)
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, *entity, acMessage.FaceTints);
        }

        pActor = Actor::Create(pNpc);
        if (pActor && pActor->GetExtension())
            pActor->GetExtension()->SetCreatedByNetwork(true);
    }
    else
    {
        const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);

        auto waitingView = m_world.view<FormIdComponent, WaitingForAssignmentComponent>();
        const auto waitingItor = std::find_if(std::begin(waitingView), std::end(waitingView), [waitingView, cActorId](auto entity) { return waitingView.get<FormIdComponent>(entity).Id == cActorId; });

        if (waitingItor != std::end(waitingView))
        {
            spdlog::info("Character with form id {:X} already has a spawn request in progress.", cActorId);
            return;
        }

        auto* const pForm = TESForm::GetById(cActorId);
        pActor = Cast<Actor>(pForm);

        if (!pActor)
        {
            spdlog::error("Failed to retrieve Actor {:X}, it will not be spawned, possibly missing mod", cActorId);
            spdlog::error("\tForm : {:X}", pForm ? pForm->formID : 0);
            return;
        }

        // Moving an uninitialized reference crashes (e.g. null race); stage until cell ref finishes loading.
        if (pActor->IsStillLoading())
        {
            spdlog::info("Deferring spawn for actor {:X} while cell ref finishes loading, server id: {:X}", cActorId, acMessage.ServerId);
            stageDeferredRemoteSpawn();
            return;
        }

        const auto view = m_world.view<FormIdComponent>();
        const auto itor = std::find_if(std::begin(view), std::end(view), [cActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == cActorId; });

        if (itor != std::end(view))
            entity = *itor;
        else
            entity = m_world.create();
    }

    if (!pActor)
    {
        if (entity.has_value() && m_world.valid(*entity) && !m_world.all_of<FormIdComponent>(*entity))
            m_world.destroy(*entity);
        spdlog::error("Actor object {:X} could not be created.", acMessage.ServerId);
        return;
    }

    spdlog::info("CharacterSpawnRequest, server id: {:X}, form id: {:X}", acMessage.ServerId, pActor->formID);

    if (pActor->IsDisabled())
    {
        spdlog::warn("Disabled actor is being re-enabled: {:X}", pActor->formID);
        pActor->EnableImpl();
    }

    pActor->GetExtension()->SetRemote(true);

    pActor->rotation.x = acMessage.Rotation.x;
    pActor->rotation.z = acMessage.Rotation.y;
    pActor->MoveTo(PlayerCharacter::Get()->parentCell, acMessage.Position);
    pActor->GetExtension()->SetPlayer(acMessage.IsPlayer);

    if (!acMessage.IsPlayer)
    {
        if (const auto maxHealthIt = acMessage.InitialActorValues.ActorMaxValuesList.find(ActorValueInfo::kHealth);
            maxHealthIt != acMessage.InitialActorValues.ActorMaxValuesList.end())
        {
            m_world.GetActorValueService().SeedUnscaledMaxHealthFromRemote(pActor->formID, maxHealthIt->second);
        }
    }

    pActor->SetActorValues(acMessage.InitialActorValues);

    if (acMessage.IsPlayer)
    {
        pActor->SetIgnoreFriendlyHit(true);
        pActor->SetPlayerRespawnMode();
        m_world.emplace_or_replace<PlayerComponent>(*entity, acMessage.PlayerId);
        m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
        if (m_world.GetDownedStateService().IsRemotePlayerDowned(acMessage.PlayerId))
            m_world.GetDownedStateService().ApplyRemoteActorDownedPosture(pActor, true);
    }

    if (pActor->IsDead() != acMessage.IsDead)
    {
        if (acMessage.IsDead)
        {
            pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, -1.0f);
            pActor->Kill();
        }
        else
        {
            pActor->Respawn();
        }
    }

    spdlog::info("Spawn Request Is summon {}", acMessage.IsPlayerSummon);

    if (acMessage.IsPlayerSummon)
    {
        // Bind remote summons to their owning remote player actor (never 0x14, which would consume
        // the local player's Conjuration summon limit!) while preventing friendly-fire aggro.
        AssignRemoteSummonOwner(m_world, pActor, acMessage.PlayerId);
    }

    auto& remoteComponent = m_world.emplace_or_replace<RemoteComponent>(*entity, acMessage.ServerId, pActor->formID);
    m_world.emplace_or_replace<FormIdComponent>(*entity, pActor->formID);
    if (acMessage.IsPlayer)
        m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();

    auto& interpolationComponent = InterpolationSystem::Setup(m_world, *entity);
    interpolationComponent.Position = acMessage.Position;

    AnimationSystem::Setup(m_world, *entity);

    m_cachedRemoteSpawns[acMessage.ServerId] = acMessage;
    m_world.emplace_or_replace<WaitingFor3D>(*entity, acMessage);

    auto& remoteAnimationComponent = m_world.get<RemoteAnimationComponent>(*entity);

    AnimationSystem::AddActionsForReplay(remoteAnimationComponent, acMessage.ActionsToReplay);

#if (!IS_MASTER)
    m_world.emplace_or_replace<ReplayedActionsDebugComponent>(*entity, acMessage.ActionsToReplay);
#endif
}

void CharacterService::OnRemoteSpawnDataReceived(const NotifySpawnData& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);

    const auto itor = std::find_if(
        std::begin(view), std::end(view),
        [view, id = acMessage.Id](auto entity)
        {
            if (auto serverId = Utils::GetServerId(entity))
            {
                if (serverId.has_value() && serverId.value() == id)
                    return true;
            }
            return false;
        });

    if (itor == std::end(view))
        return;

    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(*itor))
    {
        pWaitingFor3D->SpawnRequest.InitialActorValues = acMessage.NewActorData.InitialActorValues;
        pWaitingFor3D->SpawnRequest.InventoryContent = acMessage.NewActorData.InitialInventory;
        pWaitingFor3D->SpawnRequest.IsDead = acMessage.NewActorData.IsDead;
        pWaitingFor3D->SpawnRequest.IsWeaponDrawn = acMessage.NewActorData.IsWeaponDrawn;
    }

    auto& formIdComponent = view.get<FormIdComponent>(*itor);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    pActor->SetActorValues(acMessage.NewActorData.InitialActorValues);
    pActor->SetActorInventory(acMessage.NewActorData.InitialInventory);
    m_weaponDrawUpdates[pActor->formID] = {acMessage.NewActorData.IsWeaponDrawn};

    if (pActor->IsDead() != acMessage.NewActorData.IsDead)
        acMessage.NewActorData.IsDead ? KillActorSuppressingDuplicateXp(pActor) : pActor->Respawn();

    spdlog::info("Applied remote spawn data, actor form id: {:X}", pActor->formID);
}

void CharacterService::OnReferencesMoveRequest(const ServerReferencesMoveRequest& acMessage) const noexcept
{
    auto view = m_world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent>();

    for (const auto& [serverId, update] : acMessage.Updates)
    {
        auto itor = std::find_if(std::begin(view), std::end(view), [serverId = serverId, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == serverId; });

        if (itor == std::end(view))
            continue;

        auto& interpolationComponent = view.get<InterpolationComponent>(*itor);
        auto& animationComponent = view.get<RemoteAnimationComponent>(*itor);
        const auto& movement = update.UpdatedMovement;

        InterpolationComponent::TimePoint point;
        point.Tick = acMessage.Tick;
        point.Position = movement.Position;
        point.Rotation = {movement.Rotation.x, 0.f, movement.Rotation.y};
        point.Variables = movement.Variables;
        point.Direction = movement.Direction;

        InterpolationSystem::AddPoint(interpolationComponent, point);

        for (const auto& action : update.ActionEvents)
        {
            animationComponent.TimePoints.push_back(action);
        }
    }
}

void CharacterService::OnActionEvent(const ActionEvent& acActionEvent) const noexcept
{
    auto view = m_world.view<LocalAnimationComponent, FormIdComponent>();
    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acActionEvent.ActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (itor != std::end(view))
    {
        auto& localComponent = view.get<LocalAnimationComponent>(*itor);

        localComponent.Append(acActionEvent);
    }
    else if (m_transport.IsOnline())
    {
        // A `LocalAnimationComponent` is not attached yet, but the actor already exists and is running animations

        auto view = m_world.view<FormIdComponent, EarlyAnimationBufferComponent>();
        const auto itor = std::find_if(std::begin(view), std::end(view), [id = acActionEvent.ActorId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

        if (itor != std::end(view))
        {
            view.get<EarlyAnimationBufferComponent>(*itor).Actions.push_back(acActionEvent);
        }
    }
}

void CharacterService::OnFactionsChanges(const NotifyFactionsChanges& acEvent) const noexcept
{
    auto view = m_world.view<RemoteComponent, FormIdComponent, CacheComponent>();

    for (const auto& [id, factions] : acEvent.Changes)
    {
        const auto itor = std::find_if(std::begin(view), std::end(view), [id = id, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

        if (itor != std::end(view))
        {
            auto& formIdComponent = view.get<FormIdComponent>(*itor);

            auto* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (!pActor)
                continue;

            auto& cacheComponent = view.get<CacheComponent>(*itor);
            cacheComponent.FactionsContent = factions;

            pActor->SetFactions(cacheComponent.FactionsContent);
        }
    }
}

void CharacterService::OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) const noexcept
{
    if (!m_ownershipEpochTracker.AcceptGrant(acMessage.ServerId, acMessage.OwnershipEpoch))
    {
        spdlog::debug("Ignored stale NotifyOwnershipTransfer for {:X} (epoch {})", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    // TODO(cosideci): handle case if no one has it, therefore no RemoteComponent
    auto view = m_world.view<RemoteComponent, FormIdComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [&acMessage, &view](auto entity) { return view.get<RemoteComponent>(entity).Id == acMessage.ServerId; });

    if (itor != std::end(view))
    {
        auto& formIdComponent = view.get<FormIdComponent>(*itor);

        if (TakeOwnership(formIdComponent.Id, acMessage.ServerId, *itor, acMessage.OwnershipEpoch))
        {
            spdlog::info("Ownership claimed {:X} (epoch {})", acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }
    }

    spdlog::warn("Actor for ownership transfer not found {:X}", acMessage.ServerId);

    RequestOwnershipTransfer request{};
    request.ServerId = acMessage.ServerId;
    request.OwnershipEpoch = acMessage.OwnershipEpoch;

    m_transport.Send(request);
}

void CharacterService::OnRemoveCharacter(const NotifyRemoveCharacter& acMessage) const noexcept
{
    m_ownershipEpochTracker.Forget(acMessage.ServerId);
    m_cachedRemoteSpawns.erase(acMessage.ServerId);
    auto view = m_world.view<RemoteComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acMessage.ServerId, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

    if (itor != std::end(view))
    {
        const entt::entity entity = *itor;
        bool removedNetworkActor = false;
        if (auto* pFormIdComponent = m_world.try_get<FormIdComponent>(entity))
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));
            removedNetworkActor = IsNetworkCreatedActor(pActor);
            CharacterService::DeleteTempActor(pFormIdComponent->Id);
        }
        else if (const uint32_t cachedRefId = view.get<RemoteComponent>(entity).CachedRefId; cachedRefId != 0)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(cachedRefId));
            removedNetworkActor = IsNetworkCreatedActor(pActor);
            CharacterService::DeleteTempActor(cachedRefId);
        }

        DeleteRemoteEntityComponents(entity);
        if (removedNetworkActor)
        {
            m_world.remove<FormIdComponent, EarlyAnimationBufferComponent>(entity);
            if (m_world.orphan(entity))
                m_world.destroy(entity);
        }
    }
}

void CharacterService::OnNotifyRespawn(const NotifyRespawn& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();
    const auto entityIt = std::find_if(view.begin(), view.end(), [view, id = acMessage.ActorId](auto aEntity) { return view.get<RemoteComponent>(aEntity).Id == id; });

    if (entityIt == view.end())
    {
        spdlog::error("Actor to respawn not found in: {:X}", acMessage.ActorId);
        return;
    }

    const auto cId = *entityIt;
    m_cachedRemoteSpawns.erase(acMessage.ActorId);

    const uint32_t respawnFormId = view.get<FormIdComponent>(cId).Id;
    CancelServerAssignment(cId, respawnFormId);

    m_world.remove<EarlyAnimationBufferComponent>(cId);

    if (m_world.all_of<FormIdComponent>(cId))
        m_world.remove<FormIdComponent>(cId);

    if (m_world.orphan(cId))
        m_world.destroy(cId);

    RequestRespawn request;
    request.ActorId = acMessage.ActorId;

    m_transport.Send(request);
}

void CharacterService::OnBeastFormChange(const BeastFormChangeEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });
    if (it == view.end())
        return;

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*it);
    if (!serverIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    uint32_t serverId = serverIdRes.value();

    RequestRespawn request;
    request.ActorId = serverId;

    Actor* pActor = Utils::GetByServerId<Actor>(serverId);
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor for server id {:X}", serverId);
        return;
    }

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor baseform for server id {:X}", serverId);
        return;
    }

    pNpc->Serialize(&request.AppearanceBuffer);
    request.ChangeFlags = pNpc->GetChangeFlags();

    m_transport.Send(request);
}

void CharacterService::OnMountEvent(const MountEvent& acEvent) const noexcept
{
    auto view = m_world.view<FormIdComponent>();

    const auto riderIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.RiderID, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (riderIt == std::end(view))
    {
        spdlog::warn("Rider not found, form id: {:X}", acEvent.RiderID);
        return;
    }

    const entt::entity cRiderEntity = *riderIt;

    std::optional<uint32_t> riderServerIdRes = Utils::GetServerId(cRiderEntity);
    if (!riderServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    const auto mountIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.MountID, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (mountIt == std::end(view))
    {
        spdlog::warn("Mount not found, form id: {:X}", acEvent.MountID);
        return;
    }

    const entt::entity cMountEntity = *mountIt;

    std::optional<uint32_t> mountServerIdRes = Utils::GetServerId(cMountEntity);
    if (!mountServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    if (m_world.try_get<RemoteComponent>(cMountEntity))
        TakeOwnership(acEvent.MountID, *mountServerIdRes, cMountEntity);

    MountRequest request;
    request.MountId = mountServerIdRes.value();
    request.RiderId = riderServerIdRes.value();

    m_transport.Send(request);
}

void CharacterService::OnNotifyMount(const NotifyMount& acMessage) const noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();

    const auto riderIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.RiderId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (riderIt == std::end(remoteView))
    {
        spdlog::warn("Rider with remote id {:X} not found.", acMessage.RiderId);
        return;
    }

    auto& riderFormIdComponent = remoteView.get<FormIdComponent>(*riderIt);
    TESForm* pRiderForm = TESForm::GetById(riderFormIdComponent.Id);
    Actor* pRider = Cast<Actor>(pRiderForm);

    Actor* pMount = nullptr;

    auto formView = m_world.view<FormIdComponent>();
    Vector<entt::entity> entities(formView.begin(), formView.end());

    // TODO(cosideci): remove this, cause of NotifyRelinquishControl?
    for (auto entity : entities)
    {
        std::optional<uint32_t> serverIdRes = Utils::GetServerId(entity);
        if (!serverIdRes.has_value())
        {
            spdlog::error("{}: failed to find server id", __FUNCTION__);
            continue;
        }

        uint32_t serverId = serverIdRes.value();

        if (serverId == acMessage.MountId)
        {
            auto& mountFormIdComponent = m_world.get<FormIdComponent>(entity);

            if (m_world.all_of<LocalComponent>(entity))
            {
                m_world.remove<LocalAnimationComponent, LocalComponent>(entity);
                m_world.emplace_or_replace<RemoteComponent>(entity, acMessage.MountId, mountFormIdComponent.Id);
            }

            TESForm* pMountForm = TESForm::GetById(mountFormIdComponent.Id);
            pMount = Cast<Actor>(pMountForm);
            if (pMount && pMount->GetExtension())
                pMount->GetExtension()->SetRemote(true);

            InterpolationSystem::Setup(m_world, entity);
            AnimationSystem::Setup(m_world, entity);

            break;
        }
    }

    if (pRider && pMount)
        pRider->InitiateMountPackage(pMount);
}

void CharacterService::OnInitPackageEvent(const InitPackageEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto actorIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.ActorId, view](auto entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (actorIt == std::end(view))
        return;

    const entt::entity cActorEntity = *actorIt;

    std::optional<uint32_t> actorServerIdRes = Utils::GetServerId(cActorEntity);
    if (!actorServerIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    NewPackageRequest request;
    request.ActorId = actorServerIdRes.value();
    if (!m_world.GetModSystem().GetServerModId(acEvent.PackageId, request.PackageId.ModId, request.PackageId.BaseId))
        return;

    m_transport.Send(request);
}

void CharacterService::OnNotifyNewPackage(const NotifyNewPackage& acMessage) const noexcept
{
    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.ActorId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteIt == std::end(remoteView))
    {
        spdlog::warn("Actor for package with remote id {:X} not found.", acMessage.ActorId);
        return;
    }

    auto formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);

    const TESForm* pForm = TESForm::GetById(formIdComponent.Id);
    Actor* pActor = Cast<Actor>(pForm);

    const uint32_t cPackageFormId = World::Get().GetModSystem().GetGameId(acMessage.PackageId);
    const TESForm* pPackageForm = TESForm::GetById(cPackageFormId);
    if (!pPackageForm)
    {
        spdlog::warn("Actor package not found, base id: {:X}, mod id: {:X}", acMessage.PackageId.BaseId, acMessage.PackageId.ModId);
        return;
    }

    TESPackage* pPackage = Cast<TESPackage>(pPackageForm);
    if (!pActor || !pPackage)
        return;

    pActor->SetPackage(pPackage);
}

void CharacterService::OnAddExperienceEvent(const AddExperienceEvent& acEvent) noexcept
{
    // When drake1ch/RFAB is loaded (RFAB_XP_Amount global present), progression is governed strictly by
    // RFAB's XP pool rather than vanilla skill-use ticks — never leak spurious skill XP packets across party!
    if (GetRfabXpGlobal() != nullptr)
    {
        m_cachedExperience = 0.f;
        return;
    }

    m_cachedExperience += acEvent.Experience;
}

void CharacterService::OnNotifySyncExperience(const NotifySyncExperience& acMessage) noexcept
{
    if (GetRfabXpGlobal() != nullptr)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    if (!pPlayer || PlayerCharacter::LastUsedCombatSkill == -1)
        return;

    pPlayer->AddSkillExperience(PlayerCharacter::LastUsedCombatSkill, acMessage.Experience);
}

void CharacterService::OnNotifySyncRfabExperience(const NotifySyncRfabExperience& acMessage) noexcept
{
    if (acMessage.Experience == 0 || acMessage.Experience > cMaxRfabXpDelta)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESGlobal* pRfabXp = GetRfabXpGlobal();
    if (pPlayer == nullptr || pRfabXp == nullptr)
        return;

    const auto now = std::chrono::steady_clock::now();
    const float localMultiplier = m_world.GetRfabAdapterService().GetLocalXpMultiplier();
    int32_t rawIncoming = static_cast<int32_t>(acMessage.Experience);
    int32_t delta = rawIncoming;
    if (std::fabs(localMultiplier - 1.0f) > 0.01f)
    {
        const int32_t scaled = static_cast<int32_t>(static_cast<float>(rawIncoming) * localMultiplier);
        delta = scaled - (scaled % 5);
        if (delta <= 0 && rawIncoming > 0)
            delta = 5;
    }

    // If a synced quest stage already executed locally on this client, absorb the matching quest XP delta
    // so both players get exact 1x quest XP rather than 2x.
    if (now <= m_remoteQuestXpWindowUntil && m_localQuestXpGainedInWindow > 0)
    {
        const int32_t absorbed = std::min(delta, m_localQuestXpGainedInWindow);
        m_localQuestXpGainedInWindow -= absorbed;
        delta -= absorbed;
        if (delta <= 0)
            return;
    }

    const int32_t currentExperience = ReadRfabXp(pRfabXp);
    int32_t newExperience = currentExperience;

    if (!ApplyRfabExperience(pRfabXp, delta, newExperience))
    {
        spdlog::warn("Unable to apply RFAB XP: Game.SetPlayerExperience is unavailable");
        return;
    }

    if (!m_rfabExperienceInitialized)
    {
        m_lastRfabExperience = currentExperience;
        m_rfabExperienceInitialized = true;
    }

    m_lastRfabExperience += delta;
    m_lastAppliedRemoteXpAmount = delta;
    m_lastAppliedRemoteXpTime = now;

    spdlog::debug("Received RFAB experience {} (total {})", delta, newExperience);
}

void CharacterService::ApplyAuthoritativeRfabXpDelta(int32_t aDelta) noexcept
{
    if (aDelta == 0)
        return;

    TESGlobal* pRfabXp = GetRfabXpGlobal();
    if (pRfabXp == nullptr)
        return;

    const int32_t currentExperience = ReadRfabXp(pRfabXp);
    int32_t newExperience = currentExperience;
    if (!ApplyRfabExperience(pRfabXp, aDelta, newExperience))
        return;

    m_lastRfabExperience = newExperience;
    m_rfabExperienceInitialized = true;
    m_lastAppliedRemoteXpAmount = aDelta;
    m_lastAppliedRemoteXpTime = std::chrono::steady_clock::now();
}

void CharacterService::OnDialogueEvent(const DialogueEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    const bool isLeader = World::Get().GetPartyService().IsLeader(); // Helps distinguish in 2-party logs
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.ActorID](auto entity) {
        return view.get<FormIdComponent>(entity).Id == formId;
    });

    if (entityIt == view.end())
    {
        spdlog::debug(__FUNCTION__ ": failed to find speaking Actor's FormIdComponent, formId {:X}, isLeader {}", acEvent.ActorID, isLeader);
        return;
    }

    auto serverIdRes = Utils::GetServerId(*entityIt);
    if (!serverIdRes)
    {
        spdlog::debug(__FUNCTION__ ": server id not found for formId {:X}, isLeader {}", acEvent.ActorID, isLeader);
        return;
    }
    
    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorID));
    if (!pActor)
        return;

    bool isLocal = pActor->GetExtension()->IsLocal();
    bool isInScene = pActor->IsInScene();
    auto sceneId = (isInScene && pActor->GetCurrentScene()) ? pActor->GetCurrentScene()->formID : 0;
    bool isTaskDialogue = pActor->IsTalking() && pActor->IsInDialogueWithPlayer();
    bool isSpeakingInScene = pActor->IsSpeakingInScene();

    const bool willSync = isTaskDialogue || isLocal && !isInScene;

    spdlog::debug(
        __FUNCTION__ ": isLocal {}, isInScene {}, isSpeakingInScene {}, isTaskDialogue {}, willSync {}, scene {:X}, Actor "
                     "{:X}, serverId {:X}, isLeader {}, name {}, soundFile {}",
        isLocal, isInScene, isSpeakingInScene, isTaskDialogue, willSync, sceneId, pActor->formID, serverIdRes.value(), isLeader, pActor->baseForm ? pActor->baseForm->GetName() : "<null>",
        acEvent.VoiceFile);

    if (willSync)
    {
        DialogueRequest request{};
        request.ServerId = serverIdRes.value();
        request.SoundFilename = acEvent.VoiceFile;

        m_transport.Send(request);
    }
}

void CharacterService::OnNotifyDialogue(const NotifyDialogue& acMessage) noexcept
{
    const bool isLeader = World::Get().GetPartyService().IsLeader(); // Helps distinguish in 2-party logs
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    auto viewIt = std::find_if(
        view.begin(), view.end(),
        [view, id = acMessage.ServerId](auto entity)
        {
            auto serverId = Utils::GetServerId(entity);
            return serverId.has_value() && serverId.value() == id;
        });

    if (viewIt == view.end())
    {
        spdlog::debug(__FUNCTION__ ": failed to find speaking Actor's FormIdComponent, serverId {:X}, isLeader {}", acMessage.ServerId, isLeader);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*viewIt).Id));
    if (!pActor)
        return;

    spdlog::debug(
        __FUNCTION__ ": playing dialogue Actor {:X}, serverId {:X}, isLeader {}, name {}, soundFile {}", pActor->formID, acMessage.ServerId, isLeader, pActor->baseForm ? pActor->baseForm->GetName() : "<null>",
        acMessage.SoundFilename);

    pActor->StopCurrentDialogue(true);
    pActor->SpeakSound(acMessage.SoundFilename.c_str());
}

void CharacterService::OnSubtitleEvent(const SubtitleEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    const bool isLeader = World::Get().GetPartyService().IsLeader(); // Helps distinguish in 2-party logs
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    auto entityIt = std::find_if(view.begin(), view.end(), [view, formId = acEvent.SpeakerID](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (entityIt == view.end())
    {
        spdlog::debug(__FUNCTION__ ": failed to find subtitle Actor's FormIdComponent, formId {:X}, isLeader {}", acEvent.SpeakerID, isLeader);
        return;
    }

    auto serverIdRes = Utils::GetServerId(*entityIt);
    if (!serverIdRes)
    {
        spdlog::debug(__FUNCTION__ ": server id not found for formId {:X}, isLeader {}", acEvent.SpeakerID, isLeader);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.SpeakerID));
    if (!pActor)
        return;

    bool isLocal = pActor->GetExtension()->IsLocal();
    bool isInScene = pActor->IsInScene();
    auto isSpeakingInScene = pActor->IsSpeakingInScene();
    auto sceneId = (isInScene && pActor->GetCurrentScene()) ? pActor->GetCurrentScene()->formID : 0;
    bool isTaskDialogue = pActor->IsTalking() && pActor->IsInDialogueWithPlayer();

    const bool willSync = isTaskDialogue || isLocal && !isInScene;

    spdlog::debug(
        __FUNCTION__ ": isLocal {}, isInScene {}, isSpeakingInScene {}, isTaskDialogue {}, willSync {}, scene {:X}, Actor "
                     "{:X}, serverId {:X}, isLeader {}, name {}, subtitle {}",
        isLocal, isInScene, isSpeakingInScene, isTaskDialogue, willSync, sceneId, pActor->formID, serverIdRes.value(), isLeader, pActor->baseForm ? pActor->baseForm->GetName() : "<null>",
        acEvent.Text);

    if (willSync)
    {
        SubtitleRequest request{};
        request.ServerId = serverIdRes.value();
        request.Text = acEvent.Text;
        request.TopicFormId = acEvent.TopicFormID;
        m_transport.Send(request);
    }
}

void CharacterService::OnNotifySubtitle(const NotifySubtitle& acMessage) noexcept
{
    const bool isLeader = m_world.Get().GetPartyService().IsLeader();
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    auto viewIt = std::find_if(
        view.begin(), view.end(),
        [view, id = acMessage.ServerId](auto entity)
        {
            auto serverId = Utils::GetServerId(entity);
            return serverId.has_value() && serverId.value() == id;
        });

    if (viewIt == view.end())
    {
        spdlog::debug(__FUNCTION__ ": failed to find subtitle Actor's FormIdComponent, serverId {:X}, isLeader {}", acMessage.ServerId, isLeader);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(*viewIt).Id));
    if (!pActor)
        return;

    // This is only for fallout 4
    TESTopicInfo* pInfo = nullptr;
    pInfo = Cast<TESTopicInfo>(TESForm::GetById(acMessage.TopicFormId));

    spdlog::debug(__FUNCTION__ ": showing subtitle Actor {:X}, serverId {:X}, isLeader {}, name {}, message: {}",
                     pActor->formID, acMessage.ServerId, isLeader, pActor->baseForm ? pActor->baseForm->GetName() : "<null>", acMessage.Text);

    SubtitleManager::Get()->HideSubtitle(pActor);   // Subtitle conflicts can hang, this makes it beter at least.
    SubtitleManager::Get()->ShowSubtitle(pActor, acMessage.Text.c_str(), pInfo);
}

void CharacterService::OnNotifyRelinquishControl(const NotifyRelinquishControl& acMessage) noexcept
{
    if (!m_ownershipEpochTracker.AcceptRelinquish(acMessage.ServerId, acMessage.OwnershipEpoch))
    {
        spdlog::debug("Ignored stale NotifyRelinquishControl for {:X} (epoch {})", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    auto formView = m_world.view<FormIdComponent>();
    Vector<entt::entity> entities(formView.begin(), formView.end());

    // TODO(cosideci): this entity iteration shouldn't technically be necessary, just look for the local component
    for (auto entity : entities)
    {
        std::optional<uint32_t> serverIdRes = Utils::GetServerId(entity);
        if (!serverIdRes.has_value())
        {
            spdlog::error(__FUNCTION__ ": failed to find server id for entity");
            continue;
        }

        uint32_t serverId = serverIdRes.value();

        if (serverId == acMessage.ServerId)
        {
            auto& formIdComponent = m_world.get<FormIdComponent>(entity);

            if (m_world.all_of<LocalComponent>(entity))
            {
                m_world.remove<LocalAnimationComponent, LocalComponent>(entity);
                m_world.emplace_or_replace<RemoteComponent>(entity, acMessage.ServerId, formIdComponent.Id);
            }

            Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (!pActor)
            {
                // Probably left the room and/or temporary.
                spdlog::info(__FUNCTION__ ": no local Actor for serverId {:X} to relinquish", serverId);
                continue;
            }

            pActor->GetExtension()->SetRemote(true);

            InterpolationSystem::Setup(m_world, entity);
            AnimationSystem::Setup(m_world, entity);

            spdlog::info(__FUNCTION__ ": relinquished control of actor {:X} with server id {:X}", pActor->formID, acMessage.ServerId);

            return;
        }
    }

    spdlog::error("Did not find actor to relinquish control of, server id {:X}", acMessage.ServerId);
}

void CharacterService::OnNotifyActorTeleport(const NotifyActorTeleport& acMessage) noexcept
{
    auto& modSystem = m_world.GetModSystem();

    const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);
    Actor* pActor = Cast<Actor>(TESForm::GetById(cActorId));
    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": failed to retrieve actor to teleport.");
        return;
    }

    MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);

    spdlog::info("Successfully teleported actor, form id: {:X}, world space: {:X}, cell: {:X}, position: ({}, {}, {})", pActor->formID, acMessage.WorldSpaceId.BaseId, acMessage.CellId.BaseId, acMessage.Position.x, acMessage.Position.y, acMessage.Position.z);
}

void CharacterService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    // Takes ownership of all actors
    if (acEvent.IsLeader)
    {
        auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        Vector<entt::entity> entities(view.begin(), view.end());

        for (auto entity : entities)
            ProcessNewEntity(entity);
    }
}

void CharacterService::ClaimCombatActorsFromDownedPeer(uint32_t aDownedPeerPlayerId) noexcept
{
    if (!m_transport.IsOnline())
        return;

    if (m_world.GetDownedStateService().IsLocalPlayerDowned())
        return;

    const auto& partyService = m_world.GetPartyService();
    if (!partyService.IsInParty() || !partyService.IsPlayerInParty(aDownedPeerPlayerId))
        return;

    PlayerCharacter* pLocalPlayer = PlayerCharacter::Get();
    if (!pLocalPlayer || !pLocalPlayer->parentCell || pLocalPlayer->VirtIsDead(false))
        return;

    auto view = m_world.view<RemoteComponent, FormIdComponent>(entt::exclude<ObjectComponent, PlayerComponent>);
    Vector<entt::entity> entities(view.begin(), view.end());

    for (auto entity : entities)
    {
        if (!m_world.valid(entity))
            continue;

        const auto* pRemoteComp = m_world.try_get<RemoteComponent>(entity);
        const auto* pFormIdComp = m_world.try_get<FormIdComponent>(entity);
        if (!pRemoteComp || !pFormIdComp || pFormIdComp->Id == 0x14)
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComp->Id));
        if (!pActor || pActor->IsDead() || pActor->IsTemporary() || pActor->IsMount() || pActor->IsPlayerSummon())
            continue;

        const bool sameCellOrWorld =
            (pActor->parentCell == pLocalPlayer->parentCell) ||
            (pActor->GetWorldSpace() != nullptr && pActor->GetWorldSpace() == pLocalPlayer->GetWorldSpace());
        if (!sameCellOrWorld)
            continue;

        const float dx = pActor->position.x - pLocalPlayer->position.x;
        const float dy = pActor->position.y - pLocalPlayer->position.y;
        const float dz = pActor->position.z - pLocalPlayer->position.z;
        const float distSq = dx * dx + dy * dy + dz * dz;

        const bool isBoss = m_world.GetRfabAdapterService().IsBossActor(pActor);
        if (!isBoss && !pActor->IsInCombat() && distSq > (4096.0f * 4096.0f))
            continue;

        spdlog::info(
            "Standing teammate claiming actor {:X} (serverId {:X}, isBoss={}) after peer {} entered downed state",
            pActor->formID,
            pRemoteComp->Id,
            isBoss,
            aDownedPeerPlayerId);

        TakeOwnership(pActor->formID, pRemoteComp->Id, entity);
    }
}

void CharacterService::MoveActor(const Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) const noexcept
{
    TESObjectCELL* pCell = nullptr;
    if (!acWorldSpaceId)
    {
        const uint32_t cCellId = m_world.GetModSystem().GetGameId(acCellId);
        pCell = Cast<TESObjectCELL>(TESForm::GetById(cCellId));
    }
    // In case of lazy-loading of exterior cells
    else
    {
        const uint32_t cWorldSpaceId = m_world.GetModSystem().GetGameId(acWorldSpaceId);
        TESWorldSpace* const pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(cWorldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acPosition);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }
    }

    if (!pCell)
    {
        spdlog::error(__FUNCTION__ ": failed to fetch cell to teleport, actor: {:X}, worldspace: {:X}, cell: {:X}, position: {}, {}, {}", apActor->formID, acWorldSpaceId.BaseId, acCellId.BaseId, acPosition.x, acPosition.y, acPosition.z);
        return;
    }

    apActor->MoveTo(pCell, acPosition);
}

void CharacterService::ProcessNewEntity(entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return;

    auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": actor for new entity not found, form id: {:X}", formIdComponent.Id);
        return;
    }

    if (auto* pRemoteComponent = m_world.try_get<RemoteComponent>(aEntity); pRemoteComponent)
    {
        // Never steal actor ownership onto a downed party leader or from a remote player summon!
        if (m_world.GetPartyService().IsLeader() &&
            !m_world.GetDownedStateService().IsLocalPlayerDowned() &&
            !pActor->IsTemporary() &&
            !pActor->IsMount() &&
            !pActor->IsPlayerSummon())
        {
            spdlog::info("Sending ownership claim for actor {:X} with server id {:X}", pActor->formID, pRemoteComponent->Id);

            TakeOwnership(pActor->formID, pRemoteComponent->Id, aEntity);
        }
        else
            spdlog::info("New entity remotely managed, form id: {:X}, server id: {:X}", pActor->formID, pRemoteComponent->Id);

        return;
    }

    if (m_world.any_of<RemoteComponent, LocalComponent, WaitingForAssignmentComponent>(aEntity))
        return;

    CacheSystem::Setup(World::Get(), aEntity, pActor);

    RequestServerAssignment(aEntity);
}

void CharacterService::RequestServerAssignment(const entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return;

    static uint32_t sCookieSeed = 0;

    const auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
        return;

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
        return;

    AssignCharacterRequest message{};

    message.Cookie = sCookieSeed;

    if (!m_world.GetModSystem().GetServerModId(formIdComponent.Id, message.ReferenceId))
    {
        spdlog::error("Server reference id not found for form id {:X}", formIdComponent.Id);
        return;
    }

    if (!pActor->parentCell || !m_world.GetModSystem().GetServerModId(pActor->parentCell->formID, message.CellId))
    {
        spdlog::error("Server cell id not found for cell id {:X}", pActor->parentCell ? pActor->parentCell->formID : 0u);
        return;
    }

    if (const auto pWorldSpace = pActor->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, message.WorldSpaceId))
            return;
    }

    message.Position = pActor->position;
    message.Rotation.x = pActor->rotation.x;
    message.Rotation.y = pActor->rotation.z;

    // Serialize the base form
    const auto isPlayer = (formIdComponent.Id == 0x14);
    const auto isTemporary = pActor->formID >= 0xFF000000;

    if (isPlayer)
    {
        pNpc->MarkChanged(0x2000800);
    }

    const auto changeFlags = pNpc->GetChangeFlags();

    if (isPlayer || changeFlags != 0)
    {
        message.ChangeFlags = changeFlags;
        pNpc->Serialize(&message.AppearanceBuffer);
    }

    if (isPlayer)
    {
        auto& entries = message.FaceTints.Entries;

        const auto& tints = PlayerCharacter::Get()->GetTints();

        entries.resize(tints.length);

        for (auto i = 0u; i < tints.length; ++i)
        {
            entries[i].Alpha = tints[i]->alpha;
            entries[i].Color = tints[i]->color;
            entries[i].Type = tints[i]->type;

            if (tints[i]->texture)
                entries[i].Name = tints[i]->texture->name.AsAscii();
        }
    }

    if (isPlayer)
    {
        auto& questLog = message.QuestContent.Entries;
        auto& modSystem = m_world.GetModSystem();
        std::unordered_set<uint32_t> seenQuestFormIds{};

        auto tryAppendQuestEntry = [&](TESQuest* apQuest)
        {
            if (!apQuest || apQuest->currentStage == 0)
                return;
            if (!seenQuestFormIds.insert(apQuest->formID).second)
                return;
            if (QuestService::IsNonSyncableQuest(apQuest))
                return;

            GameId id{};
            if (modSystem.GetServerModId(apQuest->formID, id))
            {
                auto& entry = questLog.emplace_back();
                entry.Stage = apQuest->currentStage;
                entry.Id = id;
            }
        };

        for (const auto& objective : PlayerCharacter::Get()->objectives)
        {
            if (!objective.instance)
                continue;
            tryAppendQuestEntry(objective.instance->quest);
        }

        if (auto* pModManager = ModManager::Get())
        {
            for (TESQuest* pQuest : pModManager->quests)
            {
                if (!pQuest || !pQuest->IsEnabled() || pQuest->IsStopped())
                    continue;
                tryAppendQuestEntry(pQuest);
            }
        }
    }

    message.CurrentActorData = BuildActorData(pActor);

    message.FactionsContent = pActor->GetFactions();
    message.IsDragon = pActor->IsDragon();
    message.IsMount = pActor->IsMount();
    const Actor* pCommandingActor = pActor->GetCommandingActor();
    message.IsPlayerSummon =
        (pCommandingActor && pCommandingActor->formID == 0x14) ||
        (!pActor->GetExtension()->IsRemote() && isTemporary && pActor->IsPlayerTeammate());
    if (message.IsPlayerSummon && pActor->GetExtension())
        pActor->GetExtension()->SetPlayerSummonFlag(true);

    if (pNpc->IsTemporary())
        pNpc = pNpc->GetTemplateBase();

    if (isTemporary)
    {
        if (pNpc && !m_world.GetModSystem().GetServerModId(pNpc->formID, message.FormId))
        {
            spdlog::error("Server NPC form id not found for form id {:X}", pNpc->formID);
            return;
        }
    }

    // Serialize actions
    auto* const pExtension = pActor->GetExtension();

    message.LatestAction = pExtension->LatestAnimation;
    pActor->SaveAnimationVariables(message.LatestAction.Variables);

    const bool isLeader = m_world.Get().GetPartyService().IsLeader(); // Distinguish logs in 2-party      
    spdlog::info(__FUNCTION__ ": request id: {:X}, cookie: {:X}, entity: {:X}, isLeader: {}", formIdComponent.Id, sCookieSeed, to_integral(aEntity), isLeader);

    if (m_transport.Send(message))
    {
        m_world.emplace<WaitingForAssignmentComponent>(aEntity, sCookieSeed);

        sCookieSeed++;
    }
}

void CharacterService::CancelServerAssignment(const entt::entity aEntity, const uint32_t aFormId) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    const char* pName = (!pActor || !pActor->baseForm) ? "" : pActor->baseForm->GetName();

    if (m_world.all_of<RemoteComponent>(aEntity))
    {
        auto& remoteComponent = m_world.get<RemoteComponent>(aEntity);
        const bool isTempActor = IsNetworkCreatedActor(pActor);

        if (pActor)
        {
            if (isTempActor)
            {
                spdlog::info("Temporary Remote Unloaded {:X}", aFormId);
                PurgeDynamicActorCleanly(pActor);
            }
            else if (auto* pExt = pActor->GetExtension())
            {
                pExt->SetRemote(false);
                pExt->SetDownedPlayer(false);
            }
        }

        // Preserve exterior out-of-grid remote temporary actors (such as co-op players in Tamriel!)
        // in invisibleView so RunSpawnUpdates() seamlessly recreates their 3D & party HUD health bar
        // as soon as they re-enter the 5x5 cell grid (fixing Screenshot 2!).
        auto cachedIt = m_cachedRemoteSpawns.find(remoteComponent.Id);
        if (isTempActor && !m_savePurgeActive && cachedIt != m_cachedRemoteSpawns.end() &&
            PlayerCharacter::Get() && PlayerCharacter::Get()->GetWorldSpace())
        {
            remoteComponent.CachedRefId = 0;
            if (auto* pFaceGen = m_world.try_get<FaceGenComponent>(aEntity))
            {
                pFaceGen->Generated = false;
                pFaceGen->LastFaceNode = nullptr;
            }
            auto respawnCopy = cachedIt->second;
            if (const auto* pInterp = m_world.try_get<InterpolationComponent>(aEntity))
                respawnCopy.Position = pInterp->Position;
            // CRITICAL: Atomically strip FormIdComponent alongside PlayerComponent so RunRemoteUpdates() never
            // calls ForcePosition/ForceAction/FaceGenSystem on the deleted 0xFFxxxxxx Actor*, and RunSpawnUpdates()
            // (`entt::exclude<FormIdComponent>`) can cleanly recreate 3D when the actor re-enters the 5x5 grid.
            m_world.remove<FormIdComponent, EarlyAnimationBufferComponent, PlayerComponent>(aEntity);
            m_world.emplace_or_replace<WaitingFor3D>(aEntity, respawnCopy);
            m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
        }
        else
        {
            DeleteRemoteEntityComponents(aEntity);
        }
    }

    // In the event we were waiting for assignment, drop it
    if (m_world.all_of<WaitingForAssignmentComponent>(aEntity))
    {
        auto& waitingComponent = m_world.get<WaitingForAssignmentComponent>(aEntity);

        CancelAssignmentRequest message;
        message.Cookie = waitingComponent.Cookie;

        m_transport.Send(message);

        m_world.remove<WaitingForAssignmentComponent>(aEntity);
    }

    if (m_world.all_of<LocalComponent>(aEntity))
    {
        auto& localComponent = m_world.get<LocalComponent>(aEntity);

        RequestOwnershipTransfer request{};
        request.ServerId = localComponent.Id;
        request.OwnershipEpoch = localComponent.OwnershipEpoch;

        if (pActor)
        {
            if (!pActor->IsTemporary())
            {
                auto& modSystem = m_world.GetModSystem();

                if (TESWorldSpace* pWorldSpace = pActor->GetWorldSpace())
                {
                    if (!modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId))
                        spdlog::error("World space id not found, despite having a world space, {:X}", pWorldSpace->formID);
                }

                if (TESObjectCELL* pCell = pActor->GetParentCell())
                {
                    if (!modSystem.GetServerModId(pCell->formID, request.CellId))
                        spdlog::error("Cell id not found, despite having a cell, {:X}", pCell->formID);
                }

                request.Position = pActor->position;
            }
        }


        spdlog::info(
            __FUNCTION__ ": transferring ownership of local actor, formId: {:X}, server id: {:X}, worldspace: {:X}, cell: {:X}, position: "
                         "({}, {}, {}), name: {}",
            aFormId, request.ServerId, request.WorldSpaceId.BaseId, request.CellId.BaseId, request.Position.x, request.Position.y, request.Position.z, pName);

        m_transport.Send(request);

        m_world.remove<LocalAnimationComponent, LocalComponent>(aEntity);
    }

    if (pActor)
        pActor->GetExtension()->SetNakedDeadline();
}

Actor* CharacterService::CreateCharacterForEntity(entt::entity aEntity) const noexcept
{
    auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(aEntity);
    auto* pInterpolationComponent = m_world.try_get<InterpolationComponent>(aEntity);

    if (!pWaitingFor3D || !pInterpolationComponent)
    {
        spdlog::error(__FUNCTION__ ": could not find WaitingFor3D or InterpolationComponent");
        return nullptr;
    }

    auto& acMessage = pWaitingFor3D->SpawnRequest;

    Actor* pActor = nullptr;

    // Custom forms
    if (acMessage.FormId == GameId{})
    {
        TESNPC* pNpc = nullptr;

        if (acMessage.BaseId != GameId{})
        {
            const uint32_t cNpcId = World::Get().GetModSystem().GetGameId(acMessage.BaseId);
            if (cNpcId == 0)
            {
                spdlog::error("Failed to retrieve NPC, it will not be spawned, possibly missing mod");
                return nullptr;
            }

            pNpc = Cast<TESNPC>(TESForm::GetById(cNpcId));
            if (!pNpc)
            {
                spdlog::error("Form {:X} is not a TESNPC, skipping streamed remote spawn", cNpcId);
                return nullptr;
            }
            if (!acMessage.AppearanceBuffer.empty())
                pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            if ((pNpc->formID & 0xFF000000) != 0xFF000000)
                pNpc->UnsetChanged(0xFFFFFFFF);
        }
        else
        {
            pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
            FaceGenSystem::Setup(m_world, aEntity, acMessage.FaceTints);
        }

        pActor = Actor::Create(pNpc);
        if (pActor && pActor->GetExtension())
            pActor->GetExtension()->SetCreatedByNetwork(true);
    }
    else
    {
        const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);
        if (cActorId == 0)
        {
            m_world.destroy(aEntity);
            return nullptr;
        }

        pActor = Cast<Actor>(TESForm::GetById(cActorId));
        if (!pActor || pActor->IsStillLoading())
            return nullptr;

        auto existingView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        for (entt::entity other : existingView)
        {
            if (other == aEntity || existingView.get<FormIdComponent>(other).Id != cActorId)
                continue;

            if (m_world.any_of<LocalComponent, RemoteComponent>(other))
            {
                m_world.destroy(aEntity);
                return nullptr;
            }

            if (auto* pWaitAssign = m_world.try_get<WaitingForAssignmentComponent>(other))
            {
                CancelAssignmentRequest cancelMsg{};
                cancelMsg.Cookie = pWaitAssign->Cookie;
                m_transport.Send(cancelMsg);
            }
            m_pendingLocalTempSpawns.erase(other);
            m_world.destroy(other);
            break;
        }

        if (pActor->IsDisabled())
            pActor->EnableImpl();
    }

    auto& remoteComponent = m_world.get<RemoteComponent>(aEntity);

    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": could not spawn actor for remote server id {:X}.", remoteComponent.Id);
        return nullptr;
    }

    // CRITICAL: Always attach FormIdComponent immediately when materializing a deferred WaitingFor3D entity!
    // Without FormIdComponent, waitingView (FormIdComponent + WaitingFor3D) never equips body/skin/armor meshes
    // (causing invisible enemies) and damage/death handlers cannot resolve ServerId (causing immortal enemies).
    remoteComponent.CachedRefId = pActor->formID;
    m_world.emplace_or_replace<FormIdComponent>(aEntity, pActor->formID);

    pActor->GetExtension()->SetRemote(true);
    pActor->rotation.x = acMessage.Rotation.x;
    pActor->rotation.z = acMessage.Rotation.y;
    pActor->MoveTo(PlayerCharacter::Get()->parentCell, pInterpolationComponent->Position);

    if (!acMessage.IsPlayer)
    {
        if (const auto maxHealthIt = acMessage.InitialActorValues.ActorMaxValuesList.find(ActorValueInfo::kHealth);
            maxHealthIt != acMessage.InitialActorValues.ActorMaxValuesList.end())
        {
            m_world.GetActorValueService().SeedUnscaledMaxHealthFromRemote(pActor->formID, maxHealthIt->second);
        }
    }

    pActor->SetActorValues(acMessage.InitialActorValues);

    pActor->GetExtension()->SetPlayer(acMessage.IsPlayer);
    if (acMessage.IsPlayer)
    {
        pActor->SetIgnoreFriendlyHit(true);
        pActor->SetPlayerRespawnMode();
        m_world.emplace_or_replace<PlayerComponent>(aEntity, acMessage.PlayerId);
        m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
        if (m_world.GetDownedStateService().IsRemotePlayerDowned(acMessage.PlayerId))
            m_world.GetDownedStateService().ApplyRemoteActorDownedPosture(pActor, true);
    }

    if (acMessage.IsPlayerSummon)
    {
        AssignRemoteSummonOwner(m_world, pActor, acMessage.PlayerId);
    }

    if (pActor->IsDead() != acMessage.IsDead)
        acMessage.IsDead ? KillActorSuppressingDuplicateXp(pActor) : pActor->Respawn();

    spdlog::info("Spawned character for entity, server id: {:X}", remoteComponent.Id);

    return pActor;
}

ActorData CharacterService::BuildActorData(Actor* apActor) const noexcept
{
    ActorData actorData{};
    actorData.InitialActorValues = apActor->GetEssentialActorValues();
    actorData.InitialInventory = apActor->GetActorInventory();
    actorData.IsDead = apActor->IsDead();
    actorData.IsWeaponDrawn = apActor->actorState.IsWeaponFullyDrawn();

    return actorData;
}

void CharacterService::RunLocalUpdates() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 100ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    ClientReferencesMoveRequest message;
    message.Tick = m_transport.GetClock().GetCurrentTick();

    auto animatedLocalView = m_world.view<LocalComponent, LocalAnimationComponent, FormIdComponent>();

    for (auto entity : animatedLocalView)
    {
        auto& localComponent = animatedLocalView.get<LocalComponent>(entity);
        auto& animationComponent = animatedLocalView.get<LocalAnimationComponent>(entity);
        auto& formIdComponent = animatedLocalView.get<FormIdComponent>(entity);

        AnimationSystem::Serialize(m_world, message, localComponent, animationComponent, formIdComponent);
    }

    m_transport.Send(message);
}

void CharacterService::RunRemoteUpdates() noexcept
{
    // Delay by 300ms to let the interpolation system accumulate interpolation points
    const auto tick = m_transport.GetClock().GetCurrentTick() - 300;

    // Interpolation has to keep running even if the actor is not in view, otherwise we will never know if we need to spawn it
    auto interpolatedEntities = m_world.view<RemoteComponent, InterpolationComponent>();

    for (auto entity : interpolatedEntities)
    {
        auto* pFormIdComponent = m_world.try_get<FormIdComponent>(entity);
        auto& interpolationComponent = interpolatedEntities.get<InterpolationComponent>(entity);

        Actor* pActor = nullptr;
        if (pFormIdComponent)
        {
            auto* pForm = TESForm::GetById(pFormIdComponent->Id);
            pActor = Cast<Actor>(pForm);
        }

        InterpolationSystem::Update(pActor, interpolationComponent, tick, m_world.try_get<RemoteAnimationComponent>(entity));
    }

    auto animatedView = m_world.view<RemoteComponent, RemoteAnimationComponent, FormIdComponent>();

    for (auto entity : animatedView)
    {
        auto& animationComponent = animatedView.get<RemoteAnimationComponent>(entity);
        auto& formIdComponent = animatedView.get<FormIdComponent>(entity);

        auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        AnimationSystem::Update(m_world, pActor, animationComponent, tick);
    }

    auto facegenView = m_world.view<FormIdComponent, FaceGenComponent>();

    for (auto entity : facegenView)
    {
        auto& formIdComponent = facegenView.get<FormIdComponent>(entity);
        auto& faceGenComponent = facegenView.get<FaceGenComponent>(entity);

        const auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        FaceGenSystem::Update(m_world, pActor, faceGenComponent);
    }

    auto waitingView = m_world.view<FormIdComponent, WaitingFor3D>();

    Vector<entt::entity> toRemove;
    for (auto entity : waitingView)
    {
        auto& formIdComponent = waitingView.get<FormIdComponent>(entity);
        auto& waitingFor3D = waitingView.get<WaitingFor3D>(entity);

        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor || !pActor->GetNiNode())
            continue;

        // By now, the actor has materialized in the world and is ready for further setup

        pActor->SetActorInventory(waitingFor3D.SpawnRequest.InventoryContent);
        pActor->SetFactions(waitingFor3D.SpawnRequest.FactionsContent);

        if (!waitingFor3D.SpawnRequest.ActionsToReplay.Actions.empty())
        {
            pActor->LoadAnimationVariables(waitingFor3D.SpawnRequest.ActionsToReplay.Actions[0].Variables);
        }

        m_weaponDrawUpdates[pActor->formID] = {waitingFor3D.SpawnRequest.IsWeaponDrawn};

        if (pActor->IsDead() != waitingFor3D.SpawnRequest.IsDead)
            waitingFor3D.SpawnRequest.IsDead ? KillActorSuppressingDuplicateXp(pActor) : pActor->Respawn();

        if (pActor->IsVampireLord())
            pActor->FixVampireLordModel();

        if (waitingFor3D.SpawnRequest.IsPlayer &&
            m_world.GetDownedStateService().IsRemotePlayerDowned(waitingFor3D.SpawnRequest.PlayerId))
        {
            m_weaponDrawUpdates[pActor->formID] = {false};
            m_world.GetDownedStateService().ApplyRemoteActorDownedPosture(pActor, true);
        }

        if (waitingFor3D.SpawnRequest.IsPlayerSummon)
        {
            AssignRemoteSummonOwner(m_world, pActor, waitingFor3D.SpawnRequest.PlayerId);
        }

        // (Re)start naked NPC deadline. Actor MUST be fully constructed before check clock starts.
        pActor->GetExtension()->SetNakedDeadline();

        toRemove.push_back(entity);

        spdlog::info(__FUNCTION__ ": applied 3D for actor, form id: {:X}, name {}", pActor->formID, pActor->baseForm ? pActor->baseForm->GetName() : "<null>");
    }

    for (auto entity : toRemove)
        m_world.remove<WaitingFor3D>(entity);
}

void CharacterService::RunFactionsUpdates() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 2000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    RequestFactionsChanges message;

    auto factionedActors = m_world.view<LocalComponent, CacheComponent, FormIdComponent>();
    for (auto entity : factionedActors)
    {
        auto& formIdComponent = factionedActors.get<FormIdComponent>(entity);
        auto& localComponent = factionedActors.get<LocalComponent>(entity);
        auto& cacheComponent = factionedActors.get<CacheComponent>(entity);

        const auto* pForm = TESForm::GetById(formIdComponent.Id);
        const auto* pActor = Cast<Actor>(pForm);
        if (!pActor)
            continue;

        // Check if cached factions and current factions are identical
        auto factions = pActor->GetFactions();

        if (cacheComponent.FactionsContent == factions)
            continue;

        cacheComponent.FactionsContent = factions;

        // If not send the current factions and replace the cached factions
        message.Changes[localComponent.Id] = factions;
    }

    if (!message.Changes.empty())
        m_transport.Send(message);
}

void CharacterService::RunSpawnUpdates() const noexcept
{
    if (m_savePurgeActive)
        return;

    PlayerCharacter* pLocalPlayer = PlayerCharacter::Get();
    if (!pLocalPlayer || !pLocalPlayer->parentCell)
        return;

    auto invisibleView = m_world.view<RemoteComponent, InterpolationComponent, RemoteAnimationComponent, WaitingFor3D>(entt::exclude<FormIdComponent>);
    Vector<entt::entity> entities(invisibleView.begin(), invisibleView.end());

    for (const auto entity : entities)
    {
        if (!m_world.valid(entity))
            continue;

        auto& remoteComponent = m_world.get<RemoteComponent>(entity);
        auto& interpolationComponent = m_world.get<InterpolationComponent>(entity);

        if (pLocalPlayer->GetWorldSpace())
        {
            const TES* pTES = TES::Get();
            if (!pTES)
                continue;

            float characterX = interpolationComponent.Position.x;
            float characterY = interpolationComponent.Position.y;
            const auto characterCoords = GridCellCoords::CalculateGridCellCoords(characterX, characterY);
            const auto playerCoords = GridCellCoords(pTES->centerGridX, pTES->centerGridY);

            // TODO(cosideci): IsDragon probably shouldn't be straight up false here.
            if (GridCellCoords::IsCellInGridCell(characterCoords, playerCoords, false))
            {
                auto* pActor = remoteComponent.CachedRefId ? Cast<Actor>(TESForm::GetById(remoteComponent.CachedRefId)) : nullptr;
                if (!pActor)
                {
                    pActor = CreateCharacterForEntity(entity);
                    if (!pActor || !m_world.valid(entity))
                        continue;
                }

                remoteComponent.CachedRefId = pActor->formID;
                m_world.emplace_or_replace<FormIdComponent>(entity, pActor->formID);
                if (m_world.all_of<PlayerComponent>(entity))
                    m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();

                pActor->MoveTo(pLocalPlayer->parentCell, interpolationComponent.Position);
            }
        }
        else if (remoteComponent.CachedRefId == 0)
        {
            // Materialize deferred interior spawns queued while local parentCell was nullptr during cell load.
            auto* pActor = CreateCharacterForEntity(entity);
            if (!pActor || !m_world.valid(entity))
                continue;

            remoteComponent.CachedRefId = pActor->formID;
            m_world.emplace_or_replace<FormIdComponent>(entity, pActor->formID);
            if (m_world.all_of<PlayerComponent>(entity))
                m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();

            pActor->MoveTo(pLocalPlayer->parentCell, interpolationComponent.Position);
        }
    }
}

void CharacterService::RunExperienceUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    if (m_cachedExperience == 0.f)
        return;

    if (!World::Get().GetPartyService().IsInParty())
        return;

    SyncExperienceRequest message;
    message.Experience = m_cachedExperience;

    m_cachedExperience = 0.f;

    m_transport.Send(message);

    spdlog::debug("Sending over experience {}", message.Experience);
}

void CharacterService::RunRfabExperienceUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    if (!m_transport.IsConnected())
    {
        m_rfabExperienceInitialized = false;
        return;
    }

    if (now > m_remoteQuestXpWindowUntil)
    {
        m_localQuestXpGainedInWindow = 0;
        m_creditToRefundOnLocalQuestXp = 0;
    }

    TESGlobal* pRfabXp = GetRfabXpGlobal();
    if (pRfabXp == nullptr)
        return;

    const int32_t currentExperience = ReadRfabXp(pRfabXp);
    if (!m_rfabExperienceInitialized)
    {
        m_lastRfabExperience = currentExperience;
        m_rfabExperienceInitialized = true;
        return;
    }

    // While RFAB_Menu ("CustomMenu") is open, RFAB_XP_Amount goes down on skill allocation and back up on
    // undo/cancel — track wallet baseline without broadcasting fake XP deltas to party peers!
    if (auto* pUi = UI::Get(); pUi && pUi->GetMenuOpen(BSFixedString("CustomMenu")))
    {
        m_lastRfabExperience = currentExperience;
        return;
    }

    int32_t delta = currentExperience - m_lastRfabExperience;
    m_lastRfabExperience = currentExperience;

    if (delta <= 0)
        return;

    // Suppress echo & double-award when a synced quest stage fragment awarded XP locally on this client
    if (now <= m_remoteQuestXpWindowUntil)
    {
        if (m_creditToRefundOnLocalQuestXp > 0)
        {
            const int32_t refund = std::min(delta, m_creditToRefundOnLocalQuestXp);
            int32_t adjustedExperience = currentExperience;
            if (ApplyRfabExperience(pRfabXp, -refund, adjustedExperience))
            {
                m_creditToRefundOnLocalQuestXp -= refund;
                m_lastRfabExperience = adjustedExperience;
            }
            delta -= refund;
        }

        if (delta > 0)
            m_localQuestXpGainedInWindow += delta;

        return;
    }

    // Normalize out the sender's personal RFAB_XP_Multiplier (e.g. Adventurer's Ring / Standing Stone)
    // so each party member receives exact 1x base XP multiplied by THEIR OWN personal RFAB_XP_Multiplier!
    const float senderMultiplier = m_world.GetRfabAdapterService().GetLocalXpMultiplier();
    int32_t baseDelta = delta;
    if (std::fabs(senderMultiplier - 1.0f) > 0.01f)
    {
        baseDelta = static_cast<int32_t>(std::lround(static_cast<double>(delta) / static_cast<double>(senderMultiplier)));
        if (baseDelta >= 5)
            baseDelta = ((baseDelta + 2) / 5) * 5;
    }

    if (baseDelta <= 0)
        return;

    SyncRfabExperienceRequest message;
    message.Experience = static_cast<uint32_t>(std::min(baseDelta, static_cast<int32_t>(cMaxRfabXpDelta)));

    m_transport.Send(message);

    spdlog::debug("Sending over RFAB experience {}", message.Experience);
}

void CharacterService::ApplyCachedWeaponDraws(const UpdateEvent& acUpdateEvent) noexcept
{
    std::vector<uint32_t> toRemove{};

    for (auto& [cId, _] : m_weaponDrawUpdates)
    {
        auto& data = m_weaponDrawUpdates[cId];

        data.m_timer += acUpdateEvent.Delta;

        // We do 2 passes because Skyrim's weapon drawing is the most finnicky thing in existence
        double maxTime = data.m_isFirstPass ? 0.5 : 2.0;
        if (data.m_timer <= maxTime)
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(cId));
        if (!pActor)
            continue;

        pActor->SetWeaponDrawnEx(data.m_drawWeapon);

        if (!data.m_isFirstPass)
            toRemove.push_back(cId);

        data.m_isFirstPass = false;
    }

    for (uint32_t id : toRemove)
        m_weaponDrawUpdates.erase(id);
}
