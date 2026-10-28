#include <TiltedOnlinePCH.h>

#include <Services/CharacterService.h>

#include <Actor.h>
#include <Components.h>
#include <Forms/BGSColorForm.h>
#include <Forms/BGSHeadPart.h>
#include <Forms/BGSTextureSet.h>
#include <Forms/TESNPC.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESQuest.h>
#include <Forms/TESRace.h>
#include <Forms/TESWorldSpace.h>
#include <Games/ActorExtension.h>
#include <Games/IFormFactory.h>
#include <Games/Memory.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/TES.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifySpawnData.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <PlayerCharacter.h>
#include <Services/DiscoveryService.h>
#include <Services/DownedStateService.h>
#include <Services/PartyService.h>
#include <Services/QuestService.h>
#include <Services/RfabAdapterService.h>
#include <Services/TransportService.h>
#include <Systems/CacheSystem.h>
#include <Systems/FaceGenSystem.h>
#include <Systems/InterpolationSystem.h>
#include <World.h>

#include <limits>
#include <unordered_set>

namespace
{
uint64_t ComputeLocalPlayerAppearanceFingerprint(PlayerCharacter* apPlayer) noexcept
{
    if (!apPlayer)
        return 0ULL;

    const TESNPC* pNpc = Cast<TESNPC>(apPlayer->baseForm);
    if (!pNpc)
        return 0ULL;

    CoopHairColorSync::Fnv1a64Hasher hasher{};

    uint8_t hairR = 0, hairG = 0, hairB = 0;
    const bool hasHairColor = FaceGenSystem::TryExtractActorHairColor(apPlayer, hairR, hairG, hairB);
    hasher.MixByte(hasHairColor ? 1u : 0u);
    hasher.MixU32(CoopHairColorSync::PackHairColorRgb(hairR, hairG, hairB));

    hasher.MixU32(pNpc->raceForm.race ? pNpc->raceForm.race->formID : 0u);
    hasher.MixByte(static_cast<uint8_t>(pNpc->actorData.flags & 1u));
    hasher.MixByte(pNpc->color.red);
    hasher.MixByte(pNpc->color.green);
    hasher.MixByte(pNpc->color.blue);
    hasher.MixFloat(pNpc->weight);
    hasher.MixU32((pNpc->headData && pNpc->headData->headTexture) ? pNpc->headData->headTexture->formID : 0u);

    hasher.MixByte(pNpc->headpartsCount);
    if (pNpc->headparts)
    {
        for (uint8_t i = 0; i < pNpc->headpartsCount; ++i)
            hasher.MixU32(pNpc->headparts[i] ? pNpc->headparts[i]->formID : 0u);
    }

    if (pNpc->faceMorphs)
    {
        for (const float opt : pNpc->faceMorphs->option)
            hasher.MixFloat(opt);
        for (const uint32_t preset : pNpc->faceMorphs->presets)
            hasher.MixU32(preset);
    }

    const auto& tints = apPlayer->GetTints();
    hasher.MixU32(tints.length);
    for (uint32_t i = 0; i < tints.length; ++i)
    {
        if (const TintMask* pTint = tints[i])
        {
            hasher.MixU32(pTint->type);
            hasher.MixU32(pTint->color);
            hasher.MixFloat(pTint->alpha);
        }
    }

    return hasher.Finish();
}
} // namespace

