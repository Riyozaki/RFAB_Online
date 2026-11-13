#include <TiltedOnlinePCH.h>

#include <Services/WorldItemService.h>

#include <Events/InventoryChangeEvent.h>
#include <Events/UpdateEvent.h>
#include <ExtraData/ExtraDataList.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Messages/NotifyWorldItemDespawn.h>
#include <Messages/NotifyWorldItemSnapshot.h>
#include <Messages/NotifyWorldItemSpawn.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/RequestWorldItemDrop.h>
#include <Messages/RequestWorldItemPickup.h>
#include <PlayerCharacter.h>
#include <Services/CharacterService.h>
#include <Services/SessionResyncService.h>
#include <Services/TransportService.h>
#include <Structs/GridCellCoords.h>
#include <World.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
constexpr double kTickIntervalSec = 0.25;
constexpr uint64_t kValidationIntervalMs = 1000;
// References created per tick: spawning is cheap but loads 3D, keep a big snapshot from hitching.
constexpr size_t kMaterializeBudgetPerTick = 4;
// ExtraCount is an int16: a larger server stack is shown as this many; a pickup takes what was shown
// and the server's remainder upsert shows the rest.
constexpr int32_t kMaxShownStack = 32767;
} // namespace

WorldItemService::WorldItemService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_transport(aTransport)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&WorldItemService::OnUpdate>(this))
    , m_spawnConnection(aDispatcher.sink<NotifyWorldItemSpawn>().connect<&WorldItemService::OnSpawn>(this))
    , m_despawnConnection(aDispatcher.sink<NotifyWorldItemDespawn>().connect<&WorldItemService::OnDespawn>(this))
    , m_snapshotConnection(aDispatcher.sink<NotifyWorldItemSnapshot>().connect<&WorldItemService::OnSnapshot>(this))
{
}

uint64_t WorldItemService::NowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint32_t WorldItemService::NextCookie() noexcept
{
    // Unique among this client's in-flight drops; 0 means "no cookie" on the wire.
    if (m_nextCookie == 0)
        m_nextCookie = 1;
    return m_nextCookie++;
}

// --- placement ---------------------------------------------------------------------------------

WorldItems::Placement WorldItemService::PlacementOf(const WorldItemState& acState) noexcept
{
    WorldItems::Placement where{};
    where.CellKey = WorldItems::PackKey(acState.CellId.ModId, acState.CellId.BaseId);
    where.WorldSpaceKey = acState.WorldSpaceId ? WorldItems::PackKey(acState.WorldSpaceId.ModId, acState.WorldSpaceId.BaseId) : 0;
    where.X = acState.Position.x;
    where.Y = acState.Position.y;
    where.Z = acState.Position.z;
    return where;
}

bool WorldItemService::LocalPlacement(WorldItems::Placement& aOut) const noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || pPlayer->IsStillLoading() || !pPlayer->parentCell)
        return false;

    auto& modSystem = m_world.GetModSystem();
    GameId cellId{};
    if (!modSystem.GetServerModId(pPlayer->parentCell->formID, cellId))
        return false;

    aOut = WorldItems::Placement{};
    aOut.CellKey = WorldItems::PackKey(cellId.ModId, cellId.BaseId);
    if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace())
    {
        GameId worldSpaceId{};
        if (!modSystem.GetServerModId(pWorldSpace->formID, worldSpaceId))
            return false;
        aOut.WorldSpaceKey = WorldItems::PackKey(worldSpaceId.ModId, worldSpaceId.BaseId);
    }
    aOut.X = pPlayer->position.x;
    aOut.Y = pPlayer->position.y;
    aOut.Z = pPlayer->position.z;
    return true;
}

bool WorldItemService::CanTouchEngine() const noexcept
{
    return World::IsMainThread() && !m_world.GetLifecycle().IsWorldLoadingAnyThread() &&
           !m_world.GetCharacterService().IsEngineWorldLoading();
}

// --- engine references -------------------------------------------------------------------------

