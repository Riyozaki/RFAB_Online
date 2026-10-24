#include <Services/CharacterService.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/CharacterSpawnedEvent.h>
#include <Events/CharacterExteriorCellChangeEvent.h>
#include <Events/CharacterInteriorCellChangeEvent.h>
#include <Events/PlayerEnterWorldEvent.h>
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
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&CharacterService::OnUpdate>(this))
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
    }

    auto& animationComponent = aRegistry.get<AnimationComponent>(aEntity);
    apSpawnRequest->ActionsToReplay = animationComponent.ActionsReplayCache.FormRefinedReplayChain();
}

void CharacterService::OnUpdate(const UpdateEvent&) const noexcept
{
    ProcessFactionsChanges();
    ProcessMovementChanges();
}

void CharacterService::OnCharacterExteriorCellChange(const CharacterExteriorCellChangeEvent& acEvent) const noexcept
{
    CharacterSpawnRequest spawnMessage;
    Serialize(m_world, acEvent.Entity, &spawnMessage);

    NotifyRemoveCharacter removeMessage;
    removeMessage.ServerId = World::ToInteger(acEvent.Entity);

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (acEvent.Owner == pPlayer)
            continue;

        if (pPlayer->GetCellComponent().WorldSpaceId != acEvent.WorldSpaceId || pPlayer->GetCellComponent().WorldSpaceId == acEvent.WorldSpaceId && !GridCellCoords::IsCellInGridCell(acEvent.CurrentCoords, pPlayer->GetCellComponent().CenterCoords, false))
        {
            pPlayer->Send(removeMessage);
        }
        else if (pPlayer->GetCellComponent().WorldSpaceId == acEvent.WorldSpaceId && GridCellCoords::IsCellInGridCell(acEvent.CurrentCoords, pPlayer->GetCellComponent().CenterCoords, false))
        {
            pPlayer->Send(spawnMessage);
        }
    }
}

