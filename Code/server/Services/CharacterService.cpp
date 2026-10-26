#include <Services/CharacterService.h>
#include <Components.h>
#include <common/CoopPacketSafety.h>
#include <GameServer.h>
#include <World.h>

#include <Events/CharacterSpawnedEvent.h>
#include <Events/CharacterExteriorCellChangeEvent.h>
#include <Events/CharacterInteriorCellChangeEvent.h>
#include <Events/PlayerEnterWorldEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/CharacterRemoveEvent.h>
#include <Events/OwnershipTransferEvent.h>

#include <Game/OwnerView.h>

#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyPlayerCellChanged.h>
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

#include <Setting.h>
#include <CoopFlightDigest.h>
#include <CoopOwnership.h>
#include <chrono>

namespace
{
Console::Setting bEnableXpSync{"Gameplay:bEnableXpSync", "Syncs combat XP within the party", true};
Console::Setting bEnableRfabXpSync{"Gameplay:bEnableRfabXpSync", "Syncs RFAB XP gains within the party", true};
constexpr uint32_t kHealthActorValueIndex = 24;

uint64_t SteadyClockMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}

CharacterService::CharacterService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_charIndexConstructConn(aWorld.on_construct<CharacterComponent>().connect<&CharacterService::OnCharacterIndexConstructed>(this))
    , m_charIndexDestroyConn(aWorld.on_destroy<CharacterComponent>().connect<&CharacterService::OnCharacterIndexDestroyed>(this))
    , m_formIndexDestroyConn(aWorld.on_destroy<FormIdComponent>().connect<&CharacterService::OnFormIdIndexDestroyed>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&CharacterService::OnUpdate>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&CharacterService::OnPlayerLeave>(this))
    , m_interiorCellChangeEventConnection(aDispatcher.sink<CharacterInteriorCellChangeEvent>().connect<&CharacterService::OnCharacterInteriorCellChange>(this))
    , m_exteriorCellChangeEventConnection(aDispatcher.sink<CharacterExteriorCellChangeEvent>().connect<&CharacterService::OnCharacterExteriorCellChange>(this))
    , m_characterAssignRequestConnection(aDispatcher.sink<PacketEvent<AssignCharacterRequest>>().connect<&CharacterService::OnAssignCharacterRequest>(this))
    , m_transferOwnershipConnection(aDispatcher.sink<PacketEvent<RequestOwnershipTransfer>>().connect<&CharacterService::OnOwnershipTransferRequest>(this))
    , m_ownershipTransferEventConnection(aDispatcher.sink<OwnershipTransferEvent>().connect<&CharacterService::OnOwnershipTransferEvent>(this))
    , m_claimOwnershipConnection(aDispatcher.sink<PacketEvent<RequestOwnershipClaim>>().connect<&CharacterService::OnOwnershipClaimRequest>(this))
    , m_removeCharacterConnection(aDispatcher.sink<CharacterRemoveEvent>().connect<&CharacterService::OnCharacterRemoveEvent>(this))
    , m_characterSpawnedConnection(aDispatcher.sink<CharacterSpawnedEvent>().connect<&CharacterService::OnCharacterSpawned>(this))
    , m_referenceMovementSnapshotConnection(aDispatcher.sink<PacketEvent<ClientReferencesMoveRequest>>().connect<&CharacterService::OnReferencesMoveRequest>(this))
    , m_factionsChangesConnection(aDispatcher.sink<PacketEvent<RequestFactionsChanges>>().connect<&CharacterService::OnFactionsChanges>(this))
    , m_mountConnection(aDispatcher.sink<PacketEvent<MountRequest>>().connect<&CharacterService::OnMountRequest>(this))
    , m_newPackageConnection(aDispatcher.sink<PacketEvent<NewPackageRequest>>().connect<&CharacterService::OnNewPackageRequest>(this))
    , m_requestRespawnConnection(aDispatcher.sink<PacketEvent<RequestRespawn>>().connect<&CharacterService::OnRequestRespawn>(this))
    , m_syncExperienceConnection(aDispatcher.sink<PacketEvent<SyncExperienceRequest>>().connect<&CharacterService::OnSyncExperienceRequest>(this))
    , m_syncRfabExperienceConnection(aDispatcher.sink<PacketEvent<SyncRfabExperienceRequest>>().connect<&CharacterService::OnSyncRfabExperienceRequest>(this))
    , m_dialogueConnection(aDispatcher.sink<PacketEvent<DialogueRequest>>().connect<&CharacterService::OnDialogueRequest>(this))
    , m_subtitleConnection(aDispatcher.sink<PacketEvent<SubtitleRequest>>().connect<&CharacterService::OnSubtitleRequest>(this))
{
}

void CharacterService::OnCharacterIndexConstructed(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    if (const auto* pForm = aRegistry.try_get<FormIdComponent>(aEntity); pForm && pForm->Id != GameId{})
        m_characterByFormKey[CoopWorldView::PackGameIdKey(pForm->Id.ModId, pForm->Id.BaseId)] = aEntity;
}

void CharacterService::OnCharacterIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    if (const auto* pForm = aRegistry.try_get<FormIdComponent>(aEntity); pForm && pForm->Id != GameId{})
    {
        const uint64_t key = CoopWorldView::PackGameIdKey(pForm->Id.ModId, pForm->Id.BaseId);
        const auto it = m_characterByFormKey.find(key);
        if (it != m_characterByFormKey.end() && it->second == aEntity)
            m_characterByFormKey.erase(it);
    }
}

void CharacterService::OnFormIdIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    if (const auto* pForm = aRegistry.try_get<FormIdComponent>(aEntity); pForm && pForm->Id != GameId{})
    {
        const uint64_t key = CoopWorldView::PackGameIdKey(pForm->Id.ModId, pForm->Id.BaseId);
        const auto it = m_characterByFormKey.find(key);
        if (it != m_characterByFormKey.end() && it->second == aEntity)
            m_characterByFormKey.erase(it);
    }
}

entt::entity CharacterService::FindPersistentCharacterByFormId(const GameId& acFormId) const noexcept
{
    if (acFormId == GameId{})
        return entt::null;

    const uint64_t key = CoopWorldView::PackGameIdKey(acFormId.ModId, acFormId.BaseId);
    const auto it = m_characterByFormKey.find(key);
    if (it != m_characterByFormKey.end())
    {
        const entt::entity candidate = it->second;
        if (m_world.valid(candidate) && m_world.all_of<FormIdComponent, CharacterComponent>(candidate))
        {
            if (m_world.get<FormIdComponent>(candidate).Id == acFormId)
                return candidate;
        }
    }

    auto view = m_world.view<FormIdComponent, CharacterComponent>();
    for (entt::entity entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id == acFormId)
        {
            m_characterByFormKey[key] = entity;
            return entity;
        }
    }

    return entt::null;
}

void CharacterService::Serialize(World& aRegistry, entt::entity aEntity, CharacterSpawnRequest* apSpawnRequest) noexcept
{
    const auto& characterComponent = aRegistry.get<CharacterComponent>(aEntity);

    apSpawnRequest->ServerId = World::ToInteger(aEntity);
    apSpawnRequest->AppearanceBuffer = characterComponent.SaveBuffer;
    apSpawnRequest->ChangeFlags = characterComponent.ChangeFlags;
    apSpawnRequest->FaceTints = characterComponent.FaceTints;
    apSpawnRequest->FactionsContent = characterComponent.FactionsContent;
    apSpawnRequest->IsDead = characterComponent.IsDead();
    apSpawnRequest->IsPlayer = characterComponent.IsPlayer();
    apSpawnRequest->IsWeaponDrawn = characterComponent.IsWeaponDrawn();
    apSpawnRequest->IsPlayerSummon = characterComponent.IsPlayerSummon();
    apSpawnRequest->IsDragon = characterComponent.IsDragon();
    apSpawnRequest->PlayerId = characterComponent.PlayerId;

    const auto* pFormIdComponent = aRegistry.try_get<FormIdComponent>(aEntity);
    if (pFormIdComponent)
    {
        apSpawnRequest->FormId = pFormIdComponent->Id;
    }

    const auto* pInventoryComponent = aRegistry.try_get<InventoryComponent>(aEntity);
    if (pInventoryComponent)
    {
        apSpawnRequest->InventoryContent = pInventoryComponent->Content;
    }

    const auto* pActorValuesComponent = aRegistry.try_get<ActorValuesComponent>(aEntity);
    if (pActorValuesComponent)
    {
        apSpawnRequest->InitialActorValues = pActorValuesComponent->CurrentActorValues;
    }

    if (characterComponent.BaseId)
    {
        apSpawnRequest->BaseId = characterComponent.BaseId.Id;
    }

    const auto* pMovementComponent = aRegistry.try_get<MovementComponent>(aEntity);
    if (pMovementComponent)
    {
        apSpawnRequest->Position = pMovementComponent->Position;
        apSpawnRequest->Rotation.x = pMovementComponent->Rotation.x;
        apSpawnRequest->Rotation.y = pMovementComponent->Rotation.z;
    }

    const auto* pCellIdComponent = aRegistry.try_get<CellIdComponent>(aEntity);
    if (pCellIdComponent)
    {
        apSpawnRequest->CellId = pCellIdComponent->Cell;
        apSpawnRequest->WorldSpaceId = pCellIdComponent->WorldSpaceId;
    }

    if (auto* pAnimationComponent = aRegistry.try_get<AnimationComponent>(aEntity))
    {
        apSpawnRequest->ActionsToReplay = pAnimationComponent->ActionsReplayCache.FormRefinedReplayChain();
    }
}

