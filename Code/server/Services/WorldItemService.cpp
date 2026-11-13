#include <Services/WorldItemService.h>

#include <Components.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Game/Player.h>
#include <GameServer.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyWorldItemDespawn.h>
#include <Messages/NotifyWorldItemSnapshot.h>
#include <Messages/NotifyWorldItemSpawn.h>
#include <Messages/RequestWorldItemDrop.h>
#include <Messages/RequestWorldItemPickup.h>
#include <Services/InventoryService.h>
#include <World.h>
#include <common/CoopPacketSafety.h>

#include <Setting.h>
#include <chrono>

// Owned by InventoryService.cpp: with world item sync off entirely no path syncs dropped items.
extern Console::Setting<bool> bEnableItemDrops;

namespace
{
uint64_t WorldItemNowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Same clamps InventoryService::ApplyInventoryDelta applies, so the registry never stores a value the
// inventory writer would have refused.
Inventory::Entry SanitizeWorldItemEntry(const Inventory::Entry& acItem) noexcept
{
    Inventory::Entry item = acItem;
    item.ExtraCharge = CoopPacketSafety::SanitizeFiniteFloat(item.ExtraCharge, 0.0f, 100000.0f, 0.0f);
    item.ExtraHealth = CoopPacketSafety::SanitizeFiniteFloat(item.ExtraHealth, 0.0f, 100000.0f, 0.0f);
    // A dropped item is never worn.
    item.ExtraWorn = false;
    item.ExtraWornLeft = false;
    return item;
}
} // namespace

Console::Command<> ClearWorldItems(
    "ClearWorldItems", "Removes every synced dropped item from the world (all players)",
    [](Console::ArgStack&)
    {
        auto* pServer = GameServer::Get();
        if (!pServer || !pServer->HasWorld())
            return;
        const size_t removed = pServer->GetWorld().ctx().at<WorldItemService>().ClearAll();
        spdlog::get("ConOut")->info("Removed {} dropped items.", removed);
    });

Console::Command<> ListWorldItems(
    "ListWorldItems", "Shows how many synced dropped items exist and the registry revision",
    [](Console::ArgStack&)
    {
        auto* pServer = GameServer::Get();
        if (!pServer || !pServer->HasWorld())
            return;
        const auto& registry = pServer->GetWorld().ctx().at<WorldItemService>().GetRegistry();
        spdlog::get("ConOut")->info("{} dropped items (max {}), revision {}.", registry.Size(), registry.GetPolicy().MaxItems,
                                    registry.GetRevision());
    });

WorldItemService::WorldItemService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_dropConnection(aDispatcher.sink<PacketEvent<RequestWorldItemDrop>>().connect<&WorldItemService::OnDropRequest>(this))
    , m_pickupConnection(aDispatcher.sink<PacketEvent<RequestWorldItemPickup>>().connect<&WorldItemService::OnPickupRequest>(this))
    , m_joinConnection(aDispatcher.sink<PlayerJoinEvent>().connect<&WorldItemService::OnPlayerJoin>(this))
    , m_leaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&WorldItemService::OnPlayerLeave>(this))
{
}

uint64_t WorldItemService::CellKeyOf(const GameId& acCellId) noexcept
{
    return WorldItems::PackKey(acCellId.ModId, acCellId.BaseId);
}

WorldItemState WorldItemService::ToWireState(const WorldItemRecord& acRecord) noexcept
{
    WorldItemState state{};
    state.Id = acRecord.Id;
    state.Item = acRecord.Payload.Item;
    state.Item.Count = acRecord.Count;
    state.CellId = acRecord.Payload.CellId;
    state.WorldSpaceId = acRecord.Payload.WorldSpaceId;
    state.Position = acRecord.Payload.Position;
    state.Rotation = acRecord.Payload.Rotation;
    state.DropperPlayerId = acRecord.DropperPlayerId;
    state.DropperCookie = acRecord.DropperCookie;
    return state;
}

bool WorldItemService::IsActorControlledBy(uint32_t aServerId, const Player* apPlayer) const noexcept
{
    if (!apPlayer)
        return false;
    const entt::entity entity = static_cast<entt::entity>(aServerId);
    if (!m_world.valid(entity))
        return false;
    const auto* pOwner = m_world.try_get<OwnerComponent>(entity);
    return pOwner && pOwner->GetOwner() == apPlayer;
}