void CharacterService::OnRemoteSpawnDataReceived(const NotifySpawnData& acMessage) noexcept
{
    if (auto modelIt = m_remoteModels.find(acMessage.Id); modelIt != m_remoteModels.end())
    {
        modelIt.value().SpawnRequest.InitialActorValues = acMessage.NewActorData.InitialActorValues;
        modelIt.value().SpawnRequest.InventoryContent = acMessage.NewActorData.InitialInventory;
        modelIt.value().SpawnRequest.IsDead = acMessage.NewActorData.IsDead;
        modelIt.value().SpawnRequest.IsWeaponDrawn = acMessage.NewActorData.IsWeaponDrawn;
    }

    const entt::entity entity = FindEntityByServerId(acMessage.Id);
    if (entity == entt::null || !m_world.all_of<FormIdComponent>(entity) || m_world.all_of<ObjectComponent>(entity))
        return;

    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(entity))
    {
        pWaitingFor3D->SpawnRequest.InitialActorValues = acMessage.NewActorData.InitialActorValues;
        pWaitingFor3D->SpawnRequest.InventoryContent = acMessage.NewActorData.InitialInventory;
        pWaitingFor3D->SpawnRequest.IsDead = acMessage.NewActorData.IsDead;
        pWaitingFor3D->SpawnRequest.IsWeaponDrawn = acMessage.NewActorData.IsWeaponDrawn;
    }

    auto& formIdComponent = m_world.get<FormIdComponent>(entity);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
        return;

    // Never run SetActorInventory while 3D is still streaming (GetNiNode() == nullptr): stage into WaitingFor3D instead.
    if (pActor->GetNiNode() && !pActor->IsStillLoading())
    {
        pActor->SetActorInventory(acMessage.NewActorData.InitialInventory);
    }
    else if (!m_world.all_of<WaitingFor3D>(entity))
    {
        CharacterSpawnRequest pending3D{};
        pending3D.ServerId = acMessage.Id;
        pending3D.InitialActorValues = acMessage.NewActorData.InitialActorValues;
        pending3D.InventoryContent = acMessage.NewActorData.InitialInventory;
        pending3D.IsDead = acMessage.NewActorData.IsDead;
        pending3D.IsWeaponDrawn = acMessage.NewActorData.IsWeaponDrawn;
        m_world.emplace_or_replace<WaitingFor3D>(entity, pending3D);
    }

    pActor->SetActorValues(acMessage.NewActorData.InitialActorValues);
    m_weaponDrawUpdates[pActor->formID] = {acMessage.NewActorData.IsWeaponDrawn};

    const bool isLocalOwner = m_world.all_of<LocalComponent>(entity);
    if (!isLocalOwner)
    {
        if (auto* pInterp = m_world.try_get<InterpolationComponent>(entity))
        {
            pInterp->TimePoints.clear();
            pInterp->Position = pActor->position;
            pInterp->Clock.Reset();
        }
    }

    if (pActor->IsDead() != acMessage.NewActorData.IsDead)
    {
        if (acMessage.NewActorData.IsDead)
            KillActorSuppressingDuplicateXp(pActor);
        else if (isLocalOwner)
            spdlog::warn("Keeping local actor {:X} dead although server spawn data has it alive (#906)", pActor->formID);
        else
            pActor->ResurrectInPlace();
    }

    spdlog::info("Applied remote spawn data, actor form id: {:X}", pActor->formID);
}