void CharacterService::OnCharacterInteriorCellChange(const CharacterInteriorCellChangeEvent& acEvent) const noexcept
{
    CharacterSpawnRequest spawnMessage;
    Serialize(m_world, acEvent.Entity, &spawnMessage);

    NotifyRemoveCharacter removeMessage;
    removeMessage.ServerId = World::ToInteger(acEvent.Entity);

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (acEvent.Owner == pPlayer)
            continue;

        if (acEvent.NewCell == pPlayer->GetCellComponent().Cell)
            pPlayer->Send(spawnMessage);
        else
            pPlayer->Send(removeMessage);
    }
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
        // Look for the character
        auto view = m_world.view<FormIdComponent, ActorValuesComponent, CharacterComponent, MovementComponent, CellIdComponent, OwnerComponent, InventoryComponent>();

        const auto itor = std::find_if(
            std::begin(view), std::end(view),
            [view, refId](auto entity)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);

                return formIdComponent.Id == refId;
            });

        if (itor != std::end(view))
        {
            // This entity already has an owner
            spdlog::debug("FormId: {:x}:{:x} is already managed", refId.ModId, refId.BaseId);

            auto& actorValuesComponent = view.get<ActorValuesComponent>(*itor);
            auto& inventoryComponent = view.get<InventoryComponent>(*itor);
            auto& characterComponent = view.get<CharacterComponent>(*itor);
            auto& movementComponent = view.get<MovementComponent>(*itor);
            auto& cellIdComponent = view.get<CellIdComponent>(*itor);
            auto& ownerComponent = view.get<OwnerComponent>(*itor);

            auto& partyService = m_world.GetPartyService();

            bool isOwner = false;

            if (partyService.IsPlayerInParty(acMessage.pPlayer) && !characterComponent.IsMount())
            {
                PartyService::Party* pParty = partyService.GetPlayerParty(acMessage.pPlayer);
                auto& existingOwnerComp = view.get<OwnerComponent>(*itor);
                Player* pOwningPlayer = existingOwnerComp.GetOwner();

                const bool ownerValidInRange = pOwningPlayer != nullptr &&
                                               pOwningPlayer->GetCellComponent().IsInRange(cellIdComponent, characterComponent.IsDragon());
                const bool ownerDowned = pOwningPlayer != nullptr &&
                                         m_world.GetEncounterService().GetCoordinator().IsPlayerDowned(pOwningPlayer->GetId());
                const bool challengerDowned = m_world.GetEncounterService().GetCoordinator().IsPlayerDowned(acMessage.pPlayer->GetId());
                const bool canChallengeOwnership =
                    partyService.IsPlayerLeader(acMessage.pPlayer) || ownerDowned || !ownerValidInRange;

                if (canChallengeOwnership && pParty)
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

                    // Transfer ownership if owning player is in the same party (or disconnected) and passes anti-flap/summon/downed rules.
                    // Preserve the server's authoritative battle-damaged/dead/looted state instead of
                    // overwriting it with the newly arriving peer's untouched local NPC state!
                    const bool samePartyOrOrphan =
                        pOwningPlayer == nullptr ||
                        std::find(pParty->Members.begin(), pParty->Members.end(), pOwningPlayer) != pParty->Members.end();
                    if ((claimDecision == CoopOwnership::ClaimDecision::kAccept || claimDecision == CoopOwnership::ClaimDecision::kAlreadyOwner) &&
                        samePartyOrOrphan)
                    {
                        const ActorData authoritativeActorData = BuildActorData(*itor);
                        TransferOwnership(acMessage.pPlayer, World::ToInteger(*itor), authoritativeActorData);
                        isOwner = true;
                    }
                }
            }

            AssignCharacterResponse response{};
            response.Cookie = message.Cookie;
            response.ServerId = World::ToInteger(*itor);
            response.Owner = isOwner;
            response.OwnershipEpoch = view.get<OwnerComponent>(*itor).Epoch;
            response.AllActorValues = actorValuesComponent.CurrentActorValues;
            response.CurrentInventory = inventoryComponent.Content;
            response.IsDead = characterComponent.IsDead();
            response.IsWeaponDrawn = characterComponent.IsWeaponDrawn();
            response.PlayerId = characterComponent.PlayerId;
            response.Position = movementComponent.Position;
            response.CellId = cellIdComponent.Cell;
            response.WorldSpaceId = cellIdComponent.WorldSpaceId;

            if (auto* pAnimationComponent = m_world.try_get<AnimationComponent>(*itor))
            {
                response.ActionsToReplay = pAnimationComponent->ActionsReplayCache.FormRefinedReplayChain();
            }

            acMessage.pPlayer->Send(response);

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

    if (message.WorldSpaceId || message.CellId)
    {
        if (auto* pCellIdComponent = m_world.try_get<CellIdComponent>(cEntity))
        {
            pCellIdComponent->WorldSpaceId = message.WorldSpaceId;
            pCellIdComponent->Cell = message.CellId;
            pCellIdComponent->CenterCoords = GridCellCoords::CalculateGridCellCoords(message.Position);
        }

        if (auto* pMovementComponent = m_world.try_get<MovementComponent>(cEntity))
        {
            pMovementComponent->Position = message.Position;
            pMovementComponent->Sent = true;
        }

        // Dynamic temporary spawns (0xFFxxxxxx) do not carry FormIdComponent on the server.
        if (const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(cEntity))
        {
            NotifyActorTeleport notify{};
            notify.FormId = pFormIdComponent->Id;
            notify.WorldSpaceId = message.WorldSpaceId;
            notify.CellId = message.CellId;
            notify.Position = message.Position;

            GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);
        }
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

    m_world.GetDispatcher().trigger(CharacterRemoveEvent(response.ServerId));
}