void CharacterService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) const noexcept
{
    if (!acEvent.pPlayer)
        return;

    m_interestRegistry.RemoveObserver(acEvent.pPlayer->GetId());

    // Purge disconnecting Player* from every OwnerComponent::InvalidOwners list to prevent
    // dangling pointers and ABA address-reuse blacklisting on reconnect.
    auto ownerView = m_world.view<OwnerComponent>();
    for (auto entity : ownerView)
    {
        auto& ownerComp = ownerView.get<OwnerComponent>(entity);
        auto& invalid = ownerComp.InvalidOwners;
        invalid.erase(std::remove(invalid.begin(), invalid.end(), acEvent.pPlayer), invalid.end());
    }
}

void CharacterService::ReconcileEntityForAllObservers(entt::entity aEntity) const noexcept
{
    if (!m_world.valid(aEntity))
        return;

    const auto* pCharComp = m_world.try_get<CharacterComponent>(aEntity);
    const auto* pCellComp = m_world.try_get<CellIdComponent>(aEntity);
    const auto* pOwnerComp = m_world.try_get<OwnerComponent>(aEntity);
    if (!pCharComp || !pCellComp || !pOwnerComp)
        return;

    const uint32_t serverId = World::ToInteger(aEntity);
    Player* const pOwner = pOwnerComp->GetOwner();

    std::optional<CharacterSpawnRequest> cachedSpawn{};
    const auto getSpawnMessage = [&]() -> const CharacterSpawnRequest&
    {
        if (!cachedSpawn.has_value())
        {
            cachedSpawn.emplace();
            Serialize(m_world, aEntity, &(*cachedSpawn));
        }
        return *cachedSpawn;
    };

    for (Player* pObserver : m_world.GetPlayerManager())
    {
        if (!pObserver)
            continue;

        if (pObserver == pOwner)
        {
            m_interestRegistry.UnmarkInRange(pObserver->GetId(), serverId);
            continue;
        }

        const bool nowInRange = pCellComp->IsInRange(pObserver->GetCellComponent(), pCharComp->IsDragon());
        const auto action = m_interestRegistry.UpdateInterest(
            pObserver->GetId(), serverId, pCharComp->IsPlayer(), nowInRange);

        switch (action)
        {
        case CoopWorldView::ServerPeerSyncAction::kSendSpawn:
            pObserver->Send(getSpawnMessage());
            break;
        case CoopWorldView::ServerPeerSyncAction::kSendRemove:
        {
            NotifyRemoveCharacter removeMsg{};
            removeMsg.ServerId = serverId;
            pObserver->Send(removeMsg);
            break;
        }
        case CoopWorldView::ServerPeerSyncAction::kSendPlayerCellChangedOnly:
        {
            if (pOwner)
            {
                NotifyPlayerCellChanged cellNotify{};
                cellNotify.PlayerId = pOwner->GetId();
                cellNotify.WorldSpaceId = pCellComp->WorldSpaceId;
                cellNotify.CellId = pCellComp->Cell;
                pObserver->Send(cellNotify);
            }
            break;
        }
        case CoopWorldView::ServerPeerSyncAction::kNone:
        default:
            break;
        }
    }
}

void CharacterService::ReconcileAllEntitiesForObserver(Player* apObserver) const noexcept
{
    if (!apObserver)
        return;

    if (const auto observerChar = apObserver->GetCharacter(); observerChar.has_value() && m_world.valid(*observerChar))
    {
        if (auto* pObserverCharCell = m_world.try_get<CellIdComponent>(*observerChar))
        {
            const auto& obsCell = apObserver->GetCellComponent();
            if (obsCell.IsValid() &&
                (!pObserverCharCell->IsValid() ||
                 pObserverCharCell->WorldSpaceId != obsCell.WorldSpaceId ||
                 (obsCell.IsInInteriorCell() && pObserverCharCell->Cell != obsCell.Cell)))
            {
                *pObserverCharCell = obsCell;
            }
        }
        ReconcileEntityForAllObservers(*observerChar);
    }

    auto characterView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();
    for (auto entity : characterView)
    {
        const auto& ownerComp = characterView.get<OwnerComponent>(entity);
        const auto& cellComp = characterView.get<CellIdComponent>(entity);
        const auto& charComp = characterView.get<CharacterComponent>(entity);
        const uint32_t serverId = World::ToInteger(entity);

        if (ownerComp.GetOwner() == apObserver)
        {
            m_interestRegistry.UnmarkInRange(apObserver->GetId(), serverId);
            continue;
        }

        const bool nowInRange = apObserver->GetCellComponent().IsInRange(cellComp, charComp.IsDragon());
        const auto action = m_interestRegistry.UpdateInterest(
            apObserver->GetId(), serverId, charComp.IsPlayer(), nowInRange);

        if (action == CoopWorldView::ServerPeerSyncAction::kSendSpawn)
        {
            CharacterSpawnRequest spawnMessage{};
            Serialize(m_world, entity, &spawnMessage);
            apObserver->Send(spawnMessage);
        }
        else if (action == CoopWorldView::ServerPeerSyncAction::kSendRemove)
        {
            NotifyRemoveCharacter removeMsg{};
            removeMsg.ServerId = serverId;
            apObserver->Send(removeMsg);
        }

        // Wake dormant persistent world NPCs whenever a living observer is in their cell range — even if
        // `action == kNone` (`wasInRange == true` after `OnOwnershipTransferEvent` fell back to dormant)!
        if (nowInRange)
        {
            auto& mutableOwner = characterView.get<OwnerComponent>(entity);
            const uint64_t nowMs = SteadyClockMs();
            const bool cooldownReady = (action == CoopWorldView::ServerPeerSyncAction::kSendSpawn) ||
                                       (nowMs >= mutableOwner.LastChangedAtMs && (nowMs - mutableOwner.LastChangedAtMs) >= 1000ULL);
            if (mutableOwner.GetOwner() == nullptr &&
                cooldownReady &&
                !charComp.IsPlayer() &&
                !charComp.IsPlayerSummon() &&
                !charComp.IsMount() &&
                !m_world.GetEncounterService().GetCoordinator().IsPlayerDowned(apObserver->GetId()))
            {
                const uint64_t newEpoch = mutableOwner.SetOwner(apObserver, nowMs);
                mutableOwner.InvalidOwners.clear();
                m_interestRegistry.UnmarkInRange(apObserver->GetId(), serverId);

                NotifyOwnershipTransfer grant{};
                grant.ServerId = serverId;
                grant.OwnershipEpoch = newEpoch;
                apObserver->Send(grant);

                m_world.GetSaveSnapshotService().GetCoordinator().RecordFlightEvent(
                    nowMs, CoopFlight::FlightEventKind::kOwnershipChanged, serverId, apObserver->GetId());
            }
        }
    }
}

void CharacterService::ForceResyncObserverAfterRollback(Player* apObserver) const noexcept
{
    if (!apObserver)
        return;

    // Clear stale pre-rollback interest marks for this observer so ReconcileAllEntitiesForObserver
    // emits fresh CharacterSpawnRequest packets with post-rollback coordinates & health for ALL
    // in-range NPCs (e.g. surviving Fort Greymoor exterior bandits) and party teammates!
    m_interestRegistry.RemoveObserver(apObserver->GetId());
    ReconcileAllEntitiesForObserver(apObserver);

    auto existingPlayersView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();
    for (auto otherEntity : existingPlayersView)
    {
        const auto& otherChar = existingPlayersView.get<CharacterComponent>(otherEntity);
        const auto& otherOwner = existingPlayersView.get<OwnerComponent>(otherEntity);
        if (!otherChar.IsPlayer() || otherOwner.GetOwner() == apObserver)
            continue;

        const uint32_t otherServerId = World::ToInteger(otherEntity);
        if (!m_interestRegistry.IsMarkedInRange(apObserver->GetId(), otherServerId))
        {
            CharacterSpawnRequest spawnMessage{};
            Serialize(m_world, otherEntity, &spawnMessage);
            apObserver->Send(spawnMessage);
        }
    }
}

void CharacterService::OnUpdate(const UpdateEvent&) const noexcept
{
    ProcessFactionsChanges();
    ProcessMovementChanges();
    WakeDormantCharactersInActiveRanges();
}

void CharacterService::OnCharacterExteriorCellChange(const CharacterExteriorCellChangeEvent& acEvent) const noexcept
{
    ReconcileEntityForAllObservers(acEvent.Entity);
}

void CharacterService::OnCharacterInteriorCellChange(const CharacterInteriorCellChangeEvent& acEvent) const noexcept
{
    ReconcileEntityForAllObservers(acEvent.Entity);
}