void WorldItemService::OnDropRequest(const PacketEvent<RequestWorldItemDrop>& acMessage) noexcept
{
    Player* pPlayer = acMessage.pPlayer;
    const RequestWorldItemDrop& request = acMessage.Packet;
    if (!pPlayer)
        return;

    const int32_t count = request.Item.Count;
    if (!request.Item.BaseId || count <= 0 || !request.CellId)
    {
        spdlog::warn("[RFAB WorldItems] refused malformed drop from player {} (count {})", pPlayer->GetId(), count);
        return;
    }
    if (!IsActorControlledBy(request.ActorServerId, pPlayer))
    {
        spdlog::warn("[RFAB WorldItems] refused drop from player {} for actor {:X} they do not control", pPlayer->GetId(),
                     request.ActorServerId);
        return;
    }

    Inventory::Entry removal = SanitizeWorldItemEntry(request.Item);
    removal.Count = -count;

    // The feature can be switched off live: a request already on its way then takes the legacy path.
    const bool registryEnabled = bEnableItemDrops && IsCoopFeatureEnabled(Session::Feature::kServerOwnedWorldItems);
    WorldItems::ItemId id = WorldItems::kNoItem;
    WorldItems::DropResult result = WorldItems::DropResult::kInvalidCount;
    if (registryEnabled)
    {
        WorldItemPayload payload{};
        payload.Item = SanitizeWorldItemEntry(request.Item);
        payload.Item.Count = 0;
        payload.CellId = request.CellId;
        payload.WorldSpaceId = request.WorldSpaceId;
        payload.Position = request.Position;
        payload.Rotation = request.Rotation;
        result = m_registry.TryDrop(pPlayer->GetId(), CellKeyOf(request.CellId), count, request.Cookie, std::move(payload),
                                    WorldItemNowMs(), id);
    }
    const bool registered = result == WorldItems::DropResult::kAccepted;

    // Inventory first: the peers' puppet loses the items before (or with) the spawn. A refused drop
    // keeps the legacy relay: the peers in range replay it on the dropper's puppet, as before 4.1.
    const bool legacyReplay = !registered && bEnableItemDrops;
    if (!m_world.GetInventoryService().ApplyInventoryDelta(pPlayer, request.ActorServerId, removal, /*world transfer*/ true,
                                                           legacyReplay, /*update clients*/ true))
    {
        spdlog::warn("[RFAB WorldItems] inventory refused the drop of {}x {:X}:{:X} by actor {:X}", count, request.Item.BaseId.ModId,
                     request.Item.BaseId.BaseId, request.ActorServerId);
    }

    if (!registered)
    {
        ++m_dropsRefused;
        if (registryEnabled)
            spdlog::info("[RFAB WorldItems] drop by player {} not registered ({}); it stays a local item of the dropper",
                         pPlayer->GetId(), WorldItems::ToString(result));
        return;
    }

    ++m_dropsAccepted;
    const WorldItemRecord* pRecord = m_registry.Find(id);
    if (!pRecord)
        return;

    NotifyWorldItemSpawn spawn{};
    spawn.Revision = m_registry.GetRevision();
    spawn.State = ToWireState(*pRecord);
    // Everybody, the dropper included: its echo pairs the cookie with the reference it created.
    GameServer::Get()->SendToPlayers(spawn);

    spdlog::debug("[RFAB WorldItems] item {} ({}x {:X}:{:X}) dropped by player {}, revision {}", id, count, request.Item.BaseId.ModId,
                  request.Item.BaseId.BaseId, pPlayer->GetId(), spawn.Revision);
}

void WorldItemService::RollBackOptimisticPickup(Player* apPlayer, uint32_t aPickerServerId, const Inventory::Entry& acItem,
                                                int32_t aCount) const noexcept
{
    if (!apPlayer || aCount <= 0)
        return;

    // The server never credited this part, so only the claimer's engine has it.
    NotifyInventoryChanges rollback{};
    rollback.ServerId = aPickerServerId;
    rollback.Item = SanitizeWorldItemEntry(acItem);
    rollback.Item.Count = -aCount;
    rollback.Drop = false;
    apPlayer->Send(rollback);
}

