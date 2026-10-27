#include "Forms/TESObjectCELL.h"
#include "Forms/TESWorldSpace.h"
#include "Services/PapyrusService.h"
#include <Services/PartyService.h>

#include <Services/CharacterService.h>
#include <Services/DownedStateService.h>
#include <Services/QuestService.h>
#include <Services/RfabAdapterService.h>
#include <Services/TransportService.h>

#include <Games/Overrides.h>
#include <Games/References.h>
#include <Games/TES.h>
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
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyPlayerLeft.h>
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
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestDeathStateChange.h>
#include <DefaultObjectManager.h>

#include <World.h>
#include <Games/TES.h>
#include <Games/Skyrim/Interface/UI.h>
#include <CoopAnimationState.h>

#include <cmath>

extern thread_local bool g_forceAnimation;

namespace
{
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

std::vector<uint32_t>& GetDeferredDialogueDeletes() noexcept
{
    static std::vector<uint32_t> s_deferred;
    return s_deferred;
}

bool IsDialogueMenuCurrentlyOpen() noexcept
{
    if (auto* pUi = UI::Get())
        return pUi->GetMenuOpen(BSFixedString("Dialogue Menu"));
    return false;
}

void FlushDeferredDialogueDeletes() noexcept
{
    auto& queue = GetDeferredDialogueDeletes();
    if (queue.empty() || IsDialogueMenuCurrentlyOpen())
        return;

    const auto pending = std::move(queue);
    queue.clear();

    for (const uint32_t formId : pending)
    {
        if (formId == 0 || formId == 0x14 || (formId & 0xFF000000u) != 0xFF000000u)
            continue;

        if (Actor* pActor = Cast<Actor>(TESForm::GetById(formId)))
        {
            if (pActor->IsTalking())
            {
                queue.push_back(formId);
                continue;
            }

            if (auto* pExt = pActor->GetExtension())
            {
                pExt->SetRemote(false);
                pExt->SetDownedPlayer(false);
            }
            pActor->Delete();
        }
    }
}

void PurgeDynamicActorCleanly(Actor* apActor) noexcept
{
    if (!apActor || apActor->formID == 0x14)
        return;

    apActor->UnsetChanged(0xFFFFFFFF);
    apActor->SetSkipSaveFlag(true);
    if (apActor->baseForm && (apActor->baseForm->formID & 0xFF000000u) == 0xFF000000u)
    {
        apActor->baseForm->SetSkipSaveFlag(true);
        apActor->baseForm->UnsetChanged(0xFFFFFFFF);
    }

    // NEVER call synchronous DisableImpl() right before Delete():
    // DisableImpl() immediately detaches MovementControllerNPC's Actor* owner pointer (RCX = 0),
    // while Delete() queues a deferred Disable+Delete task into BSTaskPool (SE 36016) that then
    // crashes at SkyrimSE.exe+0x6EDFC2 (SE 40691, [RCX + 0x1F0] Actor::race) when flushed.
    if ((apActor->formID & 0xFF000000u) == 0xFF000000u)
    {
        // SkyMP worldCleanerService guard: calling Delete() on a dynamic Actor while Dialogue Menu is open
        // or while the actor is mid-voice-line can crash MenuTopicManager / BSSoundHandle.
        // Keep `SetRemote(true)` UNTIL `FlushDeferredDialogueDeletes()` deletes it so local AI never wakes up!
        if (IsDialogueMenuCurrentlyOpen() || apActor->IsTalking())
        {
            if (auto* pExt = apActor->GetExtension())
            {
                pExt->SetRemote(true);
                pExt->SetDownedPlayer(false);
            }
            auto& deferred = GetDeferredDialogueDeletes();
            if (std::find(deferred.begin(), deferred.end(), apActor->formID) == deferred.end())
                deferred.push_back(apActor->formID);
            return;
        }

        if (auto* pExt = apActor->GetExtension())
        {
            pExt->SetRemote(false);
            pExt->SetDownedPlayer(false);
        }
        apActor->Delete();
        return;
    }

    if (auto* pExt = apActor->GetExtension())
    {
        pExt->SetRemote(false);
        pExt->SetDownedPlayer(false);
    }
}

void SanitizeActorCombatStateForLocalOwnership(Actor* apActor) noexcept
{
    if (!apActor || apActor->formID == 0x14 || apActor->IsDead())
        return;

    // Clear stuck transient ActorState bits copied mid-action while the NPC was Remote:
    // - flags1 bits 28..31 (meleeAttackState), bits 25..27 (knockState), & stale SitSleepState (bits 14..17)
    //   when no valid nearby furniture is occupied or weapons are already drawn
    // - flags2 bits 10..11 (recoil), bit 13 (staggered), & intermediate weapon draw/sheathe transition states
    const bool forceDefaultFurnitureExit = CoopAnimationState::NormalizeLocalOwnershipActorState(
        apActor->actorState.flags1,
        apActor->actorState.flags2,
        apActor->HasValidNearbyFurniture());

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

        if (forceDefaultFurnitureExit)
        {
            BSFixedString defaultStateEvt("IdleForceDefaultState");
            apActor->SendAnimationEvent(&defaultStateEvt);
        }

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
} // namespace

uint64_t CharacterService::NowSteadyWorldViewMs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void CharacterService::KillActorSuppressingDuplicateXp(Actor* apActor) noexcept
{
    if (!apActor)
        return;

    apActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, -1.0f);
    apActor->Kill();
}

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
    m_playerCellChangedConnection = m_dispatcher.sink<NotifyPlayerCellChanged>().connect<&CharacterService::OnPlayerCellChanged>(this);
    m_playerLeftConnection = m_dispatcher.sink<NotifyPlayerLeft>().connect<&CharacterService::OnPlayerLeft>(this);

    m_formIdConstructConn = m_world.on_construct<FormIdComponent>().connect<&CharacterService::OnFormIdIndexUpdated>(this);
    m_formIdUpdateConn = m_world.on_update<FormIdComponent>().connect<&CharacterService::OnFormIdIndexUpdated>(this);
    m_formIdDestroyConn = m_world.on_destroy<FormIdComponent>().connect<&CharacterService::OnFormIdIndexDestroyed>(this);

    m_localConstructConn = m_world.on_construct<LocalComponent>().connect<&CharacterService::OnLocalIndexUpdated>(this);
    m_localUpdateConn = m_world.on_update<LocalComponent>().connect<&CharacterService::OnLocalIndexUpdated>(this);
    m_localDestroyConn = m_world.on_destroy<LocalComponent>().connect<&CharacterService::OnLocalIndexDestroyed>(this);

    m_remoteConstructConn = m_world.on_construct<RemoteComponent>().connect<&CharacterService::OnRemoteIndexUpdated>(this);
    m_remoteUpdateConn = m_world.on_update<RemoteComponent>().connect<&CharacterService::OnRemoteIndexUpdated>(this);
    m_remoteDestroyConn = m_world.on_destroy<RemoteComponent>().connect<&CharacterService::OnRemoteIndexDestroyed>(this);

    m_objectConstructConn = m_world.on_construct<ObjectComponent>().connect<&CharacterService::OnObjectIndexUpdated>(this);
    m_objectUpdateConn = m_world.on_update<ObjectComponent>().connect<&CharacterService::OnObjectIndexUpdated>(this);
    m_objectDestroyConn = m_world.on_destroy<ObjectComponent>().connect<&CharacterService::OnObjectIndexDestroyed>(this);
}

void CharacterService::SyncRemoteCachedInventory(uint32_t aServerId, const Inventory::Entry& acItem, bool aDrop) const noexcept
{
    auto applyEntry = [&](CharacterSpawnRequest& aSpawn) noexcept
    {
        Inventory::Entry adjusted = acItem;
        if (aDrop && adjusted.Count > 0)
            adjusted.Count = -adjusted.Count;
        aSpawn.InventoryContent.AddOrRemoveEntry(adjusted);
    };

    if (auto modelIt = m_remoteModels.find(aServerId); modelIt != m_remoteModels.end())
        applyEntry(modelIt.value().SpawnRequest);

    const entt::entity entity = FindEntityByServerId(aServerId);
    if (entity != entt::null)
    {
        if (auto* pWaiting = m_world.try_get<WaitingFor3D>(entity))
            applyEntry(pWaiting->SpawnRequest);
    }
}

void CharacterService::SyncRemoteCachedEquipment(const NotifyEquipmentChanges& acMessage) const noexcept
{
    const uint32_t equipSlotFormId = m_world.GetModSystem().GetGameId(acMessage.EquipSlotId);
    const TESForm* pEquipSlot = equipSlotFormId != 0 ? TESForm::GetById(equipSlotFormId) : nullptr;
    const bool isRightHand = (pEquipSlot != nullptr && pEquipSlot == DefaultObjectManager::Get().rightEquipSlot);
    const bool isLeftHand = (pEquipSlot != nullptr && pEquipSlot == DefaultObjectManager::Get().leftEquipSlot);

    auto applyEquip = [&](CharacterSpawnRequest& aSpawn) noexcept
    {
        auto& inv = aSpawn.InventoryContent;
        if (acMessage.IsShout)
        {
            inv.CurrentMagicEquipment.Shout = acMessage.Unequip ? GameId{} : acMessage.ItemId;
            return;
        }
        if (acMessage.IsSpell)
        {
            if (isRightHand)
                inv.CurrentMagicEquipment.RightHandSpell = acMessage.Unequip ? GameId{} : acMessage.ItemId;
            else
                inv.CurrentMagicEquipment.LeftHandSpell = acMessage.Unequip ? GameId{} : acMessage.ItemId;
            return;
        }

        bool matchedEntry = false;
        for (auto& entry : inv.Entries)
        {
            if (entry.BaseId != acMessage.ItemId)
                continue;
            matchedEntry = true;
            if (acMessage.Unequip)
            {
                if (isLeftHand)
                    entry.ExtraWornLeft = false;
                else if (isRightHand)
                    entry.ExtraWorn = false;
                else
                {
                    entry.ExtraWorn = false;
                    entry.ExtraWornLeft = false;
                }
            }
            else
            {
                if (isLeftHand)
                {
                    entry.ExtraWornLeft = true;
                    inv.CurrentMagicEquipment.LeftHandSpell = GameId{};
                }
                else
                {
                    entry.ExtraWorn = true;
                    if (isRightHand)
                        inv.CurrentMagicEquipment.RightHandSpell = GameId{};
                }
            }
            break;
        }

        if (!matchedEntry && !acMessage.Unequip && acMessage.ItemId != GameId{})
        {
            Inventory::Entry added{};
            added.BaseId = acMessage.ItemId;
            added.Count = std::max<int32_t>(1, static_cast<int32_t>(acMessage.Count));
            if (isLeftHand)
                added.ExtraWornLeft = true;
            else
                added.ExtraWorn = true;
            inv.Entries.push_back(std::move(added));
        }
    };

    if (auto modelIt = m_remoteModels.find(acMessage.ServerId); modelIt != m_remoteModels.end())
        applyEquip(modelIt.value().SpawnRequest);

    const entt::entity entity = FindEntityByServerId(acMessage.ServerId);
    if (entity != entt::null)
    {
        if (auto* pWaiting = m_world.try_get<WaitingFor3D>(entity))
            applyEquip(pWaiting->SpawnRequest);
    }
}