void CharacterService::OnReferencesMoveRequest(const ServerReferencesMoveRequest& acMessage) const noexcept
{
    const double cArrivalTime = m_transport.GetMessageReceiveTime();
    const CoopWorldView::ObserverState obs = BuildLocalObserverState();

    for (const auto& [serverId, update] : acMessage.Updates)
    {
        const entt::entity entity = FindEntityByServerId(serverId);
        if (entity == entt::null || !m_world.all_of<RemoteComponent, InterpolationComponent, RemoteAnimationComponent>(entity))
            continue;

        auto& interpolationComponent = m_world.get<InterpolationComponent>(entity);
        auto& animationComponent = m_world.get<RemoteAnimationComponent>(entity);
        const auto& movement = update.UpdatedMovement;

        const CoopWorldView::WorldOrCell incomingLoc = CoopWorldView::WorldOrCell::FromPacket(
            movement.WorldSpaceId, movement.CellId, movement.Position.x, movement.Position.y);

        if (auto modelIt = m_remoteModels.find(serverId); modelIt != m_remoteModels.end())
        {
            auto& model = modelIt.value();
            const bool spaceWasDifferent =
                model.Location.IsValid() && incomingLoc.IsValid() && !model.Location.IsSameSpace(incomingLoc);
            const auto transition = CoopWorldView::EvaluateMovementTransition(
                obs,
                model.Location,
                model.LatestPosition.x,
                model.LatestPosition.y,
                model.LatestPosition.z,
                incomingLoc,
                movement.Position.x,
                movement.Position.y,
                movement.Position.z);

            if (transition.HasAuthoritativeLocation)
            {
                if (spaceWasDifferent && model.SpawnRequest.IsPlayer && m_world.all_of<FormIdComponent>(entity))
                    model.DematerializeGraceUntilMs = NowSteadyWorldViewMs() + CoopWorldView::kRemotePlayerDoorTransitionGraceMs;

                model.Location = incomingLoc;
                model.SpawnRequest.WorldSpaceId = movement.WorldSpaceId;
                model.SpawnRequest.CellId = movement.CellId;
            }
            else if (model.Location.IsExterior())
            {
                model.Location.GridCoords =
                    GridCellCoords::CalculateGridCellCoords(movement.Position.x, movement.Position.y);
            }

            model.LatestPosition = movement.Position;
            model.LatestRotation = movement.Rotation;
            model.SpawnRequest.Position = movement.Position;
            model.SpawnRequest.Rotation = movement.Rotation;

            if (transition.FlushInterpolationHistory)
            {
                interpolationComponent.TimePoints.clear();
                interpolationComponent.Position = movement.Position;
                interpolationComponent.Clock.Reset();
            }

            const uint64_t nowMs = NowSteadyWorldViewMs();
            const bool withinDoorGrace = model.SpawnRequest.IsPlayer &&
                                         model.DematerializeGraceUntilMs > 0 &&
                                         nowMs < model.DematerializeGraceUntilMs;
            if (!transition.CompatibleWithObserver && !withinDoorGrace && m_world.all_of<FormIdComponent>(entity))
            {
                DematerializeRemoteView(entity, &model);
            }
        }

        if (cArrivalTime > 0.0)
            interpolationComponent.Clock.AddSample(cArrivalTime, acMessage.Tick);

        InterpolationComponent::TimePoint point;
        point.Tick = acMessage.Tick;
        point.Position = movement.Position;
        point.Rotation = {movement.Rotation.x, 0.f, movement.Rotation.y};
        point.Variables = movement.Variables;
        point.Direction = movement.Direction;

        InterpolationSystem::AddPoint(interpolationComponent, point);

        if (m_world.all_of<FormIdComponent>(entity))
        {
            for (const auto& action : update.ActionEvents)
                animationComponent.TimePoints.push_back(action);
        }
    }
}

void CharacterService::OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) noexcept
{
    const auto playerIt = m_playerServerIds.find(acMessage.PlayerId);
    if (playerIt == m_playerServerIds.end())
        return;

    const uint32_t serverId = playerIt->second;
    auto modelIt = m_remoteModels.find(serverId);
    if (modelIt == m_remoteModels.end())
        return;

    auto& model = modelIt.value();
    const CoopWorldView::WorldOrCell newLoc = CoopWorldView::WorldOrCell::FromPacket(
        acMessage.WorldSpaceId,
        acMessage.CellId,
        model.LatestPosition.x,
        model.LatestPosition.y);

    if (!newLoc.IsValid())
        return;

    const bool spaceChanged = model.Location.IsValid() && !model.Location.IsSameSpace(newLoc);
    model.Location = newLoc;
    model.SpawnRequest.WorldSpaceId = acMessage.WorldSpaceId;
    model.SpawnRequest.CellId = acMessage.CellId;

    const entt::entity remoteEntity = FindEntityByServerId(serverId);
    if (remoteEntity != entt::null && m_world.all_of<RemoteComponent>(remoteEntity) && spaceChanged)
    {
        if (m_world.all_of<FormIdComponent>(remoteEntity))
            model.DematerializeGraceUntilMs = NowSteadyWorldViewMs() + CoopWorldView::kRemotePlayerDoorTransitionGraceMs;
        if (auto* pInterp = m_world.try_get<InterpolationComponent>(remoteEntity))
            pInterp->TimePoints.clear();
    }

    ReconcileRemoteWorldViews();
}