TESObjectREFR* WorldItemService::ResolveBound(uint32_t aFormId, const WorldItemState& acState) const noexcept
{
    if (aFormId == 0)
        return nullptr;
    TESObjectREFR* pReference = Cast<TESObjectREFR>(TESForm::GetById(aFormId));
    if (!pReference || !pReference->IsTemporary() || pReference->IsDeleted() || pReference->IsDisabled() || !pReference->baseForm)
        return nullptr;
    // Temporary form ids are reused: a reference showing another base form is not ours any more.
    const uint32_t baseId = m_world.GetModSystem().GetGameId(acState.Item.BaseId);
    if (baseId == 0 || pReference->baseForm->formID != baseId)
        return nullptr;
    return pReference;
}

int32_t WorldItemService::ShownCount(WorldItems::ItemId aId) const noexcept
{
    const Mirror::Record* pRecord = m_mirror.Find(aId);
    if (!pRecord || pRecord->BoundFormId == 0)
        return 0;
    TESObjectREFR* pReference = ResolveBound(pRecord->BoundFormId, pRecord->Payload);
    if (!pReference || !pReference->GetExtraDataList())
        return 0;
    return pReference->GetExtraDataList()->GetStackCount();
}

void WorldItemService::MarkServerOwned(TESObjectREFR* apReference) noexcept
{
    if (!apReference)
        return;
    apReference->SetSkipSaveFlag(true);
    apReference->UnsetChanged(0xFFFFFFFF);
}

void WorldItemService::DeleteBoundReference(uint32_t aFormId) noexcept
{
    // While the engine world is torn down the reference goes with it (it is SkipSave, no save brings
    // it back); touching it now could race the unload.
    if (aFormId == 0 || !CanTouchEngine())
        return;
    TESObjectREFR* pReference = Cast<TESObjectREFR>(TESForm::GetById(aFormId));
    // Never a persistent form: this service only ever binds temporary references.
    if (!pReference || !pReference->IsTemporary() || pReference->IsDeleted())
        return;
    MarkServerOwned(pReference);
    pReference->Disable();
    pReference->Delete();
}

// --- local drops and pickups ---------------------------------------------------------------------

bool WorldItemService::HandleLocalDrop(const InventoryChangeEvent& acEvent, uint32_t aActorServerId) noexcept
{
    if (!World::IsFeatureEnabled(Session::Feature::kServerOwnedWorldItems) || !m_transport.IsOnline())
        return false;
    if (acEvent.Item.Count >= 0 || !acEvent.Item.BaseId)
        return false;

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.FormId));
    if (!pActor)
        return false;

    TESObjectREFR* pReference = acEvent.WorldReferenceFormId ? Cast<TESObjectREFR>(TESForm::GetById(acEvent.WorldReferenceFormId)) : nullptr;
    if (pReference && (pReference->IsDeleted() || !pReference->IsTemporary()))
        pReference = nullptr;

    // Where the item lies: the reference the engine created; the actor if there is none (the item is
    // then registered at the actor's feet and shown to the dropper like to everybody else).
    const TESObjectREFR* pWhere = pReference ? pReference : static_cast<TESObjectREFR*>(pActor);
    TESObjectCELL* pCell = pWhere->parentCell ? pWhere->parentCell : pActor->parentCell;
    if (!pCell)
        return false;

    auto& modSystem = m_world.GetModSystem();
    RequestWorldItemDrop request{};
    if (!modSystem.GetServerModId(pCell->formID, request.CellId))
        return false;
    if (TESWorldSpace* pWorldSpace = pWhere->GetWorldSpace())
    {
        if (!modSystem.GetServerModId(pWorldSpace->formID, request.WorldSpaceId))
            return false;
    }

    request.Cookie = NextCookie();
    request.ActorServerId = aActorServerId;
    request.Item = acEvent.Item;
    request.Item.Count = -acEvent.Item.Count;
    request.Position = glm::vec3(pWhere->position.x, pWhere->position.y, pWhere->position.z);
    if (pReference)
        request.Rotation = glm::vec3(pReference->rotation.x, pReference->rotation.y, pReference->rotation.z);
    else
        request.Rotation = glm::vec3(0.0f, 0.0f, pActor->rotation.z);

    if (!std::isfinite(request.Position.x) || !std::isfinite(request.Position.y) || !std::isfinite(request.Position.z))
        return false;

    if (!m_transport.Send(request))
        return false;

    if (pReference)
        m_mirror.NotePendingDrop(request.Cookie, pReference->formID, NowMs());

    spdlog::debug("[RFAB WorldItems] drop request cookie {} : {}x {:X} by actor {:X} (reference {:X})", request.Cookie,
                  request.Item.Count, request.Item.BaseId.LogFormat(), aActorServerId, pReference ? pReference->formID : 0u);
    return true;
}