void CharacterService::SyncRemoteCachedHealthDelta(uint32_t aServerId, float aDeltaHealth) const noexcept
{
    auto applyHp = [&](CharacterSpawnRequest& aSpawn) noexcept
    {
        auto& avMap = aSpawn.InitialActorValues.ActorValuesList;
        const auto maxIt = aSpawn.InitialActorValues.ActorMaxValuesList.find(ActorValueInfo::kHealth);
        const float maxHp = (maxIt != aSpawn.InitialActorValues.ActorMaxValuesList.end()) ? maxIt->second : 0.0f;
        const auto curIt = avMap.find(ActorValueInfo::kHealth);
        const float baseHp = (curIt != avMap.end()) ? curIt->second : maxHp;
        float nextHp = baseHp + aDeltaHealth;
        if (aDeltaHealth > 0.0f && maxHp > 0.0f)
            nextHp = std::min(nextHp, maxHp);
        avMap[ActorValueInfo::kHealth] = nextHp;
        if (nextHp <= 0.0f && !aSpawn.IsPlayer)
            aSpawn.IsDead = true;
    };

    if (auto modelIt = m_remoteModels.find(aServerId); modelIt != m_remoteModels.end())
        applyHp(modelIt.value().SpawnRequest);

    const entt::entity entity = FindEntityByServerId(aServerId);
    if (entity != entt::null)
    {
        if (auto* pWaiting = m_world.try_get<WaitingFor3D>(entity))
            applyHp(pWaiting->SpawnRequest);
    }
}

void CharacterService::SyncRemoteCachedActorValues(
    uint32_t aServerId,
    const TiltedPhoques::Map<uint32_t, float>& acValues,
    bool aIsMaxValues) const noexcept
{
    auto applyAvs = [&](CharacterSpawnRequest& aSpawn) noexcept
    {
        auto& dst = aIsMaxValues ? aSpawn.InitialActorValues.ActorMaxValuesList : aSpawn.InitialActorValues.ActorValuesList;
        for (const auto& [k, v] : acValues)
            dst[k] = v;
    };

    if (auto modelIt = m_remoteModels.find(aServerId); modelIt != m_remoteModels.end())
        applyAvs(modelIt.value().SpawnRequest);

    const entt::entity entity = FindEntityByServerId(aServerId);
    if (entity != entt::null)
    {
        if (auto* pWaiting = m_world.try_get<WaitingFor3D>(entity))
            applyAvs(pWaiting->SpawnRequest);
    }
}

void CharacterService::SyncRemoteCachedDeathState(uint32_t aServerId, bool aIsDead) const noexcept
{
    if (auto modelIt = m_remoteModels.find(aServerId); modelIt != m_remoteModels.end())
        modelIt.value().SpawnRequest.IsDead = aIsDead;

    const entt::entity entity = FindEntityByServerId(aServerId);
    if (entity != entt::null)
    {
        if (auto* pWaiting = m_world.try_get<WaitingFor3D>(entity))
            pWaiting->SpawnRequest.IsDead = aIsDead;
    }
}

void CharacterService::SyncRemoteCachedWeaponDraw(uint32_t aServerId, bool aIsWeaponDrawn) const noexcept
{
    if (auto modelIt = m_remoteModels.find(aServerId); modelIt != m_remoteModels.end())
        modelIt.value().SpawnRequest.IsWeaponDrawn = aIsWeaponDrawn;

    const entt::entity entity = FindEntityByServerId(aServerId);
    if (entity != entt::null)
    {
        if (auto* pWaiting = m_world.try_get<WaitingFor3D>(entity))
            pWaiting->SpawnRequest.IsWeaponDrawn = aIsWeaponDrawn;
    }
}

const CharacterSpawnRequest& CharacterService::ResolveAuthoritativeSpawnState(
    entt::entity aEntity,
    const CharacterSpawnRequest& acFallback) const noexcept
{
    if (aEntity != entt::null && m_world.valid(aEntity))
    {
        if (const auto* pRemote = m_world.try_get<RemoteComponent>(aEntity))
        {
            if (const auto modelIt = m_remoteModels.find(pRemote->Id);
                modelIt != m_remoteModels.end() && modelIt->second.SpawnRequest.ServerId != 0)
            {
                return modelIt->second.SpawnRequest;
            }
        }
    }
    return acFallback;
}

void CharacterService::DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept
{
    m_world.remove<PlayerComponent>(aEntity);
    m_world.remove<FaceGenComponent, InterpolationComponent, RemoteAnimationComponent, RemoteComponent, CacheComponent, WaitingFor3D>(aEntity);
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
    if (!pExtension)
    {
        spdlog::error("Cannot take control over actor without extension, form id: {:X}", acFormId);
        return false;
    }

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
    if (!pActor->IsDead() && pActor->formID != 0x14)
        pActor->EvaluatePackage();

    const uint64_t effectiveEpoch = aOwnershipEpoch != 0 ? aOwnershipEpoch : m_ownershipEpochTracker.GetEpoch(acServerId);
    m_cachedRemoteSpawns.erase(acServerId);
    m_remoteModels.erase(acServerId);
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
    ActorExtension::EraseLatestAnimation(aFormId);

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
        if (auto* pPlayerExt = pActor->GetExtension())
            pPlayerExt->SetPlayer(true);
    }

    entt::entity entity = entt::null;

    if (const entt::entity existingActor = FindActorEntityByFormId(acEvent.FormId); existingActor != entt::null)
    {
        entity = existingActor;
        if (m_world.all_of<RemoteComponent>(entity) && pActor && pActor->GetExtension())
            pActor->GetExtension()->SetRemote(true);
    }
    else
    {
        const auto view = m_world.view<RemoteComponent>();
        for (entt::entity candidate : view)
        {
            if (view.get<RemoteComponent>(candidate).CachedRefId == acEvent.FormId)
            {
                if (pActor && pActor->GetExtension())
                    pActor->GetExtension()->SetRemote(true);
                entity = candidate;
                break;
            }
        }

        if (entity == entt::null)
            entity = m_world.create();
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
    const entt::entity cId = FindActorEntityByFormId(acEvent.FormId);
    if (cId == entt::null)
    {
        if ((acEvent.FormId & 0xFF000000) == 0xFF000000)
            spdlog::debug("Dynamic actor {:X} already removed from form ids map", acEvent.FormId);
        else
            spdlog::error("Actor to remove not found in form ids map {:X}", acEvent.FormId);
        return;
    }

    m_pendingLocalTempSpawns.erase(cId);

    // SkyMP FormView Isolation:
    // DiscoveryService::VisitForms() fires ActorRemovedEvent whenever GetNiNode() blinks null for 1 frame
    // (e.g. during armor equip, facegen rebuild, or exterior cell-seam crossing).
    // For RemoteComponent entities backed by RemoteEntityModel, NEVER destroy a live network-created FormView
    // on a transient NiNode flicker while its WorldOrCell is still compatible with our loaded cells, and if
    // it genuinely left our loaded space, dematerialize ONLY its 3D FormView while preserving its WorldModel!
    if (const auto* pRemoteComp = m_world.try_get<RemoteComponent>(cId))
    {
        const uint32_t serverId = pRemoteComp->Id;
        auto modelIt = m_remoteModels.find(serverId);
        if (modelIt != m_remoteModels.end())
        {
            Actor* pLiveActor = Cast<Actor>(TESForm::GetById(acEvent.FormId));
            const bool isNetworkCreated = modelIt->second.SpawnRequest.FormId == GameId{} || IsNetworkCreatedActor(pLiveActor);
            if (isNetworkCreated && pLiveActor && !m_savePurgeActive && !m_engineWorldLoadInProgress)
            {
                const auto obs = BuildLocalObserverState();
                TESWorldSpace* pTargetWs = nullptr;
                TESObjectCELL* pTargetCell = obs.EngineReady ? ResolveCompatibleTargetCell(modelIt->second.Location, pTargetWs) : nullptr;

                CoopWorldView::RemoteViewQuery query{};
                query.Location = modelIt->second.Location;
                query.PosX = modelIt->second.LatestPosition.x;
                query.PosY = modelIt->second.LatestPosition.y;
                query.IsDragon = modelIt->second.SpawnRequest.IsDragon;
                query.IsPlayer = modelIt->second.SpawnRequest.IsPlayer;
                query.CurrentlyMaterialized = true;
                query.TargetCellLoadedInEngine = (pTargetCell != nullptr);

                if (CoopWorldView::EvaluateViewDecision(obs, query) == CoopWorldView::ViewDecision::kKeepMaterialized)
                {
                    spdlog::debug("Ignoring transient NiNode unload on active remote FormView {:X} (serverId {:X})", acEvent.FormId, serverId);
                    return;
                }
            }

            DematerializeRemoteView(cId, &modelIt.value());
            spdlog::info("Dematerialized remote FormView {:X} (serverId {:X})", acEvent.FormId, serverId);
            return;
        }
    }

    const auto* pRemovedForm = m_world.try_get<FormIdComponent>(cId);
    const uint32_t removedFormId = pRemovedForm ? pRemovedForm->Id : acEvent.FormId;
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
    if (m_transport.IsOnline())
    {
        static std::chrono::steady_clock::time_point s_lastLocalPlayerCheck{};
        const auto now = std::chrono::steady_clock::now();
        if (now - s_lastLocalPlayerCheck >= 500ms)
        {
            s_lastLocalPlayerCheck = now;
            EnsureLocalPlayerRegistered();
        }
    }
    RunLocalUpdates();
    RunFactionsUpdates();
    if (!m_savePurgeActive && m_pendingPostSaveRemoteRestore)
        RestorePurgedRemoteActorsAfterSave();

    RunRemoteUpdates();
    RunExperienceUpdates();
    RunRfabExperienceUpdates();
    ApplyCachedWeaponDraws(acUpdateEvent);
}

void CharacterService::EnsureLocalPlayerRegistered() noexcept
{
    if (m_engineWorldLoadInProgress || m_savePurgeActive)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || pPlayer->formID != 0x14 || pPlayer->IsStillLoading() || !pPlayer->parentCell)
        return;

    if (auto* pExt = pPlayer->GetExtension())
    {
        pExt->SetPlayer(true);
        pExt->SetRemote(false);
    }

    entt::entity playerEntity = entt::null;
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (entt::entity candidate : view)
    {
        if (view.get<FormIdComponent>(candidate).Id == 0x14)
        {
            playerEntity = candidate;
            break;
        }
    }

    if (playerEntity == entt::null)
    {
        playerEntity = m_world.create();
        m_world.emplace_or_replace<FormIdComponent>(playerEntity, 0x14u);
        m_world.emplace_or_replace<EarlyAnimationBufferComponent>(playerEntity);
        spdlog::info("[RFAB SaveLoad] Re-registered local PlayerCharacter (0x14) entity after world load.");
    }

    if (m_transport.IsOnline() &&
        !m_world.all_of<LocalComponent>(playerEntity) &&
        !m_world.all_of<WaitingForAssignmentComponent>(playerEntity))
    {
        ProcessNewEntity(playerEntity);
    }
}