void CharacterService::OnCharacterRemoveEvent(const CharacterRemoveEvent& acEvent) const noexcept
{
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
    if (const auto* pAvComp = m_world.try_get<ActorValuesComponent>(cEntity))
    {
        const auto hpIt = pAvComp->CurrentActorValues.ActorValuesList.find(kHealthActorValueIndex);
        if (hpIt != pAvComp->CurrentActorValues.ActorValuesList.end())
        {
            hasServerHealth = true;
            serverHealth = hpIt->second;
        }
    }

    auto claimedHpIt = sanitizedData.InitialActorValues.ActorValuesList.find(kHealthActorValueIndex);
    float claimedHealth = (claimedHpIt != sanitizedData.InitialActorValues.ActorValuesList.end()) ? claimedHpIt->second : serverHealth;
    CoopOwnership::ReconcileClaimedState(
        pCharComp->IsDead(),
        serverHealth,
        hasServerHealth,
        sanitizedData.IsDead,
        claimedHealth);
    if (claimedHpIt != sanitizedData.InitialActorValues.ActorValuesList.end() || hasServerHealth)
        sanitizedData.InitialActorValues.ActorValuesList[kHealthActorValueIndex] = claimedHealth;

    TransferOwnership(acMessage.pPlayer, acMessage.Packet.ServerId, sanitizedData);
}

void CharacterService::OnCharacterSpawned(const CharacterSpawnedEvent& acEvent) const noexcept
{
    CharacterSpawnRequest message;
    Serialize(m_world, acEvent.Entity, &message);

    const auto& ownerComp = m_world.get<OwnerComponent>(acEvent.Entity);
    if (!GameServer::Get()->SendToPlayersInRange(message, acEvent.Entity, ownerComp.GetOwner()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);

    GameServer::Get()->GetWorld().GetScriptService().HandleCharacterSpawn(acEvent.Entity);
}

void CharacterService::OnReferencesMoveRequest(const PacketEvent<ClientReferencesMoveRequest>& acMessage) const noexcept
{
    OwnerView<AnimationComponent, MovementComponent, CellIdComponent> view(m_world, acMessage.GetSender());

    auto& message = acMessage.Packet;

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

        movementComponent.Tick = message.Tick;

        const auto movementCopy = movementComponent;

        auto& update = entry.second;
        auto& movement = update.UpdatedMovement;

        movementComponent.Position = movement.Position;
        movementComponent.Rotation = glm::vec3(movement.Rotation.x, 0.f, movement.Rotation.y);
        movementComponent.Variables = movement.Variables;
        movementComponent.Direction = movement.Direction;

        cellIdComponent.Cell = movement.CellId;
        cellIdComponent.WorldSpaceId = movement.WorldSpaceId;
        cellIdComponent.CenterCoords = GridCellCoords::CalculateGridCellCoords(movement.Position.x, movement.Position.y);

        for (auto& action : update.ActionEvents)
        {
            auto [canceled, reason] = GameServer::Get()->GetWorld().GetScriptService().HandleCharacterMove(entity);
            if (canceled)
                continue;

            animationComponent.CurrentAction = action;

            animationComponent.Actions.push_back(animationComponent.CurrentAction);
        }

        animationComponent.ActionsReplayCache.AppendAll(update.ActionEvents);

        movementComponent.Sent = false;
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
    auto& message = acMessage.Packet;

    NotifyMount notify;
    notify.RiderId = message.RiderId;
    notify.MountId = message.MountId;

    const entt::entity cEntity = static_cast<entt::entity>(message.MountId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
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

    // Replay cache needs to be cleared when a character respawns
    if (auto* pAnimationComponent = m_world.try_get<AnimationComponent>(*it))
        pAnimationComponent->ActionsReplayCache.Clear();

    if (ownerComponent.GetOwner() == acMessage.pPlayer)
    {
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
    else
    {
        CharacterSpawnRequest message;
        Serialize(m_world, *it, &message);

        acMessage.GetSender()->Send(message);
    }
}

void CharacterService::OnSyncExperienceRequest(const PacketEvent<SyncExperienceRequest>& acMessage) const noexcept
{
    if (!bEnableXpSync)
        return;

    NotifySyncExperience notify;
    notify.Experience = acMessage.Packet.Experience;

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

    NotifyDialogue notify{};
    notify.ServerId = message.ServerId;
    notify.SoundFilename = message.SoundFilename;

    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnSubtitleRequest(const PacketEvent<SubtitleRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    NotifySubtitle notify{};
    notify.ServerId = message.ServerId;
    notify.Text = message.Text;

    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
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

    if (pPreviousOwner && pPreviousOwner != apPlayer)
    {
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
            pPlayer->Send(message);
    }
}