bool WorldItemService::SendClaim(WorldItems::ItemId aId, uint32_t aPickerServerId, const Inventory::Entry& acItem, bool aWholeStack) noexcept
{
    RequestWorldItemPickup request{};
    request.ItemId = aId;
    request.PickerServerId = aPickerServerId;
    request.Item = acItem;
    if (!m_transport.Send(request))
        return false;

    ++m_claimsSent;
    m_suspectedLost.erase(aId);
    // The engine consumed the reference: forget it now, so neither the despawn nor a reused form id
    // makes this client delete another reference. A partial pickup leaves the reference showing the
    // remainder, which the server's upsert confirms.
    if (aWholeStack)
        m_mirror.ReleaseConsumedReference(aId, NowMs());
    return true;
}

bool WorldItemService::HandleLocalPickup(const InventoryChangeEvent& acEvent, uint32_t aPickerServerId) noexcept
{
    // Not gated by the feature switch: an item the server owns is always arbitrated.
    if (acEvent.Item.Count <= 0 || acEvent.WorldReferenceFormId == 0 || !m_transport.IsOnline())
        return false;

    const WorldItems::ItemId id = m_mirror.FindByFormId(acEvent.WorldReferenceFormId);
    if (id != WorldItems::kNoItem)
        return SendClaim(id, aPickerServerId, acEvent.Item, acEvent.WholeStack);

    // An own drop picked back up before its echo: the item has no id yet. Hold the pickup until the
    // echo binds the reference (claim) or the drop times out unregistered (legacy pickup).
    if (const uint32_t cookie = m_mirror.FindPendingDropCookie(acEvent.WorldReferenceFormId); cookie != 0)
    {
        DeferredPickup& pickup = m_deferredPickups[cookie];
        if (pickup.CreatedMs != 0)
        {
            // A second pickup of the same reference cannot happen in the engine; keep the first.
            return true;
        }
        pickup.PickerServerId = aPickerServerId;
        pickup.Item = acEvent.Item;
        pickup.WholeStack = acEvent.WholeStack;
        pickup.CreatedMs = std::max<uint64_t>(NowMs(), 1);
        return true;
    }

    return false;
}

void WorldItemService::ClaimDeferredPickup(uint32_t aCookie, WorldItems::ItemId aId) noexcept
{
    if (aCookie == 0)
        return;
    const auto it = m_deferredPickups.find(aCookie);
    if (it == m_deferredPickups.end())
        return;
    const DeferredPickup pickup = it->second;
    m_deferredPickups.erase(it);
    if (!SendClaim(aId, pickup.PickerServerId, pickup.Item, pickup.WholeStack))
        SendLegacyPickup(pickup);
}

void WorldItemService::SendLegacyPickup(const DeferredPickup& acPickup) const noexcept
{
    RequestInventoryChanges request{};
    request.ServerId = acPickup.PickerServerId;
    request.Item = acPickup.Item;
    request.Drop = true;
    request.UpdateClients = true;
    m_transport.Send(request);
}

void WorldItemService::FlushExpiredDeferredPickups(uint64_t aNowMs) noexcept
{
    for (auto it = m_deferredPickups.begin(); it != m_deferredPickups.end();)
    {
        // Still waiting for its echo: keep holding it (the mirror times the drop out).
        if (m_mirror.IsDropPending(it->first))
        {
            ++it;
            continue;
        }
        // The drop was never registered (refused or lost): the server treated it the legacy way, so
        // does the pickup.
        spdlog::info("[RFAB WorldItems] drop cookie {} was not registered; its pickup is sent the legacy way", it->first);
        SendLegacyPickup(it->second);
        it = m_deferredPickups.erase(it);
    }
    (void)aNowMs;
}