void CharacterService::OnAssignCharacterRequest(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const auto& refId = message.ReferenceId;

    const auto isPlayer = (refId.ModId == 0 && refId.BaseId == 0x14);
    const auto isCustom = isPlayer || refId.ModId == std::numeric_limits<uint32_t>::max();

    // Check if id is the player
    if (!isCustom)
    {
        const entt::entity existingEntity = FindPersistentCharacterByFormId(refId);
        if (existingEntity != entt::null &&
            m_world.all_of<FormIdComponent, ActorValuesComponent, CharacterComponent, MovementComponent, CellIdComponent, OwnerComponent, InventoryComponent>(existingEntity))
        {
            // This entity already has an owner
            spdlog::debug("FormId: {:x}:{:x} is already managed", refId.ModId, refId.BaseId);

            auto& actorValuesComponent = m_world.get<ActorValuesComponent>(existingEntity);
            auto& inventoryComponent = m_world.get<InventoryComponent>(existingEntity);
            auto& characterComponent = m_world.get<CharacterComponent>(existingEntity);
            auto& movementComponent = m_world.get<MovementComponent>(existingEntity);
            auto& cellIdComponent = m_world.get<CellIdComponent>(existingEntity);
            auto& existingOwnerComp = m_world.get<OwnerComponent>(existingEntity);

            auto& partyService = m_world.GetPartyService();

            bool isOwner = false;
            Player* pOwningPlayer = existingOwnerComp.GetOwner();

            if (!characterComponent.IsMount())
            {
                PartyService::Party* pParty = partyService.GetPlayerParty(acMessage.pPlayer);
                const bool isOrphanEntity = (pOwningPlayer == nullptr);
                const bool inParty = (pParty != nullptr) && partyService.IsPlayerInParty(acMessage.pPlayer);

                if (isOrphanEntity || inParty)
                {
                    const bool ownerValidInRange = pOwningPlayer != nullptr &&
                                                   pOwningPlayer->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon());
                    const bool ownerDowned = pOwningPlayer != nullptr &&
                                             m_world.GetEncounterService().GetCoordinator().IsPlayerDowned(pOwningPlayer->GetId());
                    const bool challengerDowned = m_world.GetEncounterService().GetCoordinator().IsPlayerDowned(acMessage.pPlayer->GetId());
                    const bool canChallengeOwnership =
                        isOrphanEntity || partyService.IsPlayerLeader(acMessage.pPlayer) || ownerDowned || !ownerValidInRange;

                    if (canChallengeOwnership)
                    {
                        const auto claimDecision = CoopOwnership::EvaluateVoluntaryClaim(
                            pOwningPlayer ? pOwningPlayer->GetId() : 0,
                            acMessage.pPlayer->GetId(),
                            existingOwnerComp.LastChangedAtMs,
                            SteadyClockMs(),
                            characterComponent.IsPlayer(),
                            characterComponent.IsPlayerSummon(),
                            ownerValidInRange,
                            ownerDowned,
                            challengerDowned);

                        // Transfer ownership if entity is dormant/orphaned OR owning player is in the same party
                        // and passes anti-flap/summon/downed rules. Preserve the server's authoritative battle-damaged/dead/looted
                        // state instead of overwriting it with the newly arriving peer's untouched local NPC state!
                        const bool samePartyOrOrphan =
                            isOrphanEntity ||
                            (pParty != nullptr &&
                             std::find(pParty->Members.begin(), pParty->Members.end(), pOwningPlayer) != pParty->Members.end());
                        if ((claimDecision == CoopOwnership::ClaimDecision::kAccept || claimDecision == CoopOwnership::ClaimDecision::kAlreadyOwner) &&
                            samePartyOrOrphan)
                        {
                            const ActorData authoritativeActorData = BuildActorData(existingEntity);
                            TransferOwnership(acMessage.pPlayer, World::ToInteger(existingEntity), authoritativeActorData);
                            isOwner = true;
                        }
                    }
                }
            }

            AssignCharacterResponse response{};
            response.Cookie = message.Cookie;
            response.ServerId = World::ToInteger(existingEntity);
            response.Owner = isOwner;
            response.OwnershipEpoch = m_world.get<OwnerComponent>(existingEntity).Epoch;
            response.AllActorValues = actorValuesComponent.CurrentActorValues;
            response.CurrentInventory = inventoryComponent.Content;
            response.IsDead = characterComponent.IsDead();
            response.IsWeaponDrawn = characterComponent.IsWeaponDrawn();
            response.PlayerId = characterComponent.PlayerId;
            response.Position = movementComponent.Position;
            response.CellId = cellIdComponent.Cell;
            response.WorldSpaceId = cellIdComponent.WorldSpaceId;

            if (auto* pAnimationComponent = m_world.try_get<AnimationComponent>(existingEntity))
            {
                response.ActionsToReplay = pAnimationComponent->ActionsReplayCache.FormRefinedReplayChain();
            }

            if (!isOwner && acMessage.pPlayer)
                m_interestRegistry.MarkInRange(acMessage.pPlayer->GetId(), response.ServerId);

            acMessage.pPlayer->Send(response);

            // If this placed `TESLevCharacter` resolved to a different `BaseId` on the creator than on the newly
            // arriving peer (`characterComponent.BaseId.Id != message.FormId`), also emit `CharacterSpawnRequest`
            // so the peer receives the authoritative `BaseId` immediately even if `AssignCharacterRequest` arrived first!
            if (acMessage.pPlayer &&
                characterComponent.BaseId &&
                characterComponent.BaseId.Id != GameId{} &&
                characterComponent.BaseId.Id != message.FormId &&
                !isOwner)
            {
                CharacterSpawnRequest spawnSync{};
                Serialize(m_world, existingEntity, &spawnSync);
                acMessage.pPlayer->Send(spawnSync);
            }

            return;
        }
    }

    // Deduplicate dynamic scripted ambush spawns (e.g. RFAB_AmbushScript::PlaceActorAtMe producing 0xFF000000 actors)
    // across peers in the same cell & radius within 3s so 2 players fight 1x enemy with 3x HP instead of 2x clones,
    // while strictly preserving multi-actor ambushes from the SAME owner (PlaceActorAtMe(Wolf, 3)) and player summons!
    const bool isTemporarySpawn = (refId.ModId == std::numeric_limits<uint32_t>::max());
    if (isTemporarySpawn && !isPlayer && !message.IsPlayerSummon && message.FormId != GameId{})
    {
        constexpr uint64_t kAmbushDedupWindowMs = 3000;
        constexpr float kAmbushDedupRadiusUnits = RfabCompat::MetersToSkyrimUnits(12.0f);
        const uint64_t nowMs = SteadyClockMs();

        const uint32_t existingServerId = m_world.GetRfabProgressionService().GetLedger().FindMatchingRecentAmbush(
            acMessage.pPlayer->GetId(),
            message.CellId,
            message.FormId,
            message.Position.x,
            message.Position.y,
            message.Position.z,
            nowMs,
            kAmbushDedupWindowMs,
            kAmbushDedupRadiusUnits);

        if (existingServerId != 0)
        {
            const entt::entity existingEntity = static_cast<entt::entity>(existingServerId);
            if (m_world.valid(existingEntity))
            {
                const auto* pChar = m_world.try_get<CharacterComponent>(existingEntity);
                const auto* pAv = m_world.try_get<ActorValuesComponent>(existingEntity);
                const auto* pInv = m_world.try_get<InventoryComponent>(existingEntity);
                const auto* pMove = m_world.try_get<MovementComponent>(existingEntity);
                const auto* pCell = m_world.try_get<CellIdComponent>(existingEntity);
                const auto* pOwner = m_world.try_get<OwnerComponent>(existingEntity);

                if (pChar && pAv && pInv && pMove && pCell)
                {
                    AssignCharacterResponse response{};
                    response.Cookie = message.Cookie;
                    response.ServerId = existingServerId;
                    response.Owner = false;
                    response.OwnershipEpoch = pOwner ? pOwner->Epoch : 0;
                    response.AllActorValues = pAv->CurrentActorValues;
                    response.CurrentInventory = pInv->Content;
                    response.IsDead = pChar->IsDead();
                    response.IsWeaponDrawn = pChar->IsWeaponDrawn();
                    response.PlayerId = pChar->PlayerId;
                    response.Position = pMove->Position;
                    response.CellId = pCell->Cell;
                    response.WorldSpaceId = pCell->WorldSpaceId;
                    if (auto* pAnim = m_world.try_get<AnimationComponent>(existingEntity))
                        response.ActionsToReplay = pAnim->ActionsReplayCache.FormRefinedReplayChain();

                    m_interestRegistry.MarkInRange(acMessage.pPlayer->GetId(), existingServerId);
                    acMessage.pPlayer->Send(response);
                    m_world.GetSaveSnapshotService().GetCoordinator().RecordFlightEvent(
                        nowMs, CoopFlight::FlightEventKind::kAmbushDeduplicated, existingServerId, acMessage.pPlayer->GetId());
                    spdlog::debug(
                        "[RFAB AmbushDedup] Bound peer {} dynamic ambush NPC {:X}:{:X} to existing ServerId {:X}",
                        acMessage.pPlayer->GetId(),
                        message.FormId.ModId,
                        message.FormId.BaseId,
                        existingServerId);
                    return;
                }
            }
        }
    }

    // This entity has no owner create it
    CreateCharacter(acMessage);
}