void CharacterService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    if (const auto playerIt = m_playerServerIds.find(acMessage.PlayerId); playerIt != m_playerServerIds.end())
    {
        const uint32_t serverId = playerIt->second;
        m_playerServerIds.erase(playerIt);
        NotifyRemoveCharacter removeMsg{};
        removeMsg.ServerId = serverId;
        OnRemoveCharacter(removeMsg);
    }
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
        CleanupUnassignedActorEntity(aEntity);
        return;
    }

    if (auto* pRemoteComponent = m_world.try_get<RemoteComponent>(aEntity); pRemoteComponent)
    {
        // Never steal actor ownership onto a downed party leader or from a remote player summon!
        if (m_world.GetPartyService().IsLeader() &&
            !m_world.GetDownedStateService().IsLocalPlayerDowned() &&
            !pActor->IsInCombat() &&
            !pActor->IsTemporary() &&
            !pActor->IsMount() &&
            !pActor->IsPlayerSummon())
        {
            spdlog::info("Sending ownership claim for actor {:X} with server id {:X}", pActor->formID, pRemoteComponent->Id);
            TakeOwnership(pActor->formID, pRemoteComponent->Id, aEntity);
        }
        else
        {
            spdlog::info("New entity remotely managed, form id: {:X}, server id: {:X}", pActor->formID, pRemoteComponent->Id);
        }

        return;
    }

    if (m_world.any_of<RemoteComponent, LocalComponent, WaitingForAssignmentComponent>(aEntity))
        return;

    CacheSystem::Setup(World::Get(), aEntity, pActor);

    if (!RequestServerAssignment(aEntity))
        CleanupUnassignedActorEntity(aEntity);
}

bool CharacterService::RequestServerAssignment(const entt::entity aEntity) const noexcept
{
    if (!m_transport.IsOnline())
        return false;

    static uint32_t sCookieSeed = 0;

    const auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
        return false;

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
        return false;

    AssignCharacterRequest message{};
    message.Cookie = sCookieSeed;

    if (!m_world.GetModSystem().GetServerModId(formIdComponent.Id, message.ReferenceId))
    {
        spdlog::debug("Server reference id not found for non-synced form id {:X}", formIdComponent.Id);
        return false;
    }

    if (!pActor->parentCell || !m_world.GetModSystem().GetServerModId(pActor->parentCell->formID, message.CellId))
    {
        spdlog::debug("Server cell id not found for cell id {:X}", pActor->parentCell ? pActor->parentCell->formID : 0u);
        return false;
    }

    if (const auto pWorldSpace = pActor->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, message.WorldSpaceId))
            return false;
    }

    message.Position = pActor->position;
    message.Rotation.x = pActor->rotation.x;
    message.Rotation.y = pActor->rotation.z;

    const auto isPlayer = (formIdComponent.Id == 0x14);
    const auto isTemporary = pActor->formID >= 0xFF000000;
    TESNPC* pResolvedTemplateNpc = pActor->GetLeveledTemplateBase();

    if (isPlayer)
    {
        // Include kBaseData (0x2 = female/male bit), kRace (0x1000000), and kFaceGen (0x2000800)
        // so cross-gender & cross-race BGSHeadPart / morph sync never leaves remote clones headless!
        pNpc->MarkChanged(0x3000802u);
    }

    const auto changeFlags = pNpc->GetChangeFlags();
    const bool hasStaticNpcTemplate = (pResolvedTemplateNpc && !pResolvedTemplateNpc->IsTemporary());

    if (isPlayer || (!hasStaticNpcTemplate && changeFlags != 0))
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

        // Capture exact runtime RGB hair color (both vanilla BGSColorForm and RaceMenu custom HairColorCustom)
        // because TESNPC::Save only serializes the shared BGSColorForm FormID, never its in-memory RGB bytes!
        uint8_t hairR = 0, hairG = 0, hairB = 0;
        if (FaceGenSystem::TryExtractActorHairColor(pActor, hairR, hairG, hairB))
        {
            message.FaceTints.SetHairColor(hairR, hairG, hairB);
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

    if (pResolvedTemplateNpc)
        pNpc = pResolvedTemplateNpc;
    else if (pNpc->IsTemporary())
        pNpc = pNpc->GetTemplateBase();

    if (!isPlayer)
    {
        if (pNpc && !pNpc->IsTemporary() &&
            m_world.GetModSystem().GetServerModId(pNpc->formID, message.FormId) &&
            message.FormId.ModId != std::numeric_limits<uint32_t>::max())
        {
            // Resolved static plugin TESNPC FormId
        }
        else
        {
            message.FormId = GameId{};
            if (isTemporary)
            {
                spdlog::debug(
                    "Server NPC form id not found for non-synced temporary actor {:X} (base {:X})",
                    pActor->formID,
                    pNpc ? pNpc->formID : 0u);
                return false;
            }
        }
    }

    (void)ActorExtension::TryGetLatestAnimation(pActor->formID, message.LatestAction);
    pActor->SaveAnimationVariables(message.LatestAction.Variables);

    const bool isLeader = m_world.Get().GetPartyService().IsLeader();
    spdlog::info(
        __FUNCTION__ ": request id: {:X}, cookie: {:X}, entity: {:X}, isLeader: {}",
        formIdComponent.Id,
        sCookieSeed,
        to_integral(aEntity),
        isLeader);

    if (m_transport.Send(message))
    {
        if (isPlayer)
        {
            m_localAppearanceResync.RecordSyncedFingerprint(
                ComputeLocalPlayerAppearanceFingerprint(PlayerCharacter::Get()),
                NowSteadyWorldViewMs());
        }
        m_world.emplace_or_replace<WaitingForAssignmentComponent>(aEntity, sCookieSeed);
        sCookieSeed++;
        return true;
    }

    return false;
}