// --- server messages -----------------------------------------------------------------------------

void WorldItemService::NoteRevision(WorldItems::RevisionCheck aCheck) noexcept
{
    if (aCheck == WorldItems::RevisionCheck::kGap)
        m_world.GetSessionResyncService().NoteGap(Session::Resync::Scope::kWorldItems, Session::Resync::kNoServerId,
                                                  Session::Resync::Reason::kWorldItemRevisionGap);
}

void WorldItemService::OnSpawn(const NotifyWorldItemSpawn& acMessage) noexcept
{
    const WorldItems::RevisionCheck check = m_mirror.AcceptRevision(acMessage.Revision);
    if (check == WorldItems::RevisionCheck::kDuplicate)
        return;

    const WorldItemState& state = acMessage.State;
    const int32_t shown = ShownCount(state.Id);
    const auto effect = m_mirror.Upsert(state.Id, state.Item.Count, state.DropperPlayerId, state.DropperCookie, PlacementOf(state), state,
                                        m_transport.GetLocalPlayerId(), shown);

    if (effect.StaleFormId != 0)
        DeleteBoundReference(effect.StaleFormId);
    if (effect.AdoptedFormId != 0)
    {
        MarkServerOwned(ResolveBound(effect.AdoptedFormId, state));
        ClaimDeferredPickup(state.DropperCookie, state.Id);
    }

    NoteRevision(check);
}

void WorldItemService::OnDespawn(const NotifyWorldItemDespawn& acMessage) noexcept
{
    // Applied whatever the revision says: ids are never reused, a removal is final.
    const WorldItems::RevisionCheck check = m_mirror.AcceptRevision(acMessage.Revision);
    const auto binding = m_mirror.Remove(acMessage.ItemId);
    m_suspectedLost.erase(acMessage.ItemId);
    if (binding.FormId != 0)
        DeleteBoundReference(binding.FormId);

    if (acMessage.Reason == WorldItems::DespawnReason::kClaimRejected)
        spdlog::info("[RFAB WorldItems] claim on item {} rejected: somebody else took it first", acMessage.ItemId);

    NoteRevision(check);
}

void WorldItemService::OnSnapshot(const NotifyWorldItemSnapshot& acMessage) noexcept
{
    std::vector<Mirror::SnapshotEntry> entries;
    entries.reserve(acMessage.Items.size());
    for (const WorldItemState& state : acMessage.Items)
    {
        Mirror::SnapshotEntry entry{};
        entry.Id = state.Id;
        entry.Count = state.Item.Count;
        entry.DropperPlayerId = state.DropperPlayerId;
        entry.DropperCookie = state.DropperCookie;
        entry.Where = PlacementOf(state);
        entry.Payload = state;
        entry.ShownCount = ShownCount(state.Id);
        entries.push_back(std::move(entry));
    }

    auto effect = m_mirror.ApplySnapshot(acMessage.Revision, std::move(entries), m_transport.GetLocalPlayerId());

    for (const auto& binding : effect.Vanished)
        DeleteBoundReference(binding.FormId);
    for (const auto& binding : effect.Stale)
        DeleteBoundReference(binding.FormId);
    for (const auto& binding : effect.Adopted)
    {
        if (const Mirror::Record* pRecord = m_mirror.Find(binding.Id))
        {
            MarkServerOwned(ResolveBound(binding.FormId, pRecord->Payload));
            ClaimDeferredPickup(pRecord->DropperCookie, binding.Id);
        }
    }
    for (auto it = m_suspectedLost.begin(); it != m_suspectedLost.end();)
        it = m_mirror.Find(it->first) ? std::next(it) : m_suspectedLost.erase(it);

    spdlog::info("[RFAB WorldItems] snapshot: {} items at revision {} ({} removed here, {} rebuilt, {} adopted)", acMessage.Items.size(),
                 acMessage.Revision, effect.Vanished.size(), effect.Stale.size(), effect.Adopted.size());
}

// --- per-tick work -------------------------------------------------------------------------------