void CharacterService::OnConnected(const ConnectedEvent& acConnectedEvent) noexcept
{
    m_savePurgeActive = false;
    m_engineWorldLoadInProgress = false;
    m_pendingPostSaveRemoteRestore = false;
    m_pendingLocalTempSpawns.clear();
    m_cachedRemoteSpawns.clear();
    m_remoteModels.clear();
    m_playerServerIds.clear();
    NotifySaveLoadReset();

    EnsureLocalPlayerRegistered();

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
    ActorExtension::ClearAllLatestAnimations();

    if (m_engineWorldLoadInProgress)
        return;

    PurgeNetworkActorsOnLiveDisconnect();
}

void CharacterService::BeginEngineWorldLoad() noexcept
{
    m_engineWorldLoadInProgress = true;
    GetDeferredDialogueDeletes().clear();
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

    // Ensure every pooled 0xFFxxxxxx remote player TESNPC base form is excluded from BGSSaveLoadGame!
    for (const auto& [playerId, npcFormId] : m_playerBaseNpcPool)
    {
        (void)playerId;
        if ((npcFormId & 0xFF000000u) != 0xFF000000u)
            continue;
        if (TESForm* pBaseForm = TESForm::GetById(npcFormId))
        {
            pBaseForm->SetSkipSaveFlag(true);
            pBaseForm->UnsetChanged(0xFFFFFFFF);
        }
    }

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
    // Also sweep every 0xFF00xxxx Actor & pooled TESNPC ever created by the network in this world session
    // (including any recently Delete()-queued refs waiting in BSTaskPool) so zero dynamic clones can bake into .ess!
    for (const uint32_t dynFormId : m_createdNetworkFormIds)
    {
        if (dynFormId == 0 || dynFormId == 0x14 || (dynFormId & 0xFF000000u) != 0xFF000000u)
            continue;
        if (TESForm* pDynForm = TESForm::GetById(dynFormId))
        {
            pDynForm->SetSkipSaveFlag(true);
            pDynForm->UnsetChanged(0xFFFFFFFF);
        }
    }

    for (const auto& [_, pooledNpcId] : m_playerBaseNpcPool)
    {
        if (pooledNpcId == 0 || (pooledNpcId & 0xFF000000u) != 0xFF000000u)
            continue;
        if (TESForm* pNpcForm = TESForm::GetById(pooledNpcId))
        {
            pNpcForm->SetSkipSaveFlag(true);
            pNpcForm->UnsetChanged(0xFFFFFFFF);
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
    m_remoteModels.clear();
    m_playerServerIds.clear();
    m_playerBaseNpcPool.clear();
    m_createdNetworkFormIds.clear();
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

    m_entityIndex.Clear();
    m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
}

void CharacterService::PurgeNetworkActorsOnLiveDisconnect() noexcept
{
    m_savePurgeActive = false;
    m_pendingPostSaveRemoteRestore = false;
    m_pendingLocalTempSpawns.clear();
    m_cachedRemoteSpawns.clear();
    m_remoteModels.clear();
    m_playerServerIds.clear();
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
    m_rfabXpTracker.Reset();
    m_ownershipEpochTracker.Clear();
    m_cancelledCookies.Clear();
    m_pendingRemoteMounts.clear();
    m_pendingLocalMount = {};
}

void CharacterService::NotifyInSessionRollbackApplied() noexcept
{
    auto interpView = m_world.view<InterpolationComponent>();
    for (const entt::entity entity : interpView)
    {
        auto& interp = interpView.get<InterpolationComponent>(entity);
        interp.TimePoints.clear();
        interp.Clock.Reset();
    }

    for (auto it = m_remoteModels.begin(); it != m_remoteModels.end(); ++it)
        it.value().DematerializeGraceUntilMs = 0;

    ReconcileRemoteWorldViews();
}

void CharacterService::OnAssignCharacter(const AssignCharacterResponse& acMessage) noexcept
{
    spdlog::info(__FUNCTION__ ": Received for cookie {:X}, server id {:X}", acMessage.Cookie, acMessage.ServerId);

    auto view = m_world.view<WaitingForAssignmentComponent>();
    entt::entity cEntity = entt::null;
    for (entt::entity candidate : view)
    {
        if (view.get<WaitingForAssignmentComponent>(candidate).Cookie == acMessage.Cookie)
        {
            cEntity = candidate;
            break;
        }
    }

    if (cEntity == entt::null)
    {
        if (m_cancelledCookies.ConsumeIfCancelled(acMessage.Cookie))
            spdlog::debug("Ignored AssignCharacterResponse for cancelled cookie {:X} (server id {:X})", acMessage.Cookie, acMessage.ServerId);
        else
            spdlog::warn("Never found requested cookie: {}", acMessage.Cookie);
        return;
    }

    const auto waitingSnapshot = view.get<WaitingForAssignmentComponent>(cEntity);

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

        if (const auto modelIt = m_remoteModels.find(acMessage.ServerId);
            modelIt != m_remoteModels.end() && acMessage.PlayerId == 0)
        {
            // NEVER re-template a live, locally owned actor here. RFAB resolves `TESLevCharacter` placed
            // references into its own dynamic leveled templates (`0xFF00xxxx`), so the server-side cached
            // `BaseId` is by definition older than the live base form. Forcing it back used to run
            // `DisableImpl()` -> `SetBaseForm()` -> `EnableImpl()`, which made the engine drop the NiNode,
            // which `DiscoveryService` reported as `ActorRemovedEvent`, which triggered
            // `CancelServerAssignment()` -> ownership transfer to the server -> the NPC lost its local AI
            // (it "ignored" the player) and had its equipment re-applied from stale spawn data (it fought
            // with a non-primary weapon) until the next assignment round.
            // The owner client is authoritative for its own actors: keep the live base form. The wire format is
            // deliberately left untouched (`ActorData` carries no base id), because RFAB's leveled templates live
            // in the dynamic `0xFF00xxxx` range and `ModSystem::GetServerModId` would map them onto an unrelated
            // static form on the receiving side. Full template parity for peers is the v2.0.4 backlog item.
            const uint32_t cExpectedBaseId = m_world.GetModSystem().GetGameId(modelIt->second.SpawnRequest.BaseId);
            if (cExpectedBaseId != 0 && pActor->baseForm && pActor->baseForm->formID != cExpectedBaseId)
            {
                spdlog::debug(
                    "[RFAB LeveledSync] keeping owner-authoritative base form {:X} on locally owned actor {:X} "
                    "(server cached {:X}); publishing it upwards instead of re-templating the live reference",
                    pActor->baseForm->formID,
                    pActor->formID,
                    cExpectedBaseId);
            }

            m_remoteModels.erase(modelIt);
        }
        m_cachedRemoteSpawns.erase(acMessage.ServerId);

        const bool serverProvidedExistingState =
            (acMessage.PlayerId == 0 && !acMessage.AllActorValues.ActorValuesList.empty());

        // When taking ownership of an actor already tracked on the server (e.g. a party leader
        // entering a dungeon room where another party member already damaged, looted, or killed an NPC),
        // apply the server's authoritative state BEFORE constructing LocalComponent so we never
        // heal or resurrect wounded/dead enemies!
        if (serverProvidedExistingState)
        {
            // Equip inventory only once 3D is ready; if 3D is still streaming, stage WaitingFor3D so
            // EquipManager::EquipItem never runs on a half-constructed actor (SkyrimSE.exe+0x1133D8 / 0x65D2CB).
            if (pActor->GetNiNode() && !pActor->IsStillLoading())
            {
                pActor->SetActorInventory(acMessage.CurrentInventory);
            }
            else
            {
                CharacterSpawnRequest pending3D{};
                pending3D.ServerId = acMessage.ServerId;
                pending3D.InventoryContent = acMessage.CurrentInventory;
                pending3D.InitialActorValues = acMessage.AllActorValues;
                pending3D.IsDead = acMessage.IsDead;
                pending3D.IsWeaponDrawn = acMessage.IsWeaponDrawn;
                m_world.emplace_or_replace<WaitingFor3D>(cEntity, pending3D);
            }
            pActor->SetActorValues(acMessage.AllActorValues);

            if (pActor->IsDead() != acMessage.IsDead)
            {
                if (acMessage.IsDead)
                {
                    KillActorSuppressingDuplicateXp(pActor);
                }
                else
                {
                    RequestDeathStateChange deathSync{};
                    deathSync.Id = acMessage.ServerId;
                    deathSync.IsDead = true;
                    m_transport.Send(deathSync);
                }
            }

            m_weaponDrawUpdates[pActor->formID] = {acMessage.IsWeaponDrawn};
            MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);
        }
        else
        {
            m_weaponDrawUpdates.erase(pActor->formID);
        }

        m_world.emplace_or_replace<LocalComponent>(cEntity, acMessage.ServerId, acMessage.OwnershipEpoch);
        auto& localAnimationComponent = m_world.emplace_or_replace<LocalAnimationComponent>(cEntity);

        pActor->GetExtension()->SetRemote(false);
        SanitizeActorCombatStateForLocalOwnership(pActor);
        if (!pActor->IsDead() && pActor->formID != 0x14)
            pActor->EvaluatePackage();

        if (auto* pEarlyAnimComponent = m_world.try_get<EarlyAnimationBufferComponent>(cEntity))
        {
            for (const auto& action : pEarlyAnimComponent->Actions)
            {
                localAnimationComponent.Append(action);
            }
        }
        m_world.remove<EarlyAnimationBufferComponent>(cEntity);

        // Flush mutations that occurred during the `WaitingForAssignment` RTT window (#932):
        // if an NPC finished equipping its outfit/weapons while `AssignCharacterRequest` was in flight,
        // push the updated `ActorData` immediately so peers never see naked NPCs!
        if (!serverProvidedExistingState && waitingSnapshot.DirtyInventory)
        {
            RequestOwnershipClaim syncClaim{};
            syncClaim.ServerId = acMessage.ServerId;
            syncClaim.NewActorData = BuildActorData(pActor);
            m_transport.Send(syncClaim);
        }
        else if (std::fabs(waitingSnapshot.PendingHealthDelta) > 0.05f)
        {
            RequestHealthChangeBroadcast hpSync{};
            hpSync.Id = acMessage.ServerId;
            hpSync.DeltaHealth = waitingSnapshot.PendingHealthDelta;
            m_transport.Send(hpSync);
        }
    }
    else
    {
        m_ownershipEpochTracker.AcceptRelinquish(acMessage.ServerId, acMessage.OwnershipEpoch);
        spdlog::info(__FUNCTION__ ": received remote actor, form id: {:X}, isweapondrawn: {}, {}", pActor->formID, acMessage.IsWeaponDrawn, pActor->baseForm ? pActor->baseForm->GetName() : "<null>");

        if (const auto modelIt = m_remoteModels.find(acMessage.ServerId);
            modelIt != m_remoteModels.end() && acMessage.PlayerId == 0)
        {
            (void)ReconcilePlacedActorBaseFormInPlace(pActor, modelIt->second.SpawnRequest.BaseId);
        }

        m_world.emplace_or_replace<RemoteComponent>(cEntity, acMessage.ServerId, formIdComponent->Id);

        pActor->GetExtension()->SetRemote(true);

        m_world.remove<EarlyAnimationBufferComponent>(cEntity);
        InterpolationSystem::Setup(m_world, cEntity).Position = pActor->position;
        AnimationSystem::Setup(m_world, cEntity);
        AnimationSystem::AddActionsForReplay(m_world.get<RemoteAnimationComponent>(cEntity), acMessage.ActionsToReplay);

#if (!IS_MASTER)
        m_world.emplace_or_replace<ReplayedActionsDebugComponent>(cEntity, acMessage.ActionsToReplay);
#endif

        // Equip inventory immediately only if 3D is already loaded; otherwise WaitingFor3D applies it once GetNiNode() != nullptr.
        if (pActor->GetNiNode() && !pActor->IsStillLoading())
            pActor->SetActorInventory(acMessage.CurrentInventory);
        pActor->SetActorValues(acMessage.AllActorValues);

        if (pActor->IsDead() != acMessage.IsDead)
            acMessage.IsDead ? KillActorSuppressingDuplicateXp(pActor) : pActor->ResurrectInPlace();

        // Preserve opening sneak/arrow hits dealt while `AssignCharacterRequest` was in flight!
        if (std::fabs(waitingSnapshot.PendingHealthDelta) > 0.05f && !acMessage.IsDead)
        {
            RequestHealthChangeBroadcast hpSync{};
            hpSync.Id = acMessage.ServerId;
            hpSync.DeltaHealth = waitingSnapshot.PendingHealthDelta;
            m_transport.Send(hpSync);
        }

        m_weaponDrawUpdates[pActor->formID] = {acMessage.IsWeaponDrawn};

        auto& model = m_remoteModels[acMessage.ServerId];
        m_world.GetModSystem().GetServerModId(pActor->formID, model.SpawnRequest.FormId);
        // Preserve the creator/server's authoritative `BaseId` if `CharacterSpawnRequest` already arrived;
        // only fall back to local resolved static `TESNPC` when `model.SpawnRequest.BaseId` is still empty.
        if (model.SpawnRequest.BaseId == GameId{})
        {
            if (TESNPC* pResolvedBase = pActor->GetLeveledTemplateBase(); pResolvedBase && !pResolvedBase->IsTemporary())
                m_world.GetModSystem().GetServerModId(pResolvedBase->formID, model.SpawnRequest.BaseId);
            if (model.SpawnRequest.BaseId.ModId == std::numeric_limits<uint32_t>::max())
                model.SpawnRequest.BaseId = GameId{};
        }
        model.SpawnRequest.ServerId = acMessage.ServerId;
        model.SpawnRequest.CellId = acMessage.CellId;
        model.SpawnRequest.WorldSpaceId = acMessage.WorldSpaceId;
        model.SpawnRequest.Position = acMessage.Position;
        model.SpawnRequest.InitialActorValues = acMessage.AllActorValues;
        model.SpawnRequest.InventoryContent = acMessage.CurrentInventory;
        model.SpawnRequest.ActionsToReplay = acMessage.ActionsToReplay;
        model.SpawnRequest.PlayerId = acMessage.PlayerId;
        model.SpawnRequest.IsDead = acMessage.IsDead;
        model.SpawnRequest.IsWeaponDrawn = acMessage.IsWeaponDrawn;
        model.SpawnRequest.IsDragon = pActor->IsDragon();
        model.SpawnRequest.IsPlayerSummon = pActor->IsPlayerSummon();
        model.Location = CoopWorldView::WorldOrCell::FromPacket(
            acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position.x, acMessage.Position.y);
        model.LatestPosition = acMessage.Position;
        model.LatestRotation.x = pActor->rotation.x;
        model.LatestRotation.y = pActor->rotation.z;
        model.IsMaterialized = true;

        if (!pActor->GetNiNode())
            m_world.emplace_or_replace<WaitingFor3D>(cEntity, model.SpawnRequest);

        if (!MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position))
        {
            DematerializeRemoteView(cEntity, &model);
            return;
        }
    }
    pActor->GetExtension()->SetNakedDeadline();
}