void CharacterService::OnOwnershipTransferRequest(const PacketEvent<RequestOwnershipTransfer>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);

    if (!m_world.valid(cEntity))
    {
        spdlog::warn("Client {:X} requested ownership transfer of an entity that doesn't exist, server id: {:X}", acMessage.pPlayer->GetConnectionId(), message.ServerId);
        return;
    }

    auto* pCharacterOwnerComponent = m_world.try_get<OwnerComponent>(cEntity);
    if (!pCharacterOwnerComponent)
        return;

    const Player* pCurrentOwner = pCharacterOwnerComponent->GetOwner();
    const uint32_t currentOwnerId = pCurrentOwner ? pCurrentOwner->GetId() : 0;
    const uint32_t senderId = acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0;

    if (!CoopOwnership::ValidateHandoffFromOwner(
            currentOwnerId,
            pCharacterOwnerComponent->Epoch,
            senderId,
            message.OwnershipEpoch))
    {
        // Stale handoff from a former owner after ownership already moved to another peer:
        // reject immediately so we NEVER clobber position/InvalidOwners or destroy a live boss via CharacterRemoveEvent!
        NotifyRelinquishControl relinquish{};
        relinquish.ServerId = message.ServerId;
        relinquish.OwnershipEpoch = pCharacterOwnerComponent->Epoch;
        if (acMessage.pPlayer)
            acMessage.pPlayer->Send(relinquish);

        m_world.GetSaveSnapshotService().GetCoordinator().RecordFlightEvent(
            SteadyClockMs(), CoopFlight::FlightEventKind::kStaleEpochRejected, message.ServerId, senderId);
        spdlog::debug(
            "[Ownership] Rejected stale RequestOwnershipTransfer for {:X} from player {} (pktEpoch={}, authEpoch={}, authOwner={})",
            message.ServerId,
            senderId,
            message.OwnershipEpoch,
            pCharacterOwnerComponent->Epoch,
            currentOwnerId);
        return;
    }

    if (auto* pCharacterComponent = m_world.try_get<CharacterComponent>(cEntity))
    {
        if (pCharacterComponent->IsPlayerSummon())
        {
            spdlog::info("Client {:X} requested ownership transfer of an orphaned summon, serverid id: {:X}", acMessage.pPlayer->GetConnectionId(), message.ServerId);
            m_world.GetDispatcher().trigger(CharacterRemoveEvent(message.ServerId));
            return;
        }
    }

    auto* pPriorMovement = m_world.try_get<MovementComponent>(cEntity);
    const bool hasPriorPos = (pPriorMovement != nullptr);
    const glm::vec3 priorPos = hasPriorPos ? pPriorMovement->Position : glm::vec3{};
    const bool hasValidHandoffCoords = CoopOwnership::IsValidHandoffCoordinates(
        static_cast<bool>(message.WorldSpaceId || message.CellId),
        static_cast<bool>(message.WorldSpaceId),
        message.Position.x,
        message.Position.y,
        message.Position.z,
        hasPriorPos,
        priorPos.x,
        priorPos.y,
        priorPos.z);

    if (hasValidHandoffCoords)
    {
        bool cellChanged = false;
        if (auto* pCellIdComponent = m_world.try_get<CellIdComponent>(cEntity))
        {
            const CellIdComponent prevCell = *pCellIdComponent;
            pCellIdComponent->WorldSpaceId = message.WorldSpaceId;
            pCellIdComponent->Cell = message.CellId;
            if (message.WorldSpaceId)
                pCellIdComponent->CenterCoords = GridCellCoords::CalculateGridCellCoords(message.Position);
            else
                pCellIdComponent->CenterCoords.Reset();
            cellChanged = (*pCellIdComponent != prevCell);
        }

        if (pPriorMovement)
        {
            pPriorMovement->Position = message.Position;
            pPriorMovement->Sent = true;
        }

        if (cellChanged)
            ReconcileEntityForAllObservers(cEntity);

        // Dynamic temporary spawns (0xFFxxxxxx) do not carry FormIdComponent on the server.
        // Only notify peers whose active cell/grid is actually in range of the NPC's new location!
        if (const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cEntity))
        {
            NotifyActorTeleport notify{};
            notify.FormId = pFormIdComponent->Id;
            notify.WorldSpaceId = message.WorldSpaceId;
            notify.CellId = message.CellId;
            notify.Position = message.Position;

            GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer);
        }

        // Normal range/cell relinquish from a live materialized owner: clear stale historical blacklists
        // so previous owners who walked back into range remain eligible (TiltedEvolution 1660eb0c),
        // while keeping cascade blacklists intact when `hasValidHandoffCoords == false` (DeclineGrant).
        pCharacterOwnerComponent->InvalidOwners.clear();
    }

    pCharacterOwnerComponent->InvalidOwners.push_back(acMessage.pPlayer);

    m_world.GetDispatcher().trigger(OwnershipTransferEvent(cEntity));
}

void CharacterService::OnOwnershipTransferEvent(const OwnershipTransferEvent& acEvent) const noexcept
{
    const auto view = m_world.view<OwnerComponent, CharacterComponent, CellIdComponent>();
    if (!view.contains(acEvent.Entity))
        return;

    auto& characterComponent = view.get<CharacterComponent>(acEvent.Entity);
    auto& ownerComponent = view.get<OwnerComponent>(acEvent.Entity);
    auto& cellIdComponent = view.get<CellIdComponent>(acEvent.Entity);

    NotifyOwnershipTransfer response;
    response.ServerId = World::ToInteger(acEvent.Entity);

    Player* pBestStandingCandidate = nullptr;
    Player* pFallbackDownedCandidate = nullptr;
    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (ownerComponent.GetOwner() == pPlayer)
            continue;

        bool isPlayerInvalid = false;
        for (const auto invalidOwner : ownerComponent.InvalidOwners)
        {
            isPlayerInvalid = invalidOwner == pPlayer;
            if (isPlayerInvalid)
                break;
        }

        if (isPlayerInvalid)
            continue;

        if (!pPlayer->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon()))
            continue;

        if (!m_world.GetEncounterService().GetCoordinator().IsPlayerDowned(pPlayer->GetId()))
        {
            pBestStandingCandidate = pPlayer;
            break;
        }

        if (!pFallbackDownedCandidate)
            pFallbackDownedCandidate = pPlayer;
    }

    Player* pSelectedOwner = pBestStandingCandidate ? pBestStandingCandidate : pFallbackDownedCandidate;
    if (pSelectedOwner)
    {
        const uint64_t nowMs = SteadyClockMs();
        response.OwnershipEpoch = ownerComponent.SetOwner(pSelectedOwner, nowMs);
        pSelectedOwner->Send(response);

        m_world.GetSaveSnapshotService().GetCoordinator().RecordFlightEvent(
            nowMs, CoopFlight::FlightEventKind::kOwnershipChanged, response.ServerId, pSelectedOwner->GetId());
        return;
    }

    // SkyMP ServerWorldModel Persistence:
    // Placed world NPCs (carrying FormIdComponent) MUST NOT be destroyed when all players leave their cell/grid!
    // Transition them into a dormant unowned server entity so their battle-damaged HP, dead corpse state,
    // looted inventory, and rolled TESLevCharacter BaseId persist across cell unloads and re-entries.
    if (m_world.all_of<FormIdComponent>(acEvent.Entity) &&
        !characterComponent.IsPlayer() &&
        !characterComponent.IsPlayerSummon())
    {
        const uint64_t nowMs = SteadyClockMs();
        ownerComponent.SetOwner(nullptr, nowMs);
        ownerComponent.InvalidOwners.clear();
        ReconcileEntityForAllObservers(acEvent.Entity);
        PruneExcessDormantCharacters();
        spdlog::debug("Character {:X} transitioned to dormant persistent world state", response.ServerId);
        return;
    }

    m_world.GetDispatcher().trigger(CharacterRemoveEvent(response.ServerId));
}

void CharacterService::OnCharacterRemoveEvent(const CharacterRemoveEvent& acEvent) const noexcept
{
    m_interestRegistry.ForgetEntityEverywhere(acEvent.ServerId);

    const auto view = m_world.view<OwnerComponent>();
    const auto it = view.find(static_cast<entt::entity>(acEvent.ServerId));
    if (it == view.end())
        return;

    const auto& characterOwnerComponent = view.get<OwnerComponent>(*it);

    GameServer::Get()->GetWorld().GetScriptService().HandleCharacterDestoy(*it);

    NotifyRemoveCharacter response;
    response.ServerId = acEvent.ServerId;

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (characterOwnerComponent.GetOwner() == pPlayer)
            continue;

        pPlayer->Send(response);
    }

    m_world.destroy(*it);
    spdlog::debug("Character destroyed {:X}", acEvent.ServerId);
}