void CharacterService::RunSpawnUpdates() const noexcept
{
    ReconcileRemoteWorldViews();
}

uint32_t CharacterService::LookupAuthoritativePlacedBaseFormId(const uint32_t aRefFormId) const noexcept
{
    return m_authoritativePlacedBases.LookupBaseFormId(aRefFormId);
}

void CharacterService::RecordAuthoritativePlacedBase(const GameId& acRefId, const GameId& acBaseId) const noexcept
{
    if (acRefId == GameId{} || acBaseId == GameId{} ||
        acRefId.ModId == std::numeric_limits<uint32_t>::max() ||
        acBaseId.ModId == std::numeric_limits<uint32_t>::max())
    {
        return;
    }

    const uint32_t refFormId = m_world.GetModSystem().GetGameId(acRefId);
    const uint32_t baseFormId = m_world.GetModSystem().GetGameId(acBaseId);
    m_authoritativePlacedBases.Record(refFormId, baseFormId);
}

bool CharacterService::IsRemotePlayerOrSummonServerId(const uint32_t aServerId) const noexcept
{
    if (aServerId == 0)
        return false;

    if (const auto modelIt = m_remoteModels.find(aServerId); modelIt != m_remoteModels.end())
    {
        if (modelIt->second.SpawnRequest.IsPlayer || modelIt->second.SpawnRequest.IsPlayerSummon)
            return true;
    }

    if (const auto spawnIt = m_cachedRemoteSpawns.find(aServerId); spawnIt != m_cachedRemoteSpawns.end())
    {
        if (spawnIt->second.IsPlayer || spawnIt->second.IsPlayerSummon)
            return true;
    }

    return false;
}

bool CharacterService::IsCachedServerActorDead(const uint32_t aServerId) const noexcept
{
    if (const auto modelIt = m_remoteModels.find(aServerId); modelIt != m_remoteModels.end())
        return modelIt->second.SpawnRequest.IsDead;

    if (const auto spawnIt = m_cachedRemoteSpawns.find(aServerId); spawnIt != m_cachedRemoteSpawns.end())
        return spawnIt->second.IsDead;

    return false;
}