void WorldItemService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    m_tickAccumulatorSec += acEvent.Delta;
    if (m_tickAccumulatorSec < kTickIntervalSec)
        return;
    m_tickAccumulatorSec = 0.0;

    if (!m_transport.IsOnline())
        return;

    const uint64_t now = NowMs();
    if (const size_t expired = m_mirror.ExpirePendingDrops(now); expired > 0)
        spdlog::info("[RFAB WorldItems] {} own drops were never registered by the server; they stay local items", expired);
    FlushExpiredDeferredPickups(now);

    if (m_mirror.Size() == 0 || !CanTouchEngine())
        return;

    if (now >= m_lastValidationMs + kValidationIntervalMs || now < m_lastValidationMs)
    {
        m_lastValidationMs = now;
        ValidateBindings(now);
    }
    MaterializeInRange(now);
}

void WorldItemService::ValidateBindings(uint64_t aNowMs) noexcept
{
    for (const auto& binding : m_mirror.CollectBindings())
    {
        const Mirror::Record* pRecord = m_mirror.Find(binding.Id);
        if (!pRecord)
            continue;
        if (ResolveBound(binding.FormId, pRecord->Payload))
        {
            m_suspectedLost.erase(binding.Id);
            continue;
        }
        // Gone (cell reset, a script, a pickup whose event is still queued). Released at the second
        // look, then shown again when in range.
        const auto [it, inserted] = m_suspectedLost.try_emplace(binding.Id, aNowMs);
        if (!inserted)
        {
            m_suspectedLost.erase(it);
            m_mirror.NoteReferenceLost(binding.Id);
        }
    }
}

void WorldItemService::MaterializeInRange(uint64_t aNowMs) noexcept
{
    WorldItems::Placement viewer{};
    if (!LocalPlacement(viewer))
        return;

    size_t budget = kMaterializeBudgetPerTick;
    for (const WorldItems::ItemId id : m_mirror.CollectUnbound(aNowMs))
    {
        if (budget == 0)
            break;
        const Mirror::Record* pRecord = m_mirror.Find(id);
        if (!pRecord || !WorldItems::IsWithinMaterializeRange(pRecord->Where, viewer))
            continue;
        const Mirror::Record record = *pRecord;  // Bind/NoteMaterializeFailed mutate the map
        switch (Materialize(id, record))
        {
        case MaterializeResult::kCreated:
            ++m_materialized;
            --budget;
            break;
        case MaterializeResult::kFailed:
            m_mirror.NoteMaterializeFailed(id, aNowMs);
            --budget;
            break;
        case MaterializeResult::kNotNow:
            break;
        }
    }
}