void CharacterService::OnCharacterSpawn(const CharacterSpawnRequest& acMessage) const noexcept
{
    if (m_engineWorldLoadInProgress)
        return;

    if (acMessage.IsPlayer && acMessage.PlayerId != 0)
    {
        if (const auto prevPlayerIt = m_playerServerIds.find(acMessage.PlayerId);
            prevPlayerIt != m_playerServerIds.end() && prevPlayerIt->second != acMessage.ServerId)
        {
            NotifyRemoveCharacter cleanupMsg{};
            cleanupMsg.ServerId = prevPlayerIt->second;
            OnRemoveCharacter(cleanupMsg);
        }
        m_playerServerIds[acMessage.PlayerId] = acMessage.ServerId;
    }

    const CoopWorldView::WorldOrCell incomingLoc = CoopWorldView::WorldOrCell::FromPacket(
        acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position.x, acMessage.Position.y);

    auto& model = m_remoteModels[acMessage.ServerId];
    const CoopWorldView::WorldOrCell previousLoc = model.Location;
    const Vector3_NetQuantize previousPos = model.LatestPosition;
    const bool hadPreviousLoc = previousLoc.IsValid();

    model.SpawnRequest = acMessage;
    model.HasAuthoritativeSpawnPacket = true;
    model.Location = incomingLoc;
    model.LatestPosition = acMessage.Position;
    model.LatestRotation = acMessage.Rotation;
    m_cachedRemoteSpawns[acMessage.ServerId] = acMessage;

    entt::entity entity = FindEntityByServerId(acMessage.ServerId);
    if (entity != entt::null && !m_world.all_of<RemoteComponent>(entity))
        entity = entt::null;

    if (entity == entt::null)
    {
        if (acMessage.FormId != GameId{})
        {
            const uint32_t cActorId = World::Get().GetModSystem().GetGameId(acMessage.FormId);
            if (cActorId == 0)
            {
                m_remoteModels.erase(acMessage.ServerId);
                m_cachedRemoteSpawns.erase(acMessage.ServerId);
                return;
            }

            const entt::entity existingByForm = FindActorEntityByFormId(cActorId);
            if (existingByForm != entt::null && m_world.all_of<WaitingForAssignmentComponent>(existingByForm))
            {
                spdlog::info("Character with form id {:X} already has an assignment request in progress; cached WorldModel {:X}.", cActorId, acMessage.ServerId);
                return;
            }

            if (existingByForm != entt::null && !m_world.any_of<LocalComponent, RemoteComponent>(existingByForm))
            {
                entity = existingByForm;
                m_pendingLocalTempSpawns.erase(entity);
                m_world.remove<FormIdComponent, EarlyAnimationBufferComponent>(entity);
            }
        }

        if (entity == entt::null)
            entity = m_world.create();

        m_world.emplace_or_replace<RemoteComponent>(entity, acMessage.ServerId, 0u);
        auto& interp = InterpolationSystem::Setup(m_world, entity);
        interp.Position = acMessage.Position;
        AnimationSystem::Setup(m_world, entity);
    }

    if (auto* pInterp = m_world.try_get<InterpolationComponent>(entity))
    {
        const float dx = acMessage.Position.x - previousPos.x;
        const float dy = acMessage.Position.y - previousPos.y;
        const float dz = acMessage.Position.z - previousPos.z;
        const bool jumped = !hadPreviousLoc || !previousLoc.IsSameSpace(incomingLoc) ||
                            ((dx * dx + dy * dy + dz * dz) > CoopWorldView::kCrossPositionSnapDistSq);
        if (jumped)
            pInterp->TimePoints.clear();
        pInterp->Position = acMessage.Position;
    }

    if (auto* pAnim = m_world.try_get<RemoteAnimationComponent>(entity))
    {
        if (!m_world.all_of<FormIdComponent>(entity))
        {
            pAnim->TimePoints.clear();
            AnimationSystem::AddActionsForReplay(*pAnim, acMessage.ActionsToReplay);
        }
    }

#if (!IS_MASTER)
    m_world.emplace_or_replace<ReplayedActionsDebugComponent>(entity, acMessage.ActionsToReplay);
#endif

    // If FormView is already materialized, reconcile in-place without Delete()/Create() churn!
    if (const auto* pFormComp = m_world.try_get<FormIdComponent>(entity))
    {
        const uint64_t nowMs = NowSteadyWorldViewMs();
        if (acMessage.IsPlayer && hadPreviousLoc && !previousLoc.IsSameSpace(incomingLoc))
            model.DematerializeGraceUntilMs = nowMs + CoopWorldView::kRemotePlayerDoorTransitionGraceMs;

        Actor* pLiveActor = Cast<Actor>(TESForm::GetById(pFormComp->Id));
        const auto obs = BuildLocalObserverState();
        TESWorldSpace* pTargetWs = nullptr;
        TESObjectCELL* pTargetCell = obs.EngineReady
            ? ResolveCompatibleTargetCell(model.Location, pTargetWs, acMessage.IsDragon)
            : nullptr;

        CoopWorldView::RemoteViewQuery query{};
        query.Location = model.Location;
        query.PosX = model.LatestPosition.x;
        query.PosY = model.LatestPosition.y;
        query.IsDragon = acMessage.IsDragon;
        query.IsPlayer = acMessage.IsPlayer;
        query.CurrentlyMaterialized = (pLiveActor != nullptr);
        query.TargetCellLoadedInEngine = (pTargetCell != nullptr);
        query.NowSteadyMs = nowMs;
        query.DematerializeGraceUntilMs = model.DematerializeGraceUntilMs;

        if (!pLiveActor || CoopWorldView::EvaluateViewDecision(obs, query) != CoopWorldView::ViewDecision::kKeepMaterialized)
        {
            DematerializeRemoteView(entity, &model);
        }
        else
        {
            if (acMessage.IsPlayer && acMessage.BaseId == GameId{} && pLiveActor->baseForm && !acMessage.AppearanceBuffer.empty())
            {
                if (TESNPC* pBaseNpc = Cast<TESNPC>(pLiveActor->baseForm);
                    pBaseNpc && ((pBaseNpc->formID & 0xFF000000u) == 0xFF000000u))
                {
                    pBaseNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
                    pBaseNpc->overlayRace = nullptr;
                    pBaseNpc->actorData.SetEssential(true);
                    pBaseNpc->SetSkipSaveFlag(true);
                    pBaseNpc->UnsetChanged(0xFFFFFFFF);
                    if (auto* pFaceGen = m_world.try_get<FaceGenComponent>(entity))
                    {
                        pFaceGen->Generated = false;
                        pFaceGen->LastFaceNode = nullptr;
                    }
                    pLiveActor->QueueUpdate();
                }
            }

            if (!acMessage.IsPlayer)
                (void)ReconcilePlacedActorBaseFormInPlace(pLiveActor, acMessage.BaseId);

            pLiveActor->SetActorValues(acMessage.InitialActorValues);
            m_world.emplace_or_replace<WaitingFor3D>(entity, acMessage);
            m_weaponDrawUpdates[pLiveActor->formID] = {acMessage.IsWeaponDrawn};
            MoveActor(pLiveActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position);
            return;
        }
    }

    ReconcileRemoteWorldViews();
}