bool CharacterService::ReconcileLoadedRemoteSpeciesMismatch(
    Actor* apActor,
    const GameId& acExpectedBaseId) const noexcept
{
    if (!World::IsMainThread() ||
        !apActor ||
        !apActor->GetNiNode() ||
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

    TESNPC* pCurrentNpc = apActor->GetLeveledTemplateBase();
    if (!pCurrentNpc)
        pCurrentNpc = Cast<TESNPC>(apActor->baseForm);

    if (apActor->baseForm->formID == expectedFormId ||
        (pCurrentNpc && pCurrentNpc->formID == expectedFormId))
    {
        return false;
    }

    TESNPC* pExpectedNpc = Cast<TESNPC>(TESForm::GetById(expectedFormId));
    if (!pExpectedNpc || pExpectedNpc->IsStillLoading() || pExpectedNpc->IsDeleted())
        return false;

    const entt::entity ownerEntity = FindActorEntityByFormId(apActor->formID);
    const bool hasLocalComponent = (ownerEntity != entt::null && m_world.all_of<LocalComponent>(ownerEntity));

    const uint32_t currentRaceFormId =
        (pCurrentNpc && pCurrentNpc->raceForm.race) ? pCurrentNpc->raceForm.race->formID : 0u;
    const uint32_t expectedRaceFormId =
        pExpectedNpc->raceForm.race ? pExpectedNpc->raceForm.race->formID : 0u;

    if (!CoopWorldStabilizer::ShouldForcePost3DSpeciesSwap(
            false,
            false,
            hasLocalComponent,
            apActor->IsDead(),
            apActor->IsInCombat(),
            currentRaceFormId,
            expectedRaceFormId))
    {
        return false;
    }

    spdlog::info(
        "[RFAB SpeciesSync] Reconciling loaded placed actor {:X} species mismatch (base {:X} -> {:X}, race {:X} -> {:X})",
        apActor->formID,
        apActor->baseForm->formID,
        expectedFormId,
        currentRaceFormId,
        expectedRaceFormId);

    // Arm a 3.5s DiscoveryService demotion hold BEFORE QueueUpdate() so the brief NiNode rebuild
    // never dispatches ActorRemovedEvent or drops the entity's RemoteComponent!
    m_world.GetDiscoveryService().HoldActorRemoval(
        apActor->formID,
        CoopWorldStabilizer::ActorDemotionHoldTracker::kDefaultDemotionHoldMs);

    apActor->SetBaseForm(pExpectedNpc);
    if (pExpectedNpc->raceForm.race)
        apActor->race = pExpectedNpc->raceForm.race;
    apActor->QueueUpdate();
    return true;
}

bool CharacterService::DespawnSuppressedNonLeaderWildTempSpawn(Actor* apActor, const entt::entity aEntity) noexcept
{
    if (!apActor || !m_transport.IsOnline())
        return false;

    const bool isTemporaryFormId = ((apActor->formID & 0xFF000000u) == 0xFF000000u && apActor->formID != 0x14);
    if (!isTemporaryFormId)
        return false;

    const auto& partyService = m_world.GetPartyService();
    if (!partyService.IsInParty() || partyService.IsLeader())
        return false;

    const Actor* pCommandingActor = apActor->GetCommandingActor();
    const bool isPlayerSummonOrTeammate =
        (pCommandingActor && pCommandingActor->formID == 0x14) ||
        apActor->IsPlayerSummon() ||
        apActor->IsCommandedActor() ||
        apActor->IsPlayerTeammate();

    TiltedPhoques::Vector<uint32_t> peerFormIds;
    m_world.GetRfabAdapterService().CollectPartyPeerActorFormIds(peerFormIds);

    bool hasNearbyPartyPeer = false;
    for (const uint32_t peerFormId : peerFormIds)
    {
        Actor* pPeer = Cast<Actor>(TESForm::GetById(peerFormId));
        if (!pPeer || pPeer->IsDeleted())
            continue;
        if (pPeer->GetWorldSpace() != apActor->GetWorldSpace())
            continue;
        if (!apActor->GetWorldSpace() && pPeer->parentCell != apActor->parentCell)
            continue;

        const float dx = pPeer->position.x - apActor->position.x;
        const float dy = pPeer->position.y - apActor->position.y;
        if ((dx * dx + dy * dy) <= (7000.0f * 7000.0f))
        {
            hasNearbyPartyPeer = true;
            break;
        }
    }

    const bool isRemoteNetworkCreated =
        (apActor->GetExtension() && apActor->GetExtension()->IsRemote()) ||
        (m_createdNetworkFormIds.find(apActor->formID) != m_createdNetworkFormIds.end());

    if (!CoopWorldStabilizer::ShouldSuppressNonLeaderTempWildSpawn(
            isTemporaryFormId,
            false,
            partyService.IsInParty(),
            partyService.IsLeader(),
            isRemoteNetworkCreated,
            isPlayerSummonOrTeammate,
            apActor->IsMount(),
            hasNearbyPartyPeer))
    {
        return false;
    }

    spdlog::info(
        "[RFAB WildSync] Suppressing redundant non-leader local 0xFF encounter {:X} ({}) near party leader/peer",
        apActor->formID,
        apActor->baseForm ? apActor->baseForm->GetName() : "<null>");

    m_world.remove<EarlyAnimationBufferComponent, FormIdComponent>(aEntity);
    if (m_world.orphan(aEntity))
        m_world.destroy(aEntity);

    apActor->UnsetChanged(0xFFFFFFFF);
    apActor->SetSkipSaveFlag(true);
    apActor->Delete();
    return true;
}

void CharacterService::ApplyRemotePlayerHairColorForm(
    const uint32_t aPlayerId,
    TESNPC* apNpc,
    const Tints& acFaceTints) const noexcept
{
    if (!apNpc || ((apNpc->formID & 0xFF000000u) != 0xFF000000u))
        return;

    uint8_t hairR = 0, hairG = 0, hairB = 0;
    if (!acFaceTints.TryGetHairColor(hairR, hairG, hairB))
        return;

    // Give each remote player's 0xFF00xxxx TESNPC its own isolated 0xFF00xxxx BGSColorForm!
    // RaceMenu (skee64.dll) mutates a SINGLE shared BGSColorForm record in RaceMenuPlugin.esp in-place,
    // so leaving apNpc->headData->hairColor pointing at the deserialized FormID caused every remote player
    // to inherit the local client's custom hair color (and vice versa).
    BGSColorForm* pDedicatedColor = nullptr;
    if (aPlayerId != 0)
    {
        if (const auto poolIt = m_playerHairColorPool.find(aPlayerId); poolIt != m_playerHairColorPool.end())
        {
            pDedicatedColor = Cast<BGSColorForm>(TESForm::GetById(poolIt->second));
        }
    }

    if (!pDedicatedColor)
    {
        if (IFormFactory* pFactory = IFormFactory::GetForType(FormType::ColorForm))
        {
            if (TESForm* pCreated = pFactory->Create())
            {
                pDedicatedColor = Cast<BGSColorForm>(pCreated);
                if (pDedicatedColor)
                {
                    pDedicatedColor->SetSkipSaveFlag(true);
                    pDedicatedColor->UnsetChanged(0xFFFFFFFF);
                    m_createdNetworkFormIds.insert(pDedicatedColor->formID);
                    if (aPlayerId != 0)
                        m_playerHairColorPool[aPlayerId] = pDedicatedColor->formID;
                }
            }
        }
    }

    if (!pDedicatedColor)
        return;

    pDedicatedColor->color.red = hairR;
    pDedicatedColor->color.green = hairG;
    pDedicatedColor->color.blue = hairB;
    pDedicatedColor->color.alpha = 0;
    pDedicatedColor->unk20 |= 1u; // BGSColorForm::Flag::kPlayable
    pDedicatedColor->SetSkipSaveFlag(true);
    pDedicatedColor->UnsetChanged(0xFFFFFFFF);

    if (!apNpc->headData)
    {
        apNpc->headData = Memory::New<TESNPC::HeadData>();
        if (apNpc->headData)
        {
            apNpc->headData->hairColor = nullptr;
            apNpc->headData->headTexture = nullptr;
        }
    }

    if (apNpc->headData)
        apNpc->headData->hairColor = pDedicatedColor;
}

void CharacterService::PollLocalPlayerAppearanceSync() noexcept
{
    if (!m_transport.IsOnline() || m_savePurgeActive || m_engineWorldLoadInProgress)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || pPlayer->IsStillLoading() || pPlayer->IsDeleted())
        return;

    bool inRaceSexMenu = false;
    bool inLoadingMenu = false;
    if (UI* pUi = UI::Get())
    {
        static const BSFixedString* s_pRaceSexMenu = new BSFixedString("RaceSex Menu");
        static const BSFixedString* s_pLoadingMenu = new BSFixedString("Loading Menu");
        inRaceSexMenu = pUi->GetMenuOpen(*s_pRaceSexMenu);
        inLoadingMenu = pUi->GetMenuOpen(*s_pLoadingMenu);
    }

    const uint64_t nowMs = NowSteadyWorldViewMs();
    if (!m_localAppearanceResync.ShouldPollNow(nowMs, inRaceSexMenu, inLoadingMenu))
        return;

    const entt::entity playerEntity = FindActorEntityByFormId(0x14u);
    if (playerEntity == entt::null ||
        !m_world.all_of<LocalComponent>(playerEntity) ||
        m_world.any_of<WaitingForAssignmentComponent>(playerEntity))
    {
        return;
    }

    const uint64_t currentFp = ComputeLocalPlayerAppearanceFingerprint(pPlayer);
    if (!m_localAppearanceResync.EvaluateChangedFingerprint(currentFp, nowMs))
        return;

    FaceGenSystem::InvalidateLocalPlayerHairTintIsolation();
    m_nextFaceGenMainThreadScheduleMs = 0;
    (void)RequestServerAssignment(playerEntity);
}

