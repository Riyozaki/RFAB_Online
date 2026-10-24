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

namespace
{
constexpr uint32_t cMaxRfabXpDelta = 1'000'000;

void PurgeDynamicActorCleanly(Actor* apActor, bool aImmediateDisable = false) noexcept
{
    if (!apActor)
        return;

    apActor->UnsetChanged(0xFFFFFFFF);
    apActor->SetSkipSaveFlag(true);
    if (apActor->baseForm && (apActor->baseForm->formID & 0xFF000000) == 0xFF000000)
        apActor->baseForm->UnsetChanged(0xFFFFFFFF);

    // During normal runtime cell transitions (NotifyRemoveCharacter / CancelServerAssignment), rely strictly on
    // Skyrim's deferred end-of-frame Delete() queue (identical to v1.0.0) so 3D/Havok/shader resources are never
    // torn down mid-tick. Only invoke native C++ virtual DisableImpl() (vtable[0x89]) during pre-save / world-load sweeps.
    if (aImmediateDisable && !apActor->IsDisabled())
        apActor->DisableImpl();

    apActor->Delete();
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
    if (pActor && ((pActor->formID & 0xFF000000) == 0xFF000000))
    {
        PurgeDynamicActorCleanly(pActor);
        spdlog::info("\tDeleted actor {:X}", aFormId);
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

    entt::entity entity;

    const auto view = m_world.view<RemoteComponent>();
    const auto it = std::find_if(
        std::begin(view), std::end(view),
        [&acEvent, view](entt::entity entity)
        {
            auto& remoteComponent = view.get<RemoteComponent>(entity);
            return remoteComponent.CachedRefId == acEvent.FormId;
        });

    if (it != std::end(view))
    {
        if (pActor)
            pActor->GetExtension()->SetRemote(true);

        entity = *it;
    }
    else
        entity = m_world.create();

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
    m_pendingPostSaveRemoteRestore = false;
    m_pendingLocalTempSpawns.clear();
    m_cachedRemoteSpawns.clear();
    NotifySaveLoadReset();

    // Go through all the forms that were previously detected
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    Vector<entt::entity> entities(view.begin(), view.end());

    for (auto entity : entities)
    {
        auto& formIdComponent = m_world.get<FormIdComponent>(entity);
        // Delete all temporary remote clones on connect
        if (formIdComponent.Id != 0x14 && (formIdComponent.Id & 0xFF000000) == 0xFF000000)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            if (pActor)
                PurgeDynamicActorCleanly(pActor, true);

            continue;
        }

        ProcessNewEntity(entity);
    }
}

void CharacterService::OnDisconnected(const DisconnectedEvent& acDisconnectedEvent) noexcept
{
    PurgeAllDynamicActorsForWorldLoad();
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
            const bool hasDynamicBase = pActor->baseForm && ((pActor->baseForm->formID & 0xFF000000) == 0xFF000000);
            if (pActor->GetExtension()->IsRemote() || m_world.all_of<RemoteComponent>(entity) || hasDynamicBase)
            {
                pActor->SetSkipSaveFlag(true);
                pActor->UnsetChanged(0xFFFFFFFF);
                if (hasDynamicBase)
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

void CharacterService::PurgeAllDynamicActorsForWorldLoad() noexcept
{
    m_savePurgeActive = false;
    m_pendingPostSaveRemoteRestore = false;
    m_pendingLocalTempSpawns.clear();
    m_cachedRemoteSpawns.clear();
    m_weaponDrawUpdates.clear();
    NotifySaveLoadReset();

    auto remoteView = m_world.view<FormIdComponent, RemoteComponent>();
    Vector<entt::entity> dynamicEntities;
    for (auto entity : remoteView)
    {
        const uint32_t formId = remoteView.get<FormIdComponent>(entity).Id;
        Actor* pActor = Cast<Actor>(TESForm::GetById(formId));
        if (!pActor)
            continue;

        if ((formId & 0xFF000000) == 0xFF000000 || pActor->GetExtension()->IsRemotePlayer())
        {
            PurgeDynamicActorCleanly(pActor, true);
            dynamicEntities.push_back(entity);
        }
        else
        {
            pActor->GetExtension()->SetRemote(false);
        }
    }

    for (entt::entity entity : dynamicEntities)
    {
        if (m_world.valid(entity))
            m_world.destroy(entity);
    }

    auto formView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (auto entity : formView)
    {
        const uint32_t formId = formView.get<FormIdComponent>(entity).Id;
        if (formId != 0x14 && (formId & 0xFF000000) == 0xFF000000)
        {
            if (Actor* pActor = Cast<Actor>(TESForm::GetById(formId)))
            {
                const bool hasDynamicBase = pActor->baseForm && ((pActor->baseForm->formID & 0xFF000000) == 0xFF000000);
                if (pActor->GetExtension()->IsRemote() || hasDynamicBase)
                    PurgeDynamicActorCleanly(pActor, true);
            }
        }
    }

    m_world.clear<
        WaitingFor3D,
        WaitingForAssignmentComponent,
        LocalComponent,
        LocalAnimationComponent,
        RemoteComponent,
        InterpolationComponent,
        RemoteAnimationComponent>();
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
        m_world.emplace_or_replace<PlayerComponent>(cEntity, acMessage.PlayerId);

    if (acMessage.Owner)
    {
        m_ownershipEpochTracker.AcceptGrant(acMessage.ServerId, acMessage.OwnershipEpoch);
        spdlog::info(__FUNCTION__ ": received local actor, form id: {:X}, {}", pActor->formID, pActor->baseForm->GetName());

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
        spdlog::info(__FUNCTION__ ": received remote actor, form id: {:X}, isweapondrawn: {}, {}", pActor->formID, acMessage.IsWeaponDrawn, pActor->baseForm->GetName());

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
    if (m_savePurgeActive)
    {
        if (acMessage.FormId == GameId{})
        {
            m_cachedRemoteSpawns[acMessage.ServerId] = acMessage;
            const entt::entity deferredEntity = m_world.create();
            m_world.emplace_or_replace<RemoteComponent>(deferredEntity, acMessage.ServerId, 0u);
            auto& interp = InterpolationSystem::Setup(m_world, deferredEntity);
            interp.Position = acMessage.Position;
            AnimationSystem::Setup(m_world, deferredEntity);
            AnimationSystem::AddActionsForReplay(m_world.get<RemoteAnimationComponent>(deferredEntity), acMessage.ActionsToReplay);
            m_world.emplace_or_replace<WaitingFor3D>(deferredEntity, acMessage);
        }
        return;
    }

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
    if (!pLocalPlayer || !pLocalPlayer->parentCell)
    {
        // Local player is mid-cell-transition / loading screen (`parentCell == nullptr`):
        // stage custom temporary spawns into WaitingFor3D (CachedRefId = 0, no FormIdComponent) instead of
        // calling Actor::Create / MoveTo(nullptr), so RunSpawnUpdates() materializes them once the cell loads.
        if (acMessage.FormId == GameId{})
        {
            m_cachedRemoteSpawns[acMessage.ServerId] = acMessage;
            const entt::entity deferredEntity = m_world.create();
            m_world.emplace_or_replace<RemoteComponent>(deferredEntity, acMessage.ServerId, 0u);
            auto& interp = InterpolationSystem::Setup(m_world, deferredEntity);
            interp.Position = acMessage.Position;
            AnimationSystem::Setup(m_world, deferredEntity);
            AnimationSystem::AddActionsForReplay(m_world.get<RemoteAnimationComponent>(deferredEntity), acMessage.ActionsToReplay);
            m_world.emplace_or_replace<WaitingFor3D>(deferredEntity, acMessage);
        }
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

        // Moving an uninitialized reference crashes (e.g. null race); local cell discovery will assign it once loaded.
        if (pActor->IsStillLoading())
        {
            spdlog::info("Skipped spawn for actor {:X} because its cell ref is still loading, server id: {:X}", cActorId, acMessage.ServerId);
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
        if (auto* pFormIdComponent = m_world.try_get<FormIdComponent>(*itor))
            CharacterService::DeleteTempActor(pFormIdComponent->Id);

        DeleteRemoteEntityComponents(*itor);
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
    auto view = m_world.view<FormIdComponent>();

    const auto it = std::find_if(view.begin(), view.end(), [view](auto entity) { return view.get<FormIdComponent>(entity).Id == 0x14; });

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
            pMount->GetExtension()->SetRemote(true);

            InterpolationSystem::Setup(m_world, entity);
            AnimationSystem::Setup(m_world, entity);

            break;
        }
    }

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
    auto sceneId = isInScene ? pActor->GetCurrentScene()->formID : 0;
    bool isTaskDialogue = pActor->IsTalking() && pActor->IsInDialogueWithPlayer();
    bool isSpeakingInScene = pActor->IsSpeakingInScene();

    const bool willSync = isTaskDialogue || isLocal && !isInScene;

    spdlog::debug(
        __FUNCTION__ ": isLocal {}, isInScene {}, isSpeakingInScene {}, isTaskDialogue {}, willSync {}, scene {:X}, Actor "
                     "{:X}, serverId {:X}, isLeader {}, name {}, soundFile {}",
        isLocal, isInScene, isSpeakingInScene, isTaskDialogue, willSync, sceneId, pActor->formID, serverIdRes.value(), isLeader, pActor->baseForm->GetName(),
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
        __FUNCTION__ ": playing dialogue Actor {:X}, serverId {:X}, isLeader {}, name {}, soundFile {}", pActor->formID, acMessage.ServerId, isLeader, pActor->baseForm->GetName(),
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
    auto sceneId = isInScene ? pActor->GetCurrentScene()->formID : 0;
    bool isTaskDialogue = pActor->IsTalking() && pActor->IsInDialogueWithPlayer();

    const bool willSync = isTaskDialogue || isLocal && !isInScene;

    spdlog::debug(
        __FUNCTION__ ": isLocal {}, isInScene {}, isSpeakingInScene {}, isTaskDialogue {}, willSync {}, scene {:X}, Actor "
                     "{:X}, serverId {:X}, isLeader {}, name {}, subtitle {}",
        isLocal, isInScene, isSpeakingInScene, isTaskDialogue, willSync, sceneId, pActor->formID, serverIdRes.value(), isLeader, pActor->baseForm->GetName(),
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
                     pActor->formID, acMessage.ServerId, isLeader, pActor->baseForm->GetName(), acMessage.Text);

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
        // TODO(cosideci): don't just take all actors (i.e. from other parties),
        // maybe check it server side, add a variable to the request.
        if (m_world.GetPartyService().IsLeader() && !pActor->IsTemporary() && !pActor->IsMount())
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

    if (!m_world.GetModSystem().GetServerModId(pActor->parentCell->formID, message.CellId))
    {
        spdlog::error("Server cell id not found for cell id {:X}", pActor->parentCell->formID);
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

        for (const auto& objective : PlayerCharacter::Get()->objectives)
        {
            auto* pQuest = objective.instance->quest;
            if (!pQuest)
                continue;

            if (!QuestService::IsNonSyncableQuest(pQuest))
            {
                GameId id{};

                if (modSystem.GetServerModId(pQuest->formID, id))
                {
                    auto& entry = questLog.emplace_back();
                    entry.Stage = pQuest->currentStage;
                    entry.Id = id;
                }
            }
        }

        // remove duplicates
        const auto ip = std::unique(questLog.begin(), questLog.end());
        questLog.resize(std::distance(questLog.begin(), ip));
    }

    message.CurrentActorData = BuildActorData(pActor);

    message.FactionsContent = pActor->GetFactions();
    message.IsDragon = pActor->IsDragon();
    message.IsMount = pActor->IsMount();
    const Actor* pCommandingActor = pActor->GetCommandingActor();
    message.IsPlayerSummon =
        (pCommandingActor && pCommandingActor->formID == 0x14) ||
        (!pActor->GetExtension()->IsRemote() && isTemporary && pActor->IsPlayerTeammate());

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
    const char* pName = !pActor ? "" : pActor->baseForm->GetName();

    if (m_world.all_of<RemoteComponent>(aEntity))
    {
        auto& remoteComponent = m_world.get<RemoteComponent>(aEntity);
        const bool isTempActor = pActor && pActor->IsTemporary();

        if (pActor)
        {
            if (isTempActor)
            {
                spdlog::info("Temporary Remote Unloaded {:X}", aFormId);
                PurgeDynamicActorCleanly(pActor);
            }
            else
            {
                pActor->GetExtension()->SetRemote(false);
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
    }

    auto& remoteComponent = m_world.get<RemoteComponent>(aEntity);

    if (!pActor)
    {
        spdlog::error(__FUNCTION__ ": could not spawn actor for remote server id {:X}.", remoteComponent.Id);
        return nullptr;
    }

    pActor->GetExtension()->SetRemote(true);
    pActor->rotation.x = acMessage.Rotation.x;
    pActor->rotation.z = acMessage.Rotation.y;
    pActor->MoveTo(PlayerCharacter::Get()->parentCell, pInterpolationComponent->Position);
    pActor->SetActorValues(acMessage.InitialActorValues);

    pActor->GetExtension()->SetPlayer(acMessage.IsPlayer);
    if (acMessage.IsPlayer)
    {
        pActor->SetIgnoreFriendlyHit(true);
        pActor->SetPlayerRespawnMode();
        m_world.emplace_or_replace<PlayerComponent>(aEntity, acMessage.PlayerId);
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

        // (Re)start naked NPC deadline. Actor MUST be fully constructed before check clock starts.
        pActor->GetExtension()->SetNakedDeadline();

        toRemove.push_back(entity);

        spdlog::info(__FUNCTION__ ": applied 3D for actor, form id: {:X}, name {}", pActor->formID, pActor->baseForm->GetName());
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
                    if (!pActor)
                        continue;

                    remoteComponent.CachedRefId = pActor->formID;
                }
                else
                {
                    m_world.emplace_or_replace<FormIdComponent>(entity, pActor->formID);
                }

                pActor->MoveTo(pLocalPlayer->parentCell, interpolationComponent.Position);
            }
        }
        else if (remoteComponent.CachedRefId == 0)
        {
            // Materialize deferred interior spawns queued while local parentCell was nullptr during cell load.
            auto* pActor = CreateCharacterForEntity(entity);
            if (!pActor)
                continue;

            remoteComponent.CachedRefId = pActor->formID;
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