void CharacterService::OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) const noexcept
{
    if (!m_ownershipEpochTracker.AcceptGrant(acMessage.ServerId, acMessage.OwnershipEpoch))
    {
        spdlog::debug("Ignored stale NotifyOwnershipTransfer for {:X} (epoch {})", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    const entt::entity entity = FindEntityByServerId(acMessage.ServerId);
    if (entity != entt::null && m_world.all_of<RemoteComponent, FormIdComponent>(entity))
    {
        auto& formIdComponent = m_world.get<FormIdComponent>(entity);

        if (TakeOwnership(formIdComponent.Id, acMessage.ServerId, entity, acMessage.OwnershipEpoch))
        {
            spdlog::info("Ownership claimed {:X} (epoch {})", acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }
    }

    // Two-phase dormant wake-up: if `RemoteEntityModel` already exists (queued behind the 4/frame spawn budget
    // or waiting for `TESForm::IsStillLoading()` / cell `loadedCellData` to finish streaming), stage
    // `PendingOwnershipEpoch` so `MaterializeRemoteView` promotes it to `TakeOwnership` on the very frame it
    // materializes — NEVER prematurely decline and strand the NPC in `Owner == nullptr` dormant limbo!
    if (auto modelIt = m_remoteModels.find(acMessage.ServerId); modelIt != m_remoteModels.end())
    {
        modelIt.value().PendingOwnershipEpoch = acMessage.OwnershipEpoch;
        spdlog::info(
            "Deferred ownership claim for queued WorldModel {:X} (epoch {}) until FormView materializes",
            acMessage.ServerId,
            acMessage.OwnershipEpoch);
        ReconcileRemoteWorldViews();
        return;
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

    bool isNetworkModel = false;
    if (auto modelIt = m_remoteModels.find(acMessage.ServerId); modelIt != m_remoteModels.end())
    {
        isNetworkModel = (modelIt->second.SpawnRequest.FormId == GameId{});
        if (modelIt->second.SpawnRequest.IsPlayer && modelIt->second.SpawnRequest.PlayerId != 0)
        {
            if (auto playerIt = m_playerServerIds.find(modelIt->second.SpawnRequest.PlayerId);
                playerIt != m_playerServerIds.end() && playerIt->second == acMessage.ServerId)
            {
                m_playerServerIds.erase(playerIt);
            }
        }
        m_remoteModels.erase(modelIt);
    }

    const entt::entity entity = FindEntityByServerId(acMessage.ServerId);
    if (entity != entt::null && m_world.all_of<RemoteComponent>(entity))
    {
        bool removedNetworkActor = isNetworkModel;
        Actor* pLivePlacedActor = nullptr;
        if (auto* pFormIdComponent = m_world.try_get<FormIdComponent>(entity))
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));
            removedNetworkActor = removedNetworkActor || IsNetworkCreatedActor(pActor);
            if (!removedNetworkActor)
                pLivePlacedActor = pActor;
            CharacterService::DeleteTempActor(pFormIdComponent->Id);
        }
        else if (const uint32_t cachedRefId = m_world.get<RemoteComponent>(entity).CachedRefId; cachedRefId != 0)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(cachedRefId));
            removedNetworkActor = removedNetworkActor || IsNetworkCreatedActor(pActor);
            CharacterService::DeleteTempActor(cachedRefId);
        }

        DeleteRemoteEntityComponents(entity);
        if (removedNetworkActor)
        {
            m_world.remove<FormIdComponent, EarlyAnimationBufferComponent>(entity);
            if (m_world.orphan(entity))
                m_world.destroy(entity);
        }
        else if (pLivePlacedActor &&
                 pLivePlacedActor->parentCell &&
                 pLivePlacedActor->parentCell->loadedCellData &&
                 m_world.all_of<FormIdComponent>(entity))
        {
            // Placed `0x00..0xFE` actor is still loaded in the local player's active cell (e.g. server pruned
            // an old dormant record): immediately re-register locally so it never becomes an unsynced ghost!
            ProcessNewEntity(entity);
        }
        else if (m_world.orphan(entity))
        {
            m_world.destroy(entity);
        }
    }
}

void CharacterService::OnNotifyRelinquishControl(const NotifyRelinquishControl& acMessage) noexcept
{
    if (!m_ownershipEpochTracker.AcceptRelinquish(acMessage.ServerId, acMessage.OwnershipEpoch))
    {
        spdlog::debug("Ignored stale NotifyRelinquishControl for {:X} (epoch {})", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    // O(1) server-id lookup through the entity index.
    // The previous implementation walked every `FormIdComponent` in the registry and logged an error for each
    // entity that has no `Local`/`RemoteComponent` (the vast majority: the player, objects, waiting entities).
    // A single relinquish therefore produced dozens of bogus "failed to find server id for entity" errors and
    // still found the actor afterwards, which made real desyncs impossible to spot in the logs.
    const entt::entity entity = FindEntityByServerId(acMessage.ServerId);
    if (entity == entt::null)
    {
        // The entity can legitimately be gone already: the actor left the cell (cell purge) or was unloaded while
        // the server was still revoking our ownership. There is nothing left to hand back, so this is diagnostics,
        // not an error: keep it at debug level so genuine ownership desyncs stay visible in the logs.
        spdlog::debug(
            __FUNCTION__ ": no tracked entity for server id {:X} to relinquish (actor already unloaded)",
            acMessage.ServerId);
        return;
    }

    const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(entity);
    if (!pFormIdComponent)
    {
        spdlog::debug(__FUNCTION__ ": entity for server id {:X} has no form id component", acMessage.ServerId);
        return;
    }

    const uint32_t actorFormId = pFormIdComponent->Id;
    const uint32_t serverId = acMessage.ServerId;

    if (m_world.all_of<LocalComponent>(entity))
    {
        m_world.remove<LocalAnimationComponent, LocalComponent>(entity);
        m_world.emplace_or_replace<RemoteComponent>(entity, acMessage.ServerId, actorFormId);
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(actorFormId));
    if (!pActor)
    {
        // Probably left the room and/or temporary.
        spdlog::info(__FUNCTION__ ": no local Actor for serverId {:X} to relinquish", serverId);
        return;
    }

    if (auto* pRelinquishExt = pActor->GetExtension())
        pRelinquishExt->SetRemote(true);

    auto& model = m_remoteModels[acMessage.ServerId];
    m_world.GetModSystem().GetServerModId(pActor->formID, model.SpawnRequest.FormId);
    if (model.SpawnRequest.BaseId == GameId{})
    {
        if (TESNPC* pResolvedBase = pActor->GetLeveledTemplateBase(); pResolvedBase && !pResolvedBase->IsTemporary())
            m_world.GetModSystem().GetServerModId(pResolvedBase->formID, model.SpawnRequest.BaseId);
        if (model.SpawnRequest.BaseId.ModId == std::numeric_limits<uint32_t>::max())
            model.SpawnRequest.BaseId = GameId{};
    }
    model.SpawnRequest.ServerId = acMessage.ServerId;
    if (TESWorldSpace* pWs = pActor->GetWorldSpace())
        m_world.GetModSystem().GetServerModId(pWs->formID, model.SpawnRequest.WorldSpaceId);
    else
        model.SpawnRequest.WorldSpaceId = GameId{};
    if (pActor->parentCell)
        m_world.GetModSystem().GetServerModId(pActor->parentCell->formID, model.SpawnRequest.CellId);
    model.SpawnRequest.Position = pActor->position;
    model.SpawnRequest.IsDragon = pActor->IsDragon();
    model.SpawnRequest.IsPlayerSummon = pActor->IsPlayerSummon();
    model.Location = CoopWorldView::WorldOrCell::FromPacket(
        model.SpawnRequest.WorldSpaceId,
        model.SpawnRequest.CellId,
        pActor->position.x,
        pActor->position.y);
    model.LatestPosition = pActor->position;
    model.LatestRotation.x = pActor->rotation.x;
    model.LatestRotation.y = pActor->rotation.z;
    model.IsMaterialized = true;

    auto& interp = InterpolationSystem::Setup(m_world, entity);
    interp.Position = pActor->position;
    AnimationSystem::Setup(m_world, entity);

    spdlog::info(__FUNCTION__ ": relinquished control of actor {:X} with server id {:X}", pActor->formID, acMessage.ServerId);
}

bool CharacterService::MoveActor(const Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) const noexcept
{
    if (!apActor || apActor->formID == 0x14 || apActor->IsStillLoading() || m_engineWorldLoadInProgress || m_savePurgeActive)
        return false;

    PlayerCharacter* pLocalPlayer = PlayerCharacter::Get();
    if (!pLocalPlayer || pLocalPlayer->IsStillLoading() || !pLocalPlayer->parentCell)
        return false;

    TESObjectCELL* pCell = nullptr;
    if (!acWorldSpaceId)
    {
        const uint32_t cCellId = m_world.GetModSystem().GetGameId(acCellId);
        pCell = Cast<TESObjectCELL>(TESForm::GetById(cCellId));
        if (!pCell || !pCell->loadedCellData || pLocalPlayer->GetWorldSpace() != nullptr || pLocalPlayer->GetParentCell() != pCell)
            return false;
    }
    else
    {
        // CRITICAL FOR SKYRIM SE 1.5.97 (+0x23D000 QueuedPromoteLargeReferencesTask / GetWorldSpaceOfExteriorCell):
        // NEVER invoke TESWorldSpace::LoadCell(X, Y) for an NPC! Synchronously force-loading exterior cells outside
        // the local player's active 5x5 uGridsToLoad corrupts background large-reference promotion tasks when Skyrim
        // purges detached cells on the next frame (crashing at RIP=0x14023D000 with TESObjectCELL* RCX = nullptr).
        // Only move NPCs into exterior cells that are ALREADY loaded inside the local player's active 5x5 grid.
        const uint32_t cWorldSpaceId = m_world.GetModSystem().GetGameId(acWorldSpaceId);
        TESWorldSpace* const pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(cWorldSpaceId));
        const TES* pTes = TES::Get();
        ModManager* pModMgr = ModManager::Get();
        if (!pWorldSpace || !pTes || !pModMgr || pLocalPlayer->GetWorldSpace() != pWorldSpace)
            return false;

        const GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acPosition);
        const GridCellCoords playerCenterGrid(pTes->centerGridX, pTes->centerGridY);
        if (!GridCellCoords::IsCellInGridCell(coordinates, playerCenterGrid, false))
            return false;

        pCell = pModMgr->GetCellFromCoordinates(coordinates.X, coordinates.Y, pWorldSpace, false);
        if (!pCell || !pCell->loadedCellData)
            return false;
    }

    apActor->MoveTo(pCell, acPosition);
    return true;
}