void CharacterService::OnOwnershipClaimRequest(const PacketEvent<RequestOwnershipClaim>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    const entt::entity cEntity = static_cast<entt::entity>(acMessage.Packet.ServerId);
    if (!m_world.valid(cEntity))
        return;

    auto* pOwnerComp = m_world.try_get<OwnerComponent>(cEntity);
    const auto* pCharComp = m_world.try_get<CharacterComponent>(cEntity);
    const auto* pCellComp = m_world.try_get<CellIdComponent>(cEntity);
    if (!pOwnerComp || !pCharComp || !pCellComp)
        return;

    const Player* pCurrentOwner = pOwnerComp->GetOwner();
    const bool currentOwnerInRange = pCurrentOwner != nullptr &&
                                     pCurrentOwner->GetCellComponent().IsInRange(*pCellComp, pCharComp->IsDragon());
    const bool currentOwnerDowned = pCurrentOwner != nullptr &&
                                    m_world.GetEncounterService().GetCoordinator().IsPlayerDowned(pCurrentOwner->GetId());
    const bool challengerDowned = m_world.GetEncounterService().GetCoordinator().IsPlayerDowned(acMessage.pPlayer->GetId());
    const uint64_t nowMs = SteadyClockMs();

    const auto decision = CoopOwnership::EvaluateVoluntaryClaim(
        pCurrentOwner ? pCurrentOwner->GetId() : 0,
        acMessage.pPlayer->GetId(),
        pOwnerComp->LastChangedAtMs,
        nowMs,
        pCharComp->IsPlayer(),
        pCharComp->IsPlayerSummon(),
        currentOwnerInRange,
        currentOwnerDowned,
        challengerDowned);

    if (decision != CoopOwnership::ClaimDecision::kAccept && decision != CoopOwnership::ClaimDecision::kAlreadyOwner)
    {
        // Reject optimistic client claim cleanly by issuing NotifyRelinquishControl with current authoritative epoch
        // so the challenger immediately reverts its optimistic LocalComponent -> RemoteComponent (zero split-brain!).
        NotifyRelinquishControl reject{};
        reject.ServerId = acMessage.Packet.ServerId;
        reject.OwnershipEpoch = pOwnerComp->Epoch;
        acMessage.pPlayer->Send(reject);

        m_world.GetSaveSnapshotService().GetCoordinator().RecordFlightEvent(
            nowMs, CoopFlight::FlightEventKind::kOwnershipRejected, acMessage.Packet.ServerId, acMessage.pPlayer->GetId());
        spdlog::debug(
            "[Ownership] Rejected claim on {:X} from player {} (reason={})",
            acMessage.Packet.ServerId,
            acMessage.pPlayer->GetId(),
            static_cast<uint32_t>(decision));
        return;
    }

    ActorData sanitizedData = acMessage.Packet.NewActorData;
    bool hasServerHealth = false;
    float serverHealth = 0.0f;
    float serverMaxHealth = 0.0f;
    if (const auto* pAvComp = m_world.try_get<ActorValuesComponent>(cEntity))
    {
        const auto hpIt = pAvComp->CurrentActorValues.ActorValuesList.find(kHealthActorValueIndex);
        if (hpIt != pAvComp->CurrentActorValues.ActorValuesList.end())
        {
            hasServerHealth = true;
            serverHealth = hpIt->second;
        }
        const auto maxHpIt = pAvComp->CurrentActorValues.ActorMaxValuesList.find(kHealthActorValueIndex);
        if (maxHpIt != pAvComp->CurrentActorValues.ActorMaxValuesList.end())
            serverMaxHealth = maxHpIt->second;
    }

    auto claimedHpIt = sanitizedData.InitialActorValues.ActorValuesList.find(kHealthActorValueIndex);
    auto claimedMaxHpIt = sanitizedData.InitialActorValues.ActorMaxValuesList.find(kHealthActorValueIndex);
    const float rawClaimedHealth = (claimedHpIt != sanitizedData.InitialActorValues.ActorValuesList.end()) ? claimedHpIt->second : serverHealth;
    const float rawClaimedMaxHealth = (claimedMaxHpIt != sanitizedData.InitialActorValues.ActorMaxValuesList.end()) ? claimedMaxHpIt->second : serverMaxHealth;
    float claimedHealth = rawClaimedHealth;
    float claimedMaxHealth = rawClaimedMaxHealth;
    const bool rawClaimedDead = sanitizedData.IsDead;
    CoopOwnership::ReconcileClaimedStateWithMax(
        pCharComp->IsDead(),
        serverHealth,
        serverMaxHealth,
        hasServerHealth,
        sanitizedData.IsDead,
        claimedHealth,
        claimedMaxHealth);
    if (claimedHpIt != sanitizedData.InitialActorValues.ActorValuesList.end() || hasServerHealth)
        sanitizedData.InitialActorValues.ActorValuesList[kHealthActorValueIndex] = claimedHealth;
    if (claimedMaxHealth > 0.0f && (claimedMaxHpIt != sanitizedData.InitialActorValues.ActorMaxValuesList.end() || serverMaxHealth > 0.0f))
        sanitizedData.InitialActorValues.ActorMaxValuesList[kHealthActorValueIndex] = claimedMaxHealth;

    TransferOwnership(acMessage.pPlayer, acMessage.Packet.ServerId, sanitizedData);

    // If server reconciliation clamped claimed HP down to battle-damaged server HP or enforced dead state,
    // send the authoritative state back to the claiming owner (since BroadcastActorData excludes apPlayer).
    if (sanitizedData.IsDead != rawClaimedDead || std::fabs(claimedHealth - rawClaimedHealth) > 0.01f)
    {
        NotifySpawnData reconcileNotify{};
        reconcileNotify.Id = acMessage.Packet.ServerId;
        reconcileNotify.NewActorData = sanitizedData;
        acMessage.pPlayer->Send(reconcileNotify);
    }
}

void CharacterService::OnCharacterSpawned(const CharacterSpawnedEvent& acEvent) const noexcept
{
    ReconcileEntityForAllObservers(acEvent.Entity);

    // For human player avatars, also seed initial RemotePlayerModel metadata on out-of-range connected peers
    // (leaving m_interestRegistry unmarked so a fresh CharacterSpawnRequest is still sent upon entering range).
    if (const auto* pCharComp = m_world.try_get<CharacterComponent>(acEvent.Entity);
        pCharComp && pCharComp->IsPlayer())
    {
        const uint32_t serverId = World::ToInteger(acEvent.Entity);
        const auto* pOwnerComp = m_world.try_get<OwnerComponent>(acEvent.Entity);
        Player* const pOwner = pOwnerComp ? pOwnerComp->GetOwner() : nullptr;

        CharacterSpawnRequest message{};
        Serialize(m_world, acEvent.Entity, &message);

        for (Player* pPeer : m_world.GetPlayerManager())
        {
            if (!pPeer || pPeer == pOwner)
                continue;
            if (!m_interestRegistry.IsMarkedInRange(pPeer->GetId(), serverId))
                pPeer->Send(message);
        }
    }

    GameServer::Get()->GetWorld().GetScriptService().HandleCharacterSpawn(acEvent.Entity);
}