WorldItemService::MaterializeResult WorldItemService::Materialize(WorldItems::ItemId aId, const Mirror::Record& acRecord) noexcept
{
    const WorldItemState& state = acRecord.Payload;
    auto& modSystem = m_world.GetModSystem();

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    ModManager* pModManager = ModManager::Get();
    if (!pPlayer || pPlayer->IsStillLoading() || !pPlayer->parentCell || !pModManager)
        return MaterializeResult::kNotNow;

    TESForm* pBase = TESForm::GetById(modSystem.GetGameId(state.Item.BaseId));
    if (!pBase)
    {
        spdlog::warn("[RFAB WorldItems] item {}: base form {:X} is not loaded here", aId, state.Item.BaseId.LogFormat());
        return MaterializeResult::kFailed;
    }

    // Only into a cell the engine has attached around the local player. NEVER TESWorldSpace::LoadCell:
    // force-loading exterior cells outside the uGridsToLoad grid corrupts the background
    // large-reference promotion (CharacterService::MoveActor, SE 1.5.97 +0x23D000).
    TESObjectCELL* pCell = nullptr;
    TESWorldSpace* pWorldSpace = nullptr;
    if (!state.WorldSpaceId)
    {
        pCell = Cast<TESObjectCELL>(TESForm::GetById(modSystem.GetGameId(state.CellId)));
        if (!pCell || !pCell->IsAttachedAndLoaded() || pPlayer->GetWorldSpace() != nullptr || pPlayer->parentCell != pCell)
            return MaterializeResult::kNotNow;
    }
    else
    {
        pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(modSystem.GetGameId(state.WorldSpaceId)));
        const TES* pTes = TES::Get();
        if (!pWorldSpace || !pTes || pPlayer->GetWorldSpace() != pWorldSpace)
            return MaterializeResult::kNotNow;

        const GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(state.Position.x, state.Position.y);
        const GridCellCoords playerCenterGrid(pTes->centerGridX, pTes->centerGridY);
        if (!GridCellCoords::IsCellInGridCell(coordinates, playerCenterGrid, false))
            return MaterializeResult::kNotNow;

        pCell = pModManager->GetCellFromCoordinates(coordinates.X, coordinates.Y, pWorldSpace, false);
        if (!pCell || !pCell->IsAttachedAndLoaded())
            return MaterializeResult::kNotNow;
    }

    NiPoint3 position{};
    position.x = state.Position.x;
    position.y = state.Position.y;
    position.z = state.Position.z;
    NiPoint3 rotation{};
    rotation.x = state.Rotation.x;
    rotation.y = state.Rotation.y;
    rotation.z = state.Rotation.z;

    const uint32_t handle = pModManager->SpawnReference(pBase, position, rotation, pCell, pWorldSpace);
    GamePtr<TESObjectREFR> spReference = TESObjectREFR::LookupByHandle(handle);
    if (!spReference || !spReference->IsTemporary())
    {
        spdlog::warn("[RFAB WorldItems] item {}: the engine did not create a reference for {:X}", aId, pBase->formID);
        return MaterializeResult::kFailed;
    }

    TESObjectREFR* pReference = spReference.Get();
    if (ExtraDataList* pExtra = pReference->GetExtraDataList())
    {
        const int32_t shown = std::clamp(acRecord.Count, 1, kMaxShownStack);
        if (shown > 1)
            pExtra->SetCount(static_cast<int16_t>(shown));
        TESObjectREFR::ApplyItemExtraData(pExtra, state.Item, /*include worn*/ false);
    }
    MarkServerOwned(pReference);

    if (!m_mirror.Bind(aId, pReference->formID, WorldItems::BindingOrigin::kMaterializedHere))
    {
        // The record vanished in between (cannot happen on the main thread, but never leak a copy).
        DeleteBoundReference(pReference->formID);
        return MaterializeResult::kFailed;
    }
    return MaterializeResult::kCreated;
}

// --- lifecycle -----------------------------------------------------------------------------------

void WorldItemService::OnSessionBegin(const Session::Transition& acTransition) noexcept
{
    (void)acTransition;
    // The mirror is NOT reset here: the server's join snapshot may already have been applied. It was
    // emptied when the previous session ended.
    m_tickAccumulatorSec = 0.0;
    m_lastValidationMs = 0;
}

void WorldItemService::OnSessionEnd(const Session::Transition& acTransition) noexcept
{
    (void)acTransition;
    // Every bound reference belongs to the server, own drops included once they were registered: they
    // stay in the server's world (and its checkpoints) and come back with the next join snapshot. Left
    // here they would be a second copy.
    const auto bindings = m_mirror.Reset();
    for (const auto& binding : bindings)
        DeleteBoundReference(binding.FormId);
    if (!m_deferredPickups.empty())
        spdlog::warn("[RFAB WorldItems] session ended with {} pickups of unconfirmed own drops", m_deferredPickups.size());
    m_deferredPickups.clear();
    m_suspectedLost.clear();
    spdlog::info("[RFAB WorldItems] session ended: {} references removed, {} materialized and {} claims sent this session",
                 bindings.size(), m_materialized, m_claimsSent);
    m_materialized = 0;
    m_claimsSent = 0;
}

void WorldItemService::OnWorldLoadBegin(const Session::Transition& acTransition) noexcept
{
    (void)acTransition;
    // The engine world goes away with every reference in it; the records stay (they are the server's)
    // and are shown again once the new world is loaded.
    m_mirror.ForgetEngineReferences();
    m_deferredPickups.clear();
    m_suspectedLost.clear();
}