void CharacterService::CleanupUnassignedActorEntity(entt::entity aEntity) const noexcept
{
    if (!m_world.valid(aEntity))
        return;

    m_pendingLocalTempSpawns.erase(aEntity);
    if (m_world.any_of<LocalComponent, RemoteComponent, WaitingForAssignmentComponent, ObjectComponent>(aEntity))
        return;

    if (const auto* pFormComp = m_world.try_get<FormIdComponent>(aEntity); pFormComp && pFormComp->Id == 0x14)
        return;

    m_world.remove<EarlyAnimationBufferComponent, CacheComponent, FormIdComponent>(aEntity);
    if (m_world.orphan(aEntity))
        m_world.destroy(aEntity);
}

void CharacterService::CancelServerAssignment(const entt::entity aEntity, const uint32_t aFormId) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    const char* pName = (!pActor || !pActor->baseForm) ? "" : pActor->baseForm->GetName();

    if (m_world.all_of<RemoteComponent>(aEntity))
    {
        auto& remoteComponent = m_world.get<RemoteComponent>(aEntity);
        if (auto modelIt = m_remoteModels.find(remoteComponent.Id); modelIt != m_remoteModels.end() && !m_savePurgeActive)
        {
            DematerializeRemoteView(aEntity, &modelIt.value());
        }
        else
        {
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
                    SanitizeActorCombatStateForLocalOwnership(pActor);
                    if (!pActor->IsDead() && pActor->formID != 0x14)
                        pActor->EvaluatePackage();
                }
            }
            DeleteRemoteEntityComponents(aEntity);
        }
    }

    // In the event we were waiting for assignment, drop it
    if (m_world.all_of<WaitingForAssignmentComponent>(aEntity))
    {
        auto& waitingComponent = m_world.get<WaitingForAssignmentComponent>(aEntity);
        m_cancelledCookies.RecordCancelled(waitingComponent.Cookie);

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

CoopWorldView::ObserverState CharacterService::BuildLocalObserverState() const noexcept
{
    CoopWorldView::ObserverState state{};

    if (m_savePurgeActive || m_engineWorldLoadInProgress)
        return state;

    if (UI* pUI = UI::Get())
    {
        static const BSFixedString* s_pLoadingMenu = new BSFixedString("Loading Menu");
        static const BSFixedString* s_pMainMenu = new BSFixedString("Main Menu");
        if (pUI->GetMenuOpen(*s_pLoadingMenu) || pUI->GetMenuOpen(*s_pMainMenu))
            return state;
    }

    PlayerCharacter* pLocalPlayer = PlayerCharacter::Get();
    TES* pTES = TES::Get();
    if (!pLocalPlayer || !pLocalPlayer->parentCell || !pLocalPlayer->parentCell->loadedCellData || !pTES)
        return state;

    auto& modSystem = m_world.GetModSystem();
    if (TESWorldSpace* pWorldSpace = pLocalPlayer->GetWorldSpace())
    {
        GameId worldSpaceId{};
        if (!modSystem.GetServerModId(pWorldSpace->formID, worldSpaceId) || !worldSpaceId)
            return state;

        state.EngineReady = true;
        state.PosX = pLocalPlayer->position.x;
        state.PosY = pLocalPlayer->position.y;
        state.Location = CoopWorldView::WorldOrCell::MakeExterior(
            worldSpaceId,
            GridCellCoords(pTES->centerGridX, pTES->centerGridY));
        return state;
    }

    GameId interiorCellId{};
    if (!modSystem.GetServerModId(pLocalPlayer->parentCell->formID, interiorCellId) || !interiorCellId)
        return state;

    state.EngineReady = true;
    state.PosX = pLocalPlayer->position.x;
    state.PosY = pLocalPlayer->position.y;
    state.Location = CoopWorldView::WorldOrCell::MakeInterior(interiorCellId);
    return state;
}

TESObjectCELL* CharacterService::ResolveCompatibleTargetCell(
    const CoopWorldView::WorldOrCell& acTargetLoc,
    TESWorldSpace*& apOutWorldSpace,
    bool aAllowDistantWorldSpaceAnchor) const noexcept
{
    apOutWorldSpace = nullptr;

    if (!acTargetLoc.IsValid())
        return nullptr;

    PlayerCharacter* pLocalPlayer = PlayerCharacter::Get();
    TES* pTES = TES::Get();
    if (!pLocalPlayer || !pLocalPlayer->parentCell || !pLocalPlayer->parentCell->loadedCellData || !pTES)
        return nullptr;

    auto& modSystem = m_world.GetModSystem();

    if (acTargetLoc.IsInterior())
    {
        if (pLocalPlayer->GetWorldSpace() != nullptr)
            return nullptr;

        const uint32_t targetCellFormId = modSystem.GetGameId(acTargetLoc.CellId);
        if (targetCellFormId == 0 || pLocalPlayer->parentCell->formID != targetCellFormId)
            return nullptr;

        return pLocalPlayer->parentCell;
    }

    TESWorldSpace* pLocalWs = pLocalPlayer->GetWorldSpace();
    if (!pLocalWs)
        return nullptr;

    const uint32_t targetWsFormId = modSystem.GetGameId(acTargetLoc.WorldSpaceId);
    if (targetWsFormId == 0 || pLocalWs->formID != targetWsFormId)
        return nullptr;

    apOutWorldSpace = pLocalWs;

    // Prefer exact loaded cell from ModManager 5x5 grid; only allow distant same-WorldSpace fallback
    // for flying dragons (never for pre-placed persistent world NPCs outside the 5x5 grid!).
    if (ModManager* pModMgr = ModManager::Get())
    {
        if (TESObjectCELL* pGridCell = pModMgr->GetCellFromCoordinates(
                acTargetLoc.GridCoords.X,
                acTargetLoc.GridCoords.Y,
                pLocalWs,
                false);
            pGridCell && pGridCell->loadedCellData)
        {
            return pGridCell;
        }
    }

    const GridCellCoords centerGrid(pTES->centerGridX, pTES->centerGridY);
    if (GridCellCoords::IsCellInGridCell(acTargetLoc.GridCoords, centerGrid, false))
        return nullptr; // Within 5x5 grid, wait until TES finishes streaming that grid cell's loadedCellData!

    return aAllowDistantWorldSpaceAnchor ? pLocalPlayer->parentCell : nullptr;
}

void CharacterService::DematerializeRemoteView(entt::entity aEntity, RemoteEntityModel* apModel) const noexcept
{
    if (!m_world.valid(aEntity))
        return;

    uint32_t formId = 0;
    if (const auto* pFormComp = m_world.try_get<FormIdComponent>(aEntity))
        formId = pFormComp->Id;
    else if (const auto* pRemoteComp = m_world.try_get<RemoteComponent>(aEntity))
        formId = pRemoteComp->CachedRefId;

    Actor* pActor = formId != 0 ? Cast<Actor>(TESForm::GetById(formId)) : nullptr;
    const bool isNetworkActor =
        (apModel && apModel->SpawnRequest.FormId == GameId{}) || IsNetworkCreatedActor(pActor);
    const bool wasPlayer = m_world.all_of<PlayerComponent>(aEntity) || (apModel && apModel->SpawnRequest.IsPlayer);

    if (pActor && formId != 0x14)
    {
        if (isNetworkActor)
        {
            PurgeDynamicActorCleanly(pActor);
        }
        else if (auto* pExt = pActor->GetExtension())
        {
            pExt->SetRemote(false);
            pExt->SetDownedPlayer(false);
            SanitizeActorCombatStateForLocalOwnership(pActor);
            if (!pActor->IsDead() && pActor->formID != 0x14)
                pActor->EvaluatePackage();
        }
    }

    if (formId != 0)
        m_weaponDrawUpdates.erase(formId);

    if (auto* pRemoteComp = m_world.try_get<RemoteComponent>(aEntity))
        pRemoteComp->CachedRefId = 0;

    if (auto* pFaceGen = m_world.try_get<FaceGenComponent>(aEntity))
    {
        pFaceGen->Generated = false;
        pFaceGen->LastFaceNode = nullptr;
    }

    if (auto* pAnim = m_world.try_get<RemoteAnimationComponent>(aEntity))
        pAnim->TimePoints.clear();

    // Remove PlayerComponent BEFORE WaitingFor3D so OverlayService::OnWaitingFor3DRemoved
    // never fires a spurious setPlayer3dLoaded during dematerialization!
    m_world.remove<PlayerComponent>(aEntity);
    m_world.remove<WaitingFor3D, FormIdComponent, CacheComponent, EarlyAnimationBufferComponent>(aEntity);

    if (apModel)
        apModel->IsMaterialized = false;

    if (wasPlayer)
        m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
}

bool CharacterService::ReconcilePlacedActorBaseFormInPlace(Actor* apActor, const GameId& acExpectedBaseId) const noexcept
{
    if (!apActor ||
        apActor->IsTemporary() ||
        apActor->formID == 0x14 ||
        acExpectedBaseId == GameId{} ||
        acExpectedBaseId.ModId == std::numeric_limits<uint32_t>::max())
    {
        return false;
    }

    const uint32_t expectedFormId = m_world.GetModSystem().GetGameId(acExpectedBaseId);
    if (expectedFormId == 0 || (expectedFormId & 0xFF000000u) == 0xFF000000u || !apActor->baseForm)
        return false;

    TESNPC* pResolvedCurrentBase = apActor->GetLeveledTemplateBase();
    if (apActor->baseForm->formID == expectedFormId ||
        (pResolvedCurrentBase && pResolvedCurrentBase->formID == expectedFormId))
    {
        return false;
    }

    TESNPC* pExpectedNpc = Cast<TESNPC>(TESForm::GetById(expectedFormId));
    if (!pExpectedNpc || pExpectedNpc->IsStillLoading() || pExpectedNpc->IsDeleted())
        return false;

    // Never re-template an actor that is mid-fight or already dead: the swap rebuilds the 3D and resets the
    // combat/equipment state. For a live local actor this is strictly worse than a temporary base mismatch.
    if (apActor->IsDead() || apActor->IsInCombat() || apActor->actorState.IsWeaponFullyDrawn())
        return false;

    // NEVER touch a reference whose 3D is already up. `SetBaseForm` on a loaded `REFR` forces the engine to
    // rebuild the whole actor (NiNode teardown), and `DiscoveryService` reports the still-registered reference
    // as `Actor removed` ~50-100 ms later.
    if (apActor->GetNiNode())
        return false;

    // Only reconcile views we do not actively simulate. `ActorExtension::IsLocal()` is NOT the right test here:
    // every call site runs *before* `SetRemote(true)`, so `IsLocal()` is still true for perfectly remote views.
    // What matters is ECS ownership: an actor with a `LocalComponent` is simulated by this client, and its RFAB
    // leveled template is authoritative (see `OnAssignCharacter`), so it must never be re-templated here.
    const entt::entity ownerEntity = FindActorEntityByFormId(apActor->formID);
    if (ownerEntity != entt::null && m_world.all_of<LocalComponent>(ownerEntity))
        return false;

    spdlog::info(
        "[RFAB LeveledSync] Reconciling placed actor {:X} baseForm {:X} -> {:X} in-place (preserving 0x00..0xFE quest/ambush identity)",
        apActor->formID,
        apActor->baseForm->formID,
        expectedFormId);

    // CRITICAL: never `DisableImpl()`/`EnableImpl()` around the swap, and never swap above a posed 3D (see the
    // `GetNiNode()` guard). Disabling a reference makes the engine drop its NiNode while the REFR is still
    // registered; `DiscoveryService::VisitForms` then dispatches `ActorRemovedEvent` for it, which triggers
    // `CancelServerAssignment()` -> ownership hand-back to the server -> the NPC loses local AI and its equipment
    // is rebuilt from stale spawn data. `SetBaseForm` + `QueueUpdate` on a view that is still materialising streams
    // the correct 3D on its first build, so the entity never leaves the registry.
    apActor->SetBaseForm(pExpectedNpc);
    if (pExpectedNpc->raceForm.race)
        apActor->race = pExpectedNpc->raceForm.race;

    apActor->QueueUpdate();
    return true;
}

Actor* CharacterService::MaterializeRemoteView(
    entt::entity aEntity,
    RemoteEntityModel& aModel,
    TESObjectCELL* apTargetCell,
    TESWorldSpace* apTargetWorldSpace) const noexcept
{
    if (!m_world.valid(aEntity) || !apTargetCell || !apTargetCell->loadedCellData)
        return nullptr;

    const auto& acMessage = aModel.SpawnRequest;
    const NiPoint3 spawnPos = aModel.LatestPosition;
    const NiPoint3 spawnRot(glm::vec3(aModel.LatestRotation.x, 0.f, aModel.LatestRotation.y));

    Actor* pActor = nullptr;

    if (acMessage.FormId == GameId{})
    {
        // SkyMP invariant: never materialize a brand-new temporary actor already dead on arrival,
        // and never materialize a temporary actor whose BaseId is a peer's dynamic 0xFFFFFFFF form ID.
        if ((acMessage.IsDead && !acMessage.IsPlayer) ||
            acMessage.BaseId.ModId == std::numeric_limits<uint32_t>::max())
        {
            return nullptr;
        }

        TESNPC* pNpc = nullptr;

        if (acMessage.BaseId != GameId{})
        {
            const uint32_t cNpcId = m_world.GetModSystem().GetGameId(acMessage.BaseId);
            if (cNpcId == 0 || (cNpcId & 0xFF000000u) == 0xFF000000u)
            {
                spdlog::error("MaterializeRemoteView: invalid or missing mod for NPC base {:X}:{:X}", acMessage.BaseId.ModId, acMessage.BaseId.BaseId);
                return nullptr;
            }

            pNpc = Cast<TESNPC>(TESForm::GetById(cNpcId));
            if (!pNpc || pNpc->IsStillLoading())
                return nullptr;

            // NEVER mutate shared static .esm/.esp TESNPC templates in memory: only deserialize onto dynamic 0xFF00xxxx templates!
            if ((pNpc->formID & 0xFF000000u) == 0xFF000000u && !acMessage.AppearanceBuffer.empty())
            {
                pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
                pNpc->SetSkipSaveFlag(true);
                pNpc->UnsetChanged(0xFFFFFFFF);
            }
        }
        else
        {
            // Never call TESNPC::Create with an empty AppearanceBuffer for a non-player actor:
            // a blank TESNPC has race == nullptr and crashes BSTaskPool at SkyrimSE.exe+0x65D2CB (InitWornArmorAddon / GetSex).
            if (!acMessage.IsPlayer && acMessage.AppearanceBuffer.empty())
            {
                spdlog::warn("MaterializeRemoteView: refusing to create blank TESNPC for non-player serverId {:X}", acMessage.ServerId);
                return nullptr;
            }

            // PlayerBaseNpcPool: allocate at most ONE 0xFF00xxxx TESNPC per remote PlayerId across the entire world session!
            if (acMessage.IsPlayer && acMessage.PlayerId != 0)
            {
                if (const auto poolIt = m_playerBaseNpcPool.find(acMessage.PlayerId); poolIt != m_playerBaseNpcPool.end())
                {
                    pNpc = Cast<TESNPC>(TESForm::GetById(poolIt->second));
                    if (pNpc)
                    {
                        if (!acMessage.AppearanceBuffer.empty())
                            pNpc->Deserialize(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
                        pNpc->overlayRace = nullptr;
                        pNpc->actorData.SetEssential(true);
                        pNpc->SetSkipSaveFlag(true);
                        pNpc->UnsetChanged(0xFFFFFFFF);
                    }
                }
            }

            if (!pNpc)
            {
                pNpc = TESNPC::Create(acMessage.AppearanceBuffer, acMessage.ChangeFlags);
                if (pNpc)
                {
                    pNpc->overlayRace = nullptr;
                    pNpc->actorData.SetEssential(true);
                    m_createdNetworkFormIds.insert(pNpc->formID);
                    if (acMessage.IsPlayer && acMessage.PlayerId != 0)
                        m_playerBaseNpcPool[acMessage.PlayerId] = pNpc->formID;
                }
            }

            FaceGenSystem::Setup(m_world, aEntity, acMessage.FaceTints);
            if (auto* pFaceGen = m_world.try_get<FaceGenComponent>(aEntity))
            {
                pFaceGen->Generated = false;
                pFaceGen->LastFaceNode = nullptr;
            }
        }

        auto pCreated = Actor::CreateInCell(pNpc, apTargetCell, apTargetWorldSpace, spawnPos, spawnRot);
        pActor = pCreated;
        if (pActor)
        {
            m_createdNetworkFormIds.insert(pActor->formID);
            if (pActor->GetExtension())
                pActor->GetExtension()->SetCreatedByNetwork(true);
        }
    }
    else
    {
        const uint32_t cActorId = m_world.GetModSystem().GetGameId(acMessage.FormId);
        if (cActorId == 0 || cActorId == 0x14)
            return nullptr;

        pActor = Cast<Actor>(TESForm::GetById(cActorId));
        if (!pActor || pActor->IsStillLoading() || pActor->IsDeleted())
            return nullptr;

        auto existingView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        for (entt::entity other : existingView)
        {
            if (other == aEntity || existingView.get<FormIdComponent>(other).Id != cActorId)
                continue;

            if (m_world.any_of<LocalComponent, RemoteComponent>(other))
                return nullptr;

            if (auto* pWaitAssign = m_world.try_get<WaitingForAssignmentComponent>(other))
            {
                m_cancelledCookies.RecordCancelled(pWaitAssign->Cookie);
                CancelAssignmentRequest cancelMsg{};
                cancelMsg.Cookie = pWaitAssign->Cookie;
                m_transport.Send(cancelMsg);
            }
            m_pendingLocalTempSpawns.erase(other);
            m_world.destroy(other);
            break;
        }

        // Reconcile placed `TESLevCharacter` template IN PLACE on the real `0x00..0xFE` Actor reference
        // so RFAB quest aliases, `MasterAmbushScript`, boss barriers, `OnDeath`, and `Location Cleared` never break!
        if (!acMessage.IsPlayer)
            (void)ReconcilePlacedActorBaseFormInPlace(pActor, acMessage.BaseId);

        if (pActor->IsDisabled() && !acMessage.IsDead)
            pActor->EnableImpl();
    }

    if (!pActor)
        return nullptr;

    auto& remoteComponent = m_world.emplace_or_replace<RemoteComponent>(aEntity, acMessage.ServerId, pActor->formID);
    remoteComponent.CachedRefId = pActor->formID;
    m_world.emplace_or_replace<FormIdComponent>(aEntity, pActor->formID);
    aModel.IsMaterialized = true;
    aModel.DematerializeGraceUntilMs = 0;

    pActor->GetExtension()->SetRemote(true);
    pActor->GetExtension()->SetPlayer(acMessage.IsPlayer);
    pActor->rotation.x = aModel.LatestRotation.x;
    pActor->rotation.z = aModel.LatestRotation.y;

    if (acMessage.FormId != GameId{} && !IsNetworkCreatedActor(pActor))
        pActor->MoveTo(apTargetCell, spawnPos);
    else
        pActor->ForcePosition(spawnPos);

    if (auto* pInterp = m_world.try_get<InterpolationComponent>(aEntity))
    {
        pInterp->TimePoints.clear();
        pInterp->Position = spawnPos;
        pInterp->Clock.Reset();
        pInterp->TrackedParentCell = nullptr;
        pInterp->HasTrackedParentGrid = false;
        pInterp->LastSeamlessRehomeTick = 0;
    }
    else
    {
        InterpolationSystem::Setup(m_world, aEntity).Position = spawnPos;
    }

    if (!m_world.all_of<RemoteAnimationComponent>(aEntity))
        AnimationSystem::Setup(m_world, aEntity);

    pActor->SetActorValues(acMessage.InitialActorValues);

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
        AssignRemoteSummonOwner(m_world, pActor, acMessage.PlayerId);

    auto& cacheComp = m_world.emplace_or_replace<CacheComponent>(aEntity);
    cacheComp.FactionsContent = acMessage.FactionsContent;
    pActor->SetFactions(acMessage.FactionsContent);
    pActor->GetExtension()->SetNakedDeadline();

    if (pActor->IsDead() != acMessage.IsDead)
        acMessage.IsDead ? KillActorSuppressingDuplicateXp(pActor) : pActor->ResurrectInPlace();

    const uint32_t spawnedServerId = acMessage.ServerId;
    const uint64_t deferredGrantEpoch = aModel.PendingOwnershipEpoch;

    CharacterSpawnRequest viewSpawnCopy = acMessage;
    viewSpawnCopy.Position = aModel.LatestPosition;
    viewSpawnCopy.Rotation = aModel.LatestRotation;
    m_world.emplace_or_replace<WaitingFor3D>(aEntity, viewSpawnCopy);

    spdlog::info(
        "Materialized remote FormView, server id: {:X}, form id: {:X}, cell: {:X}, ws: {:X}",
        spawnedServerId,
        pActor->formID,
        apTargetCell->formID,
        apTargetWorldSpace ? apTargetWorldSpace->formID : 0u);

    // Complete deferred two-phase dormant ownership grant only once 3D is loaded;
    // if 3D is still streaming, keep `aModel.PendingOwnershipEpoch` so `RunRemoteUpdates`
    // promotes it to `TakeOwnership` on the exact frame `GetNiNode() != nullptr`.
    if (deferredGrantEpoch != 0 && pActor->GetNiNode() && !pActor->IsStillLoading())
    {
        aModel.PendingOwnershipEpoch = 0;
        pActor->SetActorInventory(viewSpawnCopy.InventoryContent);
        (void)TakeOwnership(pActor->formID, spawnedServerId, aEntity, deferredGrantEpoch);
    }

    return pActor;
}

void CharacterService::ReconcileRemoteWorldViews() const noexcept
{
    if (m_savePurgeActive || m_engineWorldLoadInProgress)
        return;

    FlushDeferredDialogueDeletes();

    constexpr size_t kMaxRemoteNpcSpawnsPerFrame = 4;
    size_t nonPrioritySpawnsThisFrame = 0;

    const CoopWorldView::ObserverState obs = BuildLocalObserverState();
    const uint64_t nowMs = NowSteadyWorldViewMs();

    auto remoteView = m_world.view<RemoteComponent>();
    Vector<entt::entity> entities(remoteView.begin(), remoteView.end());

    for (const entt::entity entity : entities)
    {
        if (!m_world.valid(entity))
            continue;

        const uint32_t serverId = m_world.get<RemoteComponent>(entity).Id;
        auto modelIt = m_remoteModels.find(serverId);
        if (modelIt == m_remoteModels.end())
            continue;

        auto& model = modelIt.value();
        const auto* pFormComp = m_world.try_get<FormIdComponent>(entity);
        Actor* pLiveActor = pFormComp ? Cast<Actor>(TESForm::GetById(pFormComp->Id)) : nullptr;

        // Persistent pre-placed world refs (e.g. Skyrim.esm exterior bandits E5F72-E5F77) stay in TESForm::GetById
        // even when their cell unloads! Treat non-network, non-dragon refs as unmaterialized if parentCell unloaded.
        const bool requiresLoadedParentCell =
            pLiveActor && !IsNetworkCreatedActor(pLiveActor) && !model.SpawnRequest.IsDragon;
        const bool cellActuallyLoaded = !requiresLoadedParentCell ||
            (pLiveActor->parentCell != nullptr && pLiveActor->parentCell->loadedCellData != nullptr);
        const bool isMaterialized = (pFormComp != nullptr && pLiveActor != nullptr && cellActuallyLoaded);

        // Clean up stale FormIdComponent if engine natively unloaded the underlying TESForm or cell
        if (pFormComp != nullptr && !isMaterialized)
            DematerializeRemoteView(entity, &model);

        TESWorldSpace* pTargetWs = nullptr;
        TESObjectCELL* pTargetCell = obs.EngineReady
            ? ResolveCompatibleTargetCell(model.Location, pTargetWs, model.SpawnRequest.IsDragon)
            : nullptr;

        CoopWorldView::RemoteViewQuery query{};
        query.Location = model.Location;
        query.PosX = model.LatestPosition.x;
        query.PosY = model.LatestPosition.y;
        query.IsDragon = model.SpawnRequest.IsDragon;
        query.IsPlayer = model.SpawnRequest.IsPlayer;
        query.CurrentlyMaterialized = isMaterialized;
        query.TargetCellLoadedInEngine = (pTargetCell != nullptr);
        query.NowSteadyMs = nowMs;
        query.DematerializeGraceUntilMs = model.DematerializeGraceUntilMs;

        const auto decision = CoopWorldView::EvaluateViewDecision(obs, query);
        switch (decision)
        {
        case CoopWorldView::ViewDecision::kDematerialize:
            DematerializeRemoteView(entity, &model);
            break;
        case CoopWorldView::ViewDecision::kMaterialize:
        {
            const bool isHighPriority =
                model.SpawnRequest.IsPlayer ||
                model.SpawnRequest.IsDragon ||
                (model.PendingOwnershipEpoch != 0);
            if (!isHighPriority && nonPrioritySpawnsThisFrame >= kMaxRemoteNpcSpawnsPerFrame)
                break;

            if (MaterializeRemoteView(entity, model, pTargetCell, pTargetWs) != nullptr && !isHighPriority)
                ++nonPrioritySpawnsThisFrame;
            break;
        }
        case CoopWorldView::ViewDecision::kKeepMaterialized:
        case CoopWorldView::ViewDecision::kKeepDematerialized:
        default:
            break;
        }
    }
}

void CharacterService::RunLocalUpdates() const noexcept
{
    static std::chrono::steady_clock::time_point s_lastFastSendTimePoint;
    static std::chrono::steady_clock::time_point s_lastSlowSendTimePoint;
    constexpr auto cFastSnapshotInterval = 33ms;
    constexpr auto cSlowSnapshotInterval = 100ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastFastSendTimePoint < cFastSnapshotInterval)
        return;

    s_lastFastSendTimePoint = now;
    const bool includeSlowActors = (now - s_lastSlowSendTimePoint >= cSlowSnapshotInterval);
    if (includeSlowActors)
        s_lastSlowSendTimePoint = now;

    ClientReferencesMoveRequest message;
    message.Tick = m_transport.GetClock().GetCurrentTick();
    bool hasAnyActions = false;

    auto animatedLocalView = m_world.view<LocalComponent, LocalAnimationComponent, FormIdComponent>();

    for (auto entity : animatedLocalView)
    {
        auto& localComponent = animatedLocalView.get<LocalComponent>(entity);
        auto& animationComponent = animatedLocalView.get<LocalAnimationComponent>(entity);
        auto& formIdComponent = animatedLocalView.get<FormIdComponent>(entity);

        if (includeSlowActors)
        {
            if (Actor* pLocalActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id)))
            {
                if (auto* pExt = pLocalActor->GetExtension(); pExt && pExt->IsRemote())
                {
                    pExt->SetRemote(false);
                    SanitizeActorCombatStateForLocalOwnership(pLocalActor);
                    if (!pLocalActor->IsDead() && pLocalActor->formID != 0x14)
                        pLocalActor->EvaluatePackage();
                }
            }
        }

        if (!includeSlowActors && animationComponent.Actions.empty() && formIdComponent.Id != 0x14)
        {
            Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
            const bool isFastPriority = pActor && (pActor->IsInCombat() || pActor->IsMount() || pActor->IsDragon() || pActor->IsPlayerSummon());
            if (!isFastPriority)
                continue;
        }

        const bool actorHadActions = !animationComponent.Actions.empty();
        AnimationSystem::Serialize(m_world, message, localComponent, animationComponent, formIdComponent);
        if (actorHadActions)
            hasAnyActions = true;
    }

    if (!message.Updates.empty())
        m_transport.Send(message, hasAnyActions ? TiltedPhoques::kReliable : TiltedPhoques::kUnreliableNoNagle);
}