void CharacterService::OnReferencesMoveRequest(const PacketEvent<ClientReferencesMoveRequest>& acMessage) const noexcept
{
    OwnerView<AnimationComponent, MovementComponent, CellIdComponent, CharacterComponent> view(m_world, acMessage.GetSender());

    auto& message = acMessage.Packet;
    TiltedPhoques::Map<Player*, ServerReferencesMoveRequest> immediateRelays;
    TiltedPhoques::Map<Player*, bool> relayHasActions;

    for (auto& entry : message.Updates)
    {
        const auto entity = static_cast<entt::entity>(entry.first);

        auto itor = view.find(entity);
        if (itor == std::end(view))
        {
            spdlog::debug("{:x} requested move of {:x} but is not the owner or entity does not exist", acMessage.pPlayer->GetConnectionId(), entry.first);
            continue;
        }

        auto& movementComponent = view.get<MovementComponent>(*itor);
        auto& cellIdComponent = view.get<CellIdComponent>(*itor);
        auto& animationComponent = view.get<AnimationComponent>(*itor);
        const auto& characterComponent = view.get<CharacterComponent>(*itor);

        movementComponent.Tick = message.Tick;

        const CellIdComponent previousCellComp = cellIdComponent;

        auto& update = entry.second;
        auto& movement = update.UpdatedMovement;

        if (!std::isfinite(movement.Position.x) || !std::isfinite(movement.Position.y) || !std::isfinite(movement.Position.z) ||
            std::fabs(movement.Position.x) > 4.0e6f || std::fabs(movement.Position.y) > 4.0e6f || std::fabs(movement.Position.z) > 4.0e6f ||
            !std::isfinite(movement.Rotation.x) || !std::isfinite(movement.Rotation.y) || !std::isfinite(movement.Direction))
        {
            spdlog::warn("Rejected non-finite/out-of-bounds movement packet for {:X} from {:X}", entry.first, acMessage.pPlayer->GetConnectionId());
            continue;
        }

        movementComponent.Position = movement.Position;
        movementComponent.Rotation = glm::vec3(movement.Rotation.x, 0.f, movement.Rotation.y);
        movementComponent.Variables = movement.Variables;
        movementComponent.Direction = movement.Direction;

        if (movement.WorldSpaceId || movement.CellId)
        {
            cellIdComponent.Cell = movement.CellId;
            cellIdComponent.WorldSpaceId = movement.WorldSpaceId;
            if (movement.WorldSpaceId)
                cellIdComponent.CenterCoords = GridCellCoords::CalculateGridCellCoords(movement.Position.x, movement.Position.y);
            else
                cellIdComponent.CenterCoords.Reset();
        }
        else if (cellIdComponent.WorldSpaceId)
        {
            cellIdComponent.CenterCoords = GridCellCoords::CalculateGridCellCoords(movement.Position.x, movement.Position.y);
        }

        if (cellIdComponent != previousCellComp)
            ReconcileEntityForAllObservers(entity);

        Vector<ActionEvent> acceptedActions;
        acceptedActions.reserve(update.ActionEvents.size());
        for (auto& action : update.ActionEvents)
        {
            auto [canceled, reason] = GameServer::Get()->GetWorld().GetScriptService().HandleCharacterMove(entity);
            if (canceled)
                continue;

            animationComponent.CurrentAction = action;
            acceptedActions.push_back(animationComponent.CurrentAction);
        }

        animationComponent.ActionsReplayCache.AppendAll(acceptedActions);

        // Relay immediately with the owner's exact capture tick so PlaybackClock sees true network transit
        // without 20ms server polling jitter or 5ms Nagle delay (keeping action-bearing packets reliable).
        for (Player* pObserver : m_world.GetPlayerManager())
        {
            if (!pObserver || pObserver == acMessage.pPlayer)
                continue;

            if (!cellIdComponent.IsInRange(pObserver->GetCellComponent(), characterComponent.IsDragon()))
                continue;

            auto& relay = immediateRelays[pObserver];
            relay.Tick = message.Tick;
            auto& outUpdate = relay.Updates[World::ToInteger(entity)];
            outUpdate.UpdatedMovement.CellId = cellIdComponent.Cell;
            outUpdate.UpdatedMovement.WorldSpaceId = cellIdComponent.WorldSpaceId;
            outUpdate.UpdatedMovement.Position = movementComponent.Position;
            outUpdate.UpdatedMovement.Rotation.x = movementComponent.Rotation.x;
            outUpdate.UpdatedMovement.Rotation.y = movementComponent.Rotation.z;
            outUpdate.UpdatedMovement.Direction = movementComponent.Direction;
            outUpdate.UpdatedMovement.Variables = movementComponent.Variables;
            outUpdate.ActionEvents = acceptedActions;

            if (!acceptedActions.empty())
                relayHasActions[pObserver] = true;
        }

        movementComponent.Sent = true;
    }

    for (auto& [pObserver, relay] : immediateRelays)
    {
        if (relay.Updates.empty())
            continue;
        const bool hasActions = relayHasActions.contains(pObserver) && relayHasActions[pObserver];
        pObserver->Send(relay, hasActions ? TiltedPhoques::kReliable : TiltedPhoques::kUnreliableNoNagle);
    }
}

void CharacterService::OnFactionsChanges(const PacketEvent<RequestFactionsChanges>& acMessage) const noexcept
{
    OwnerView<CharacterComponent> view(m_world, acMessage.GetSender());

    auto& message = acMessage.Packet;

    for (auto& [id, factions] : message.Changes)
    {
        auto it = view.find(static_cast<entt::entity>(id));

        if (it == std::end(view) || view.get<OwnerComponent>(*it).GetOwner() != acMessage.pPlayer)
            continue;

        auto& characterComponent = view.get<CharacterComponent>(*it);
        characterComponent.FactionsContent = factions;
        characterComponent.SetDirtyFactions(true);
    }
}