void CharacterService::ScheduleMainThreadFaceGenPass() noexcept
{
    if (!m_transport.IsOnline() || m_savePurgeActive || m_engineWorldLoadInProgress)
        return;

    if (!m_spFaceGenMainThreadPending || m_spFaceGenMainThreadPending->load(std::memory_order_acquire))
        return;

    const uint64_t nowMs = NowSteadyWorldViewMs();
    if (nowMs < m_nextFaceGenMainThreadScheduleMs)
        return;

    std::vector<FaceGenMainThreadEntry> remoteActors;
    auto facegenView = m_world.view<FormIdComponent, FaceGenComponent>();
    for (auto entity : facegenView)
    {
        const uint32_t formId = facegenView.get<FormIdComponent>(entity).Id;
        if (formId == 0 || formId == 0x14)
            continue;

        FaceGenMainThreadEntry item{};
        item.FormId = formId;
        item.FaceTints = facegenView.get<FaceGenComponent>(entity).FaceTints;
        remoteActors.push_back(std::move(item));
    }

    const bool hasRemoteHairPeers = !m_playerHairColorPool.empty() || !remoteActors.empty();
    if (remoteActors.empty() && !hasRemoteHairPeers)
        return;

    m_nextFaceGenMainThreadScheduleMs = nowMs + 80ULL;
    if (m_spFaceGenMainThreadPending->exchange(true, std::memory_order_acq_rel))
        return;

    auto spPending = m_spFaceGenMainThreadPending;
    m_world.QueueMainThreadTask(
        [spPending, remoteActors = std::move(remoteActors), hasRemoteHairPeers]()
        {
            FaceGenSystem::ExecuteMainThreadPass(remoteActors, hasRemoteHairPeers);
            spPending->store(false, std::memory_order_release);
        });
}