void CharacterService::RunRemoteUpdates() noexcept
{
    const uint64_t currentClockTick = m_transport.GetClock().GetCurrentTick();
    const uint64_t fallbackTick = currentClockTick > 300 ? (currentClockTick - 300) : 0;
    const double lastPollTime = m_transport.GetLastPollTime();

    // Interpolation has to keep running even if the actor is not in view, otherwise we will never know if we need to spawn it
    auto interpolatedEntities = m_world.view<RemoteComponent, InterpolationComponent>();

    for (auto entity : interpolatedEntities)
    {
        auto* pFormIdComponent = m_world.try_get<FormIdComponent>(entity);
        auto& interpolationComponent = interpolatedEntities.get<InterpolationComponent>(entity);

        if (lastPollTime > 0.0 && interpolationComponent.Clock.IsRunning())
            interpolationComponent.Clock.Advance(lastPollTime);

        const uint64_t entityTick = interpolationComponent.Clock.IsRunning()
            ? static_cast<uint64_t>(std::max(0.0, interpolationComponent.Clock.GetRenderTick()))
            : fallbackTick;

        Actor* pActor = nullptr;
        if (pFormIdComponent)
        {
            auto* pForm = TESForm::GetById(pFormIdComponent->Id);
            pActor = Cast<Actor>(pForm);
        }

        InterpolationSystem::Update(pActor, interpolationComponent, entityTick, m_world.try_get<RemoteAnimationComponent>(entity));
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

        uint64_t animTick = fallbackTick;
        if (const auto* pInterp = m_world.try_get<InterpolationComponent>(entity); pInterp && pInterp->Clock.IsRunning())
            animTick = static_cast<uint64_t>(std::max(0.0, pInterp->Clock.GetRenderTick()));

        AnimationSystem::Update(m_world, pActor, animationComponent, animTick);
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

        // By now, the actor has materialized in the world and is ready for further setup.
        // Always resolve the Single-Source-of-Truth `RemoteEntityModel::SpawnRequest` so any
        // inventory/equipment/AV/health/death/weapon-draw delta received while 3D was streaming is preserved!
        const CharacterSpawnRequest& activeSpawn = ResolveAuthoritativeSpawnState(entity, waitingFor3D.SpawnRequest);

        pActor->SetActorInventory(activeSpawn.InventoryContent);
        pActor->SetActorValues(activeSpawn.InitialActorValues);
        pActor->SetFactions(activeSpawn.FactionsContent);

        if (!activeSpawn.ActionsToReplay.Actions.empty())
        {
            pActor->LoadAnimationVariables(activeSpawn.ActionsToReplay.Actions[0].Variables);
        }

        m_weaponDrawUpdates[pActor->formID] = {activeSpawn.IsWeaponDrawn};

        if (pActor->IsDead() != activeSpawn.IsDead)
            activeSpawn.IsDead ? KillActorSuppressingDuplicateXp(pActor) : pActor->ResurrectInPlace();

        if (pActor->IsVampireLord())
            pActor->FixVampireLordModel();

        if (activeSpawn.IsPlayer &&
            m_world.GetDownedStateService().IsRemotePlayerDowned(activeSpawn.PlayerId))
        {
            m_weaponDrawUpdates[pActor->formID] = {false};
            m_world.GetDownedStateService().ApplyRemoteActorDownedPosture(pActor, true);
        }

        if (activeSpawn.IsPlayerSummon)
        {
            AssignRemoteSummonOwner(m_world, pActor, activeSpawn.PlayerId);
        }

        // (Re)start naked NPC deadline. Actor MUST be fully constructed before check clock starts.
        pActor->GetExtension()->SetNakedDeadline();

        const uint32_t resolvedServerId = activeSpawn.ServerId;
        uint64_t pendingGrantEpoch = 0;
        if (resolvedServerId != 0)
        {
            if (auto modelIt = m_remoteModels.find(resolvedServerId); modelIt != m_remoteModels.end())
            {
                pendingGrantEpoch = modelIt.value().PendingOwnershipEpoch;
                modelIt.value().PendingOwnershipEpoch = 0;
            }
        }

        toRemove.push_back(entity);

        spdlog::info(__FUNCTION__ ": applied 3D for actor, form id: {:X}, name {}", pActor->formID, pActor->baseForm ? pActor->baseForm->GetName() : "<null>");

        if (pendingGrantEpoch != 0 && resolvedServerId != 0)
        {
            (void)TakeOwnership(pActor->formID, resolvedServerId, entity, pendingGrantEpoch);
        }
    }

    for (auto entity : toRemove)
        m_world.remove<WaitingFor3D>(entity);

    RetryPendingMounts();
}