void CharacterService::OnMountRequest(const PacketEvent<MountRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    auto& message = acMessage.Packet;

    const entt::entity riderEntity = static_cast<entt::entity>(message.RiderId);
    const entt::entity mountEntity = static_cast<entt::entity>(message.MountId);
    if (!m_world.valid(riderEntity) || !m_world.valid(mountEntity))
        return;

    // Verify that the sender actually owns the rider entity, then atomically transfer mount ownership
    // to the rider so horse movement updates belong to the player physically riding the mount!
    const auto* pRiderOwner = m_world.try_get<OwnerComponent>(riderEntity);
    if (pRiderOwner && pRiderOwner->GetOwner() != acMessage.pPlayer)
        return;

    if (auto* pMountOwner = m_world.try_get<OwnerComponent>(mountEntity))
    {
        const auto* pMountChar = m_world.try_get<CharacterComponent>(mountEntity);
        if (pMountChar && !pMountChar->IsPlayer() && pMountOwner->GetOwner() != acMessage.pPlayer)
        {
            const ActorData mountData = BuildActorData(mountEntity);
            TransferOwnership(acMessage.pPlayer, message.MountId, mountData);

            NotifyOwnershipTransfer grant{};
            grant.ServerId = message.MountId;
            grant.OwnershipEpoch = pMountOwner->Epoch;
            acMessage.pPlayer->Send(grant);
        }
    }

    NotifyMount notify;
    notify.RiderId = message.RiderId;
    notify.MountId = message.MountId;

    if (!GameServer::Get()->SendToPlayersInRange(notify, mountEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnNewPackageRequest(const PacketEvent<NewPackageRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    NotifyNewPackage notify;
    notify.ActorId = message.ActorId;
    notify.PackageId = message.PackageId;

    const entt::entity cEntity = static_cast<entt::entity>(message.ActorId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnRequestRespawn(const PacketEvent<RequestRespawn>& acMessage) const noexcept
{
    auto view = m_world.view<OwnerComponent, CharacterComponent>();
    auto it = view.find(static_cast<entt::entity>(acMessage.Packet.ActorId));
    if (it == view.end())
    {
        spdlog::warn("No OwnerComponent found for actor id {:X}", acMessage.Packet.ActorId);
        return;
    }

    auto& ownerComponent = view.get<OwnerComponent>(*it);

    if (ownerComponent.GetOwner() == acMessage.pPlayer)
    {
        // Reset per-corpse anti-dupe withdrawal history when an NPC naturally respawns!
        m_world.GetInventoryService().ForgetContainerForEntity(*it, acMessage.Packet.ActorId);

        // Replay cache needs to be cleared when the authoritative owner respawns a character
        if (auto* pAnimationComponent = m_world.try_get<AnimationComponent>(*it))
            pAnimationComponent->ActionsReplayCache.Clear();

        if (!acMessage.Packet.AppearanceBuffer.empty())
        {
            auto& characterComponent = view.get<CharacterComponent>(*it);
            characterComponent.SaveBuffer = acMessage.Packet.AppearanceBuffer;
            characterComponent.ChangeFlags = acMessage.Packet.ChangeFlags;
        }

        NotifyRespawn notify;
        notify.ActorId = acMessage.Packet.ActorId;

        if (!GameServer::Get()->SendToPlayersInRange(notify, *it, acMessage.GetSender()))
            spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
    }
    else if (acMessage.GetSender())
    {
        CharacterSpawnRequest message{};
        Serialize(m_world, *it, &message);

        acMessage.GetSender()->Send(message);
    }
}

void CharacterService::OnSyncExperienceRequest(const PacketEvent<SyncExperienceRequest>& acMessage) const noexcept
{
    if (!bEnableXpSync)
        return;

    const float sanitizedXp = CoopPacketSafety::SanitizeFiniteFloat(acMessage.Packet.Experience, 0.0f, 1'000'000.0f, 0.0f);
    if (sanitizedXp <= 0.0f)
        return;

    NotifySyncExperience notify{};
    notify.Experience = sanitizedXp;

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    if (partyComponent.JoinedPartyId.has_value())
        GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
    else
        GameServer::Get()->SendToPlayers(notify, acMessage.GetSender());
}

void CharacterService::OnSyncRfabExperienceRequest(const PacketEvent<SyncRfabExperienceRequest>& acMessage) const noexcept
{
    constexpr uint32_t cMaxXpDelta = 1'000'000;

    if (!bEnableRfabXpSync)
        return;

    const uint32_t experience = std::min(acMessage.Packet.Experience, cMaxXpDelta);
    if (experience == 0)
        return;

    NotifySyncRfabExperience notify;
    notify.Experience = experience;

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    if (partyComponent.JoinedPartyId.has_value())
        GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
    else
        GameServer::Get()->SendToPlayers(notify, acMessage.GetSender());
}

void CharacterService::OnDialogueRequest(const PacketEvent<DialogueRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    if (!m_world.valid(cEntity))
        return;

    if (const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity))
    {
        if (pOwner->GetOwner() != acMessage.pPlayer)
            return;
    }

    NotifyDialogue notify{};
    notify.ServerId = message.ServerId;
    notify.SoundFilename = message.SoundFilename;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnSubtitleRequest(const PacketEvent<SubtitleRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    if (!m_world.valid(cEntity))
        return;

    if (const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity))
    {
        if (pOwner->GetOwner() != acMessage.pPlayer)
            return;
    }

    NotifySubtitle notify{};
    notify.ServerId = message.ServerId;
    notify.Text = message.Text;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::CreateCharacter(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    const auto gameId = message.ReferenceId;
    const auto baseId = message.FormId;

    const auto cEntity = m_world.create();
    const auto isTemporary = gameId.ModId == std::numeric_limits<uint32_t>::max();
    const auto isPlayer = (gameId.ModId == 0 && gameId.BaseId == 0x14);
    const auto isCustom = isPlayer || isTemporary;

    // For player characters and temporary forms
    if (!isCustom)
    {
        m_world.emplace<FormIdComponent>(cEntity, gameId.BaseId, gameId.ModId);
    }
    else if (baseId != GameId{} && !isTemporary)
    {
        m_world.destroy(cEntity);
        spdlog::warn("Unexpected NpcId, player {:x} might be forging packets", acMessage.pPlayer->GetConnectionId());
        return;
    }

    auto* const pServer = GameServer::Get();
    const uint64_t spawnNowMs = SteadyClockMs();

    auto& ownerComponent = m_world.emplace<OwnerComponent>(cEntity, acMessage.pPlayer, spawnNowMs);

    auto& cellIdComponent = m_world.emplace<CellIdComponent>(cEntity, message.CellId);
    if (message.WorldSpaceId != GameId{})
    {
        cellIdComponent.WorldSpaceId = message.WorldSpaceId;
        cellIdComponent.CenterCoords = GridCellCoords::CalculateGridCellCoords(message.Position);
    }

    auto& characterComponent = m_world.emplace<CharacterComponent>(cEntity);
    characterComponent.ChangeFlags = message.ChangeFlags;
    characterComponent.SaveBuffer = std::move(message.AppearanceBuffer);
    characterComponent.BaseId = FormIdComponent(message.FormId);
    characterComponent.FaceTints = message.FaceTints;
    characterComponent.FactionsContent = message.FactionsContent;
    characterComponent.SetDead(message.CurrentActorData.IsDead);
    characterComponent.SetPlayer(isPlayer);
    characterComponent.SetWeaponDrawn(message.CurrentActorData.IsWeaponDrawn);
    characterComponent.SetDragon(message.IsDragon);
    characterComponent.SetMount(message.IsMount);
    characterComponent.SetPlayerSummon(message.IsPlayerSummon);

    auto& inventoryComponent = m_world.emplace<InventoryComponent>(cEntity);
    inventoryComponent.Content = message.CurrentActorData.InitialInventory;

    auto& actorValuesComponent = m_world.emplace<ActorValuesComponent>(cEntity);
    actorValuesComponent.CurrentActorValues = message.CurrentActorData.InitialActorValues;

    spdlog::debug("FormId: {:x}:{:x} - NpcId: {:x}:{:x} assigned to {:x}", gameId.ModId, gameId.BaseId, baseId.ModId, baseId.BaseId, acMessage.pPlayer->GetConnectionId());

    auto& movementComponent = m_world.emplace<MovementComponent>(cEntity);
    movementComponent.Tick = pServer->GetTick();
    movementComponent.Position = message.Position;
    movementComponent.Rotation = {message.Rotation.x, 0.f, message.Rotation.y};
    movementComponent.Sent = false;

    m_world.emplace<AnimationComponent>(cEntity);

    // If this is a player character store a ref and trigger an event
    if (isPlayer)
    {
        const auto pPlayer = acMessage.pPlayer;

        pPlayer->SetCharacter(cEntity);
        if (!pPlayer->GetCellComponent().IsValid() && cellIdComponent.IsValid())
            pPlayer->SetCellComponent(cellIdComponent);
        pPlayer->GetQuestLogComponent().QuestContent = message.QuestContent;
        characterComponent.PlayerId = pPlayer->GetId();

        auto& dispatcher = m_world.GetDispatcher();
        dispatcher.trigger(PlayerEnterWorldEvent(pPlayer));
    }
    else if (message.IsPlayerSummon && acMessage.pPlayer)
    {
        // Record the summoner's PlayerId so remote clients bind commandingActor to the remote summoner (never 0x14).
        characterComponent.PlayerId = acMessage.pPlayer->GetId();
    }

    AssignCharacterResponse response{};
    response.Cookie = message.Cookie;
    response.ServerId = World::ToInteger(cEntity);
    response.PlayerId = characterComponent.PlayerId;
    response.Owner = true;
    response.OwnershipEpoch = ownerComponent.Epoch;

    if (isTemporary && !isPlayer && !message.IsPlayerSummon && baseId != GameId{})
    {
        constexpr uint64_t kAmbushDedupWindowMs = 3000;
        m_world.GetRfabProgressionService().GetLedger().RegisterAmbushSpawn(
            World::ToInteger(cEntity),
            acMessage.pPlayer->GetId(),
            message.CellId,
            baseId,
            message.Position.x,
            message.Position.y,
            message.Position.z,
            spawnNowMs,
            kAmbushDedupWindowMs);
    }

    pServer->Send(acMessage.pPlayer->GetConnectionId(), response);

    auto& dispatcher = m_world.GetDispatcher();
    dispatcher.trigger(CharacterSpawnedEvent(cEntity));

    // Guarantee symmetric mutual visibility & persistent RemotePlayerModel registration on join/reload:
    // ReconcileAllEntitiesForObserver replicates all in-range NPCs/summons/players to `acMessage.pPlayer`,
    // and we also seed out-of-range human player models so `acMessage.pPlayer` knows all connected peers immediately.
    if (isPlayer && acMessage.pPlayer)
    {
        ReconcileAllEntitiesForObserver(acMessage.pPlayer);

        auto existingPlayersView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();
        for (auto otherEntity : existingPlayersView)
        {
            if (otherEntity == cEntity)
                continue;

            const auto& otherChar = existingPlayersView.get<CharacterComponent>(otherEntity);
            const auto& otherOwner = existingPlayersView.get<OwnerComponent>(otherEntity);
            if (!otherChar.IsPlayer() || otherOwner.GetOwner() == acMessage.pPlayer)
                continue;

            const uint32_t otherServerId = World::ToInteger(otherEntity);
            if (!m_interestRegistry.IsMarkedInRange(acMessage.pPlayer->GetId(), otherServerId))
            {
                CharacterSpawnRequest spawnMessage{};
                Serialize(m_world, otherEntity, &spawnMessage);
                acMessage.pPlayer->Send(spawnMessage);
            }
        }
    }
}

void CharacterService::TransferOwnership(Player* apPlayer, const uint32_t acServerId,
                                         const ActorData& acActorData) const noexcept
{
    if (!apPlayer)
        return;

    auto view = m_world.view<OwnerComponent>();
    const auto it = view.find(static_cast<entt::entity>(acServerId));
    if (it == view.end())
    {
        spdlog::warn("Client {:X} requested ownership of an entity that doesn't exist ({:X})!", apPlayer->GetConnectionId(), acServerId);
        return;
    }

    auto& characterOwnerComponent = view.get<OwnerComponent>(*it);
    Player* pPreviousOwner = characterOwnerComponent.GetOwner();
    const uint64_t nowMs = SteadyClockMs();
    const uint64_t newEpoch = characterOwnerComponent.SetOwner(apPlayer, nowMs);
    characterOwnerComponent.InvalidOwners.clear();

    m_interestRegistry.UnmarkInRange(apPlayer->GetId(), acServerId);

    if (pPreviousOwner && pPreviousOwner != apPlayer)
    {
        if (const auto* pCellComp = m_world.try_get<CellIdComponent>(*it))
        {
            const auto* pCharComp = m_world.try_get<CharacterComponent>(*it);
            if (pCellComp->IsInRange(pPreviousOwner->GetCellComponent(), pCharComp ? pCharComp->IsDragon() : false))
                m_interestRegistry.MarkInRange(pPreviousOwner->GetId(), acServerId);
        }

        NotifyRelinquishControl notify{};
        notify.ServerId = acServerId;
        notify.OwnershipEpoch = newEpoch;
        pPreviousOwner->Send(notify);
    }

    BroadcastActorData(apPlayer, *it, acActorData);
    m_world.GetSaveSnapshotService().GetCoordinator().RecordFlightEvent(
        nowMs, CoopFlight::FlightEventKind::kOwnershipChanged, acServerId, apPlayer->GetId());

    spdlog::debug("\tOwnership claimed {:X} (epoch {})", acServerId, newEpoch);
}

ActorData CharacterService::BuildActorData(const entt::entity acEntity) const noexcept
{
    ActorData actorData{};

    const auto* pActorValuesComponent = m_world.try_get<ActorValuesComponent>(acEntity);
    if (pActorValuesComponent)
    {
        actorData.InitialActorValues = pActorValuesComponent->CurrentActorValues;
    }

    const auto* pInventoryComponent = m_world.try_get<InventoryComponent>(acEntity);
    if (pInventoryComponent)
    {
        actorData.InitialInventory = pInventoryComponent->Content;
    }

    actorData.IsDead = false;
    const auto* pCharacterComponent = m_world.try_get<CharacterComponent>(acEntity);
    if (pCharacterComponent)
    {
        actorData.IsDead = pCharacterComponent->IsDead();
        actorData.IsWeaponDrawn = pCharacterComponent->IsWeaponDrawn();
    }

    return actorData;
}

void CharacterService::ApplyActorData(const entt::entity acEntity, const ActorData& acActorData) const noexcept
{
    auto* pActorValuesComponent = m_world.try_get<ActorValuesComponent>(acEntity);
    if (pActorValuesComponent)
    {
        pActorValuesComponent->CurrentActorValues = acActorData.InitialActorValues;
    }

    auto* pInventoryComponent = m_world.try_get<InventoryComponent>(acEntity);
    if (pInventoryComponent)
    {
        pInventoryComponent->Content = acActorData.InitialInventory;
    }

    auto* pCharacterComponent = m_world.try_get<CharacterComponent>(acEntity);
    if (pCharacterComponent)
    {
        pCharacterComponent->SetDead(acActorData.IsDead);
        pCharacterComponent->SetWeaponDrawn(acActorData.IsWeaponDrawn);
    }
}

void CharacterService::BroadcastActorData(Player* apPlayer, const entt::entity acEntity,
                                          const ActorData& acActorData) const noexcept
{
    ApplyActorData(acEntity, acActorData);

    NotifySpawnData notifySpawnData;
    notifySpawnData.Id = World::ToInteger(acEntity);
    notifySpawnData.NewActorData = acActorData;

    GameServer::Get()->SendToPlayersInRange(notifySpawnData, acEntity, apPlayer);
}

void CharacterService::ProcessFactionsChanges() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 2000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    const auto characterView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();

    TiltedPhoques::Map<Player*, NotifyFactionsChanges> messages;

    for (auto entity : characterView)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(entity);
        auto& cellIdComponent = characterView.get<CellIdComponent>(entity);
        auto& ownerComponent = characterView.get<OwnerComponent>(entity);

        // If we have nothing new to send skip this
        if (!characterComponent.IsDirtyFactions())
            continue;

        for (auto pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer == ownerComponent.GetOwner())
                continue;

            if (!cellIdComponent.IsInRange(pPlayer->GetCellComponent(), characterComponent.IsDragon()))
                continue;

            auto& message = messages[pPlayer];
            auto& change = message.Changes[World::ToInteger(entity)];

            change = characterComponent.FactionsContent;
        }

        characterComponent.SetDirtyFactions(false);
    }

    for (auto [pPlayer, message] : messages)
    {
        if (!message.Changes.empty())
            pPlayer->Send(message);
    }
}

void CharacterService::ProcessMovementChanges() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 1000ms / 50;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    const auto characterView = m_world.view<CharacterComponent, CellIdComponent, MovementComponent, AnimationComponent, OwnerComponent>();

    TiltedPhoques::Map<Player*, ServerReferencesMoveRequest> messages;

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        auto& message = messages[pPlayer];

        message.Tick = GameServer::Get()->GetTick();
    }

    for (auto entity : characterView)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(entity);
        auto& movementComponent = characterView.get<MovementComponent>(entity);
        auto& cellIdComponent = characterView.get<CellIdComponent>(entity);
        auto& ownerComponent = characterView.get<OwnerComponent>(entity);
        auto& animationComponent = characterView.get<AnimationComponent>(entity);

        // If we have nothing new to send skip this
        if (movementComponent.Sent == true)
            continue;

        for (auto pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer == ownerComponent.GetOwner())
                continue;

            if (!cellIdComponent.IsInRange(pPlayer->GetCellComponent(), characterComponent.IsDragon()))
                continue;

            auto& message = messages[pPlayer];
            auto& update = message.Updates[World::ToInteger(entity)];
            auto& movement = update.UpdatedMovement;

            movement.CellId = cellIdComponent.Cell;
            movement.WorldSpaceId = cellIdComponent.WorldSpaceId;
            movement.Position = movementComponent.Position;

            movement.Rotation.x = movementComponent.Rotation.x;
            movement.Rotation.y = movementComponent.Rotation.z;

            movement.Direction = movementComponent.Direction;
            movement.Variables = movementComponent.Variables;

            update.ActionEvents = animationComponent.Actions;
        }
    }

    m_world.view<AnimationComponent>().each([](AnimationComponent& animationComponent)
    {
        // Remove actions we've sent
        animationComponent.Actions.clear();
    });

    m_world.view<MovementComponent>().each([](MovementComponent& movementComponent) { movementComponent.Sent = true; });

    for (auto& [pPlayer, message] : messages)
    {
        if (!message.Updates.empty())
        {
            bool hasActions = false;
            for (const auto& [_, upd] : message.Updates)
            {
                if (!upd.ActionEvents.empty())
                {
                    hasActions = true;
                    break;
                }
            }
            pPlayer->Send(message, hasActions ? TiltedPhoques::kReliable : TiltedPhoques::kUnreliableNoNagle);
        }
    }
}