void WorldItemService::OnPickupRequest(const PacketEvent<RequestWorldItemPickup>& acMessage) noexcept
{
    Player* pPlayer = acMessage.pPlayer;
    const RequestWorldItemPickup& request = acMessage.Packet;
    if (!pPlayer)
        return;

    const int32_t requested = request.Item.Count;
    if (requested <= 0 || !IsActorControlledBy(request.PickerServerId, pPlayer))
    {
        // Nothing sane to roll back onto: an actor the sender does not control is not theirs to fix.
        spdlog::warn("[RFAB WorldItems] refused pickup of item {} by player {} (actor {:X}, count {})", request.ItemId, pPlayer->GetId(),
                     request.PickerServerId, requested);
        return;
    }

    const WorldItemRecord* pRecord = m_registry.Find(request.ItemId);
    if (pRecord && pRecord->Payload.Item.BaseId != request.Item.BaseId)
    {
        // The claimer's binding shows another item: its mirror is wrong, not the registry. Undo the
        // pickup and give it the authoritative list.
        spdlog::warn("[RFAB WorldItems] player {} claimed item {} as {:X}:{:X} but it is {:X}:{:X}; sending a snapshot", pPlayer->GetId(),
                     request.ItemId, request.Item.BaseId.ModId, request.Item.BaseId.BaseId, pRecord->Payload.Item.BaseId.ModId,
                     pRecord->Payload.Item.BaseId.BaseId);
        RollBackOptimisticPickup(pPlayer, request.PickerServerId, request.Item, requested);
        (void)SendSnapshotTo(pPlayer);
        return;
    }

    // Copy what the grant needs before TryClaim may erase the record.
    Inventory::Entry grantedItem = pRecord ? pRecord->Payload.Item : Inventory::Entry{};
    const auto claim = m_registry.TryClaim(request.ItemId, requested);

    if (claim.Granted > 0)
    {
        ++m_claimsGranted;
        grantedItem.Count = claim.Granted;
        if (!m_world.GetInventoryService().ApplyInventoryDelta(pPlayer, request.PickerServerId, grantedItem, /*world transfer*/ true,
                                                               /*notify drop*/ false, /*update clients*/ true))
        {
            spdlog::warn("[RFAB WorldItems] inventory refused crediting item {} to actor {:X}", request.ItemId, request.PickerServerId);
        }
    }
    else
    {
        ++m_claimsLost;
    }

    // Whatever the claimer's engine took beyond the grant goes back.
    RollBackOptimisticPickup(pPlayer, request.PickerServerId, request.Item, requested - claim.Granted);

    switch (claim.Result)
    {
    case WorldItems::ClaimResult::kGrantedAll:
    {
        NotifyWorldItemDespawn despawn{};
        despawn.Revision = m_registry.GetRevision();
        despawn.ItemId = request.ItemId;
        despawn.Reason = WorldItems::DespawnReason::kPickedUp;
        despawn.PickerPlayerId = pPlayer->GetId();
        GameServer::Get()->SendToPlayers(despawn);
        break;
    }
    case WorldItems::ClaimResult::kGrantedPartial:
    {
        if (const WorldItemRecord* pRemaining = m_registry.Find(request.ItemId))
        {
            NotifyWorldItemSpawn upsert{};
            upsert.Revision = m_registry.GetRevision();
            upsert.State = ToWireState(*pRemaining);
            GameServer::Get()->SendToPlayers(upsert);
        }
        break;
    }
    case WorldItems::ClaimResult::kUnknownItem:
    {
        // Lost the race (or the claimer's mirror is stale): the item is not in the world. Only this
        // claimer is told; the revision did not move, the client applies a despawn regardless.
        NotifyWorldItemDespawn despawn{};
        despawn.Revision = m_registry.GetRevision();
        despawn.ItemId = request.ItemId;
        despawn.Reason = WorldItems::DespawnReason::kClaimRejected;
        pPlayer->Send(despawn);
        spdlog::info("[RFAB WorldItems] player {} lost the claim on item {}; {}x rolled back", pPlayer->GetId(), request.ItemId,
                     requested);
        break;
    }
    case WorldItems::ClaimResult::kInvalidCount:
        break;
    }
}

void WorldItemService::BuildSnapshot(NotifyWorldItemSnapshot& aOut) const noexcept
{
    aOut.Revision = m_registry.GetRevision();
    const auto records = m_registry.Export();
    aOut.Items.clear();
    aOut.Items.reserve(records.size());
    for (const auto& record : records)
        aOut.Items.push_back(ToWireState(record));
}

uint32_t WorldItemService::SendSnapshotTo(Player* apPlayer) const noexcept
{
    if (!apPlayer)
        return 0;
    NotifyWorldItemSnapshot snapshot{};
    BuildSnapshot(snapshot);
    apPlayer->Send(snapshot);
    return static_cast<uint32_t>(snapshot.Items.size());
}

void WorldItemService::BroadcastSnapshot() const noexcept
{
    NotifyWorldItemSnapshot snapshot{};
    BuildSnapshot(snapshot);
    GameServer::Get()->SendToPlayers(snapshot);
}

size_t WorldItemService::RestoreCheckpoint(std::vector<WorldItemRecord> aRecords)
{
    const size_t offered = aRecords.size();
    const size_t restored = m_registry.Import(std::move(aRecords));
    spdlog::info("[RFAB WorldItems] restored {} of {} dropped items from the checkpoint (revision {})", restored, offered,
                 m_registry.GetRevision());
    return restored;
}

size_t WorldItemService::ClearAll() noexcept
{
    const size_t removed = m_registry.Size();
    m_registry.Clear();
    BroadcastSnapshot();
    return removed;
}

void WorldItemService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    // Always, even with the feature off: items registered before a live switch-off still exist and
    // are still arbitrated, so a new player must know them.
    const uint32_t sent = SendSnapshotTo(acEvent.pPlayer);
    if (sent > 0)
        spdlog::info("[RFAB WorldItems] sent {} dropped items to joining player {}", sent, acEvent.pPlayer->GetId());
}

void WorldItemService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (acEvent.pPlayer)
        m_registry.ForgetPlayer(acEvent.pPlayer->GetId());
}