void CharacterService::WakeDormantCharactersInActiveRanges() const noexcept
{
    static std::chrono::steady_clock::time_point s_lastWakeSweep{};
    const auto nowTp = std::chrono::steady_clock::now();
    if (nowTp - s_lastWakeSweep < std::chrono::milliseconds(1000))
        return;
    s_lastWakeSweep = nowTp;

    const uint64_t nowMs = SteadyClockMs();
    auto view = m_world.view<FormIdComponent, CellIdComponent, CharacterComponent, OwnerComponent>();
    for (auto entity : view)
    {
        auto& ownerComp = view.get<OwnerComponent>(entity);
        if (ownerComp.GetOwner() != nullptr)
            continue;
        if (nowMs >= ownerComp.LastChangedAtMs && (nowMs - ownerComp.LastChangedAtMs) < 1000ULL)
            continue;

        const auto& charComp = view.get<CharacterComponent>(entity);
        if (charComp.IsPlayer() || charComp.IsPlayerSummon() || charComp.IsMount())
            continue;

        const auto& cellComp = view.get<CellIdComponent>(entity);
        Player* pBestObserver = nullptr;
        for (Player* pCandidate : m_world.GetPlayerManager())
        {
            if (!pCandidate)
                continue;
            if (m_world.GetEncounterService().GetCoordinator().IsPlayerDowned(pCandidate->GetId()))
                continue;
            if (!pCandidate->GetCellComponent().IsInRange(cellComp, charComp.IsDragon()))
                continue;

            pBestObserver = pCandidate;
            if (m_world.GetPartyService().IsPlayerLeader(pCandidate))
                break; // Prefer party leader when multiple living observers are in range
        }

        if (!pBestObserver)
            continue;

        const uint32_t serverId = World::ToInteger(entity);
        if (!m_interestRegistry.IsMarkedInRange(pBestObserver->GetId(), serverId))
        {
            CharacterSpawnRequest spawnMsg{};
            Serialize(m_world, entity, &spawnMsg);
            pBestObserver->Send(spawnMsg);
        }

        const uint64_t newEpoch = ownerComp.SetOwner(pBestObserver, nowMs);
        ownerComp.InvalidOwners.clear();
        m_interestRegistry.UnmarkInRange(pBestObserver->GetId(), serverId);

        NotifyOwnershipTransfer grant{};
        grant.ServerId = serverId;
        grant.OwnershipEpoch = newEpoch;
        pBestObserver->Send(grant);

        m_world.GetSaveSnapshotService().GetCoordinator().RecordFlightEvent(
            nowMs, CoopFlight::FlightEventKind::kOwnershipChanged, serverId, pBestObserver->GetId());
    }
}

void CharacterService::PruneExcessDormantCharacters() const noexcept
{
    constexpr size_t kMaxDormantPersistentNpcs = 4096;

    size_t dormantCount = 0;
    entt::entity oldestPrunable = entt::null;
    uint64_t oldestChangedMs = std::numeric_limits<uint64_t>::max();

    auto view = m_world.view<FormIdComponent, OwnerComponent, CharacterComponent>();
    for (auto entity : view)
    {
        const auto& ownerComp = view.get<OwnerComponent>(entity);
        if (ownerComp.GetOwner() != nullptr)
            continue;

        ++dormantCount;

        const auto& charComp = view.get<CharacterComponent>(entity);
        if (charComp.IsDead() || charComp.IsDragon() || charComp.IsPlayer())
            continue;

        bool isDamaged = false;
        if (const auto* pAv = m_world.try_get<ActorValuesComponent>(entity))
        {
            const auto hpIt = pAv->CurrentActorValues.ActorValuesList.find(kHealthActorValueIndex);
            const auto maxIt = pAv->CurrentActorValues.ActorMaxValuesList.find(kHealthActorValueIndex);
            if (hpIt != pAv->CurrentActorValues.ActorValuesList.end() &&
                maxIt != pAv->CurrentActorValues.ActorMaxValuesList.end() &&
                maxIt->second > 0.0f &&
                hpIt->second + 0.5f < maxIt->second)
            {
                isDamaged = true;
            }
        }
        if (isDamaged)
            continue;

        if (ownerComp.LastChangedAtMs < oldestChangedMs)
        {
            oldestChangedMs = ownerComp.LastChangedAtMs;
            oldestPrunable = entity;
        }
    }

    if (dormantCount > kMaxDormantPersistentNpcs && oldestPrunable != entt::null)
    {
        m_world.GetDispatcher().trigger(CharacterRemoveEvent(World::ToInteger(oldestPrunable)));
    }
}
