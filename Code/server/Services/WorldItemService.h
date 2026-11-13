#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <common/Session/WorldItems.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

struct World;
struct Player;
struct PlayerJoinEvent;
struct PlayerLeaveEvent;
struct RequestWorldItemDrop;
struct RequestWorldItemPickup;
struct NotifyWorldItemSnapshot;
struct WorldItemState;

// What the server remembers about a dropped item besides its count (WorldItems::Registry payload).
struct WorldItemPayload
{
    Inventory::Entry Item{};  // Item.Count is unused: the registry record holds the count
    GameId CellId{};
    GameId WorldSpaceId{};
    glm::vec3 Position{0.0f, 0.0f, 0.0f};
    glm::vec3 Rotation{0.0f, 0.0f, 0.0f};
};

using WorldItemRegistry = WorldItems::Registry<WorldItemPayload>;
using WorldItemRecord = WorldItemRegistry::Record;

/**
 * Server half of the server-owned dropped items (architecture step 4, common/Session/WorldItems.h).
 *
 *  - RequestWorldItemDrop: moves the items out of the dropper's inventory (the one inventory writer,
 *    InventoryService::ApplyInventoryDelta) and registers the item; every joined player gets
 *    NotifyWorldItemSpawn. A drop the registry refuses (bounds, rate) falls back to the legacy relay,
 *    so the item behaves exactly as before 4.1 instead of vanishing.
 *  - RequestWorldItemPickup: a claim. The first claim processed wins; the item goes into the picker's
 *    inventory from the server's record, everything not granted is rolled back on the claimer only.
 *  - A join, a Session::Resync::Scope::kWorldItems request and a checkpoint rollback send the whole
 *    NotifyWorldItemSnapshot.
 *
 * Gated by Session::Feature::kServerOwnedWorldItems for NEW drops only: claims for items that exist are
 * always arbitrated, so switching the feature off live can never leave a registered item that two
 * players can both take.
 */
class WorldItemService
{
public:
    WorldItemService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~WorldItemService() noexcept = default;

    TP_NOCOPYMOVE(WorldItemService);

    // Resync / join: the whole registry to one player. Returns how many items were sent.
    uint32_t SendSnapshotTo(Player* apPlayer) const noexcept;
    // Checkpoint rollback / admin: the whole registry to every player.
    void BroadcastSnapshot() const noexcept;

    // Checkpoints (SaveSnapshotService).
    [[nodiscard]] std::vector<WorldItemRecord> CaptureCheckpoint() const { return m_registry.Export(); }
    // Replaces the registry with a checkpoint's content. Does not broadcast (the caller decides when
    // the clients can take it); returns how many items were restored.
    size_t RestoreCheckpoint(std::vector<WorldItemRecord> aRecords);
    // Removes every item and tells every player. Returns how many items were removed.
    size_t ClearAll() noexcept;

    [[nodiscard]] const WorldItemRegistry& GetRegistry() const noexcept { return m_registry; }

    // Conversions shared with the checkpoint code.
    [[nodiscard]] static WorldItemState ToWireState(const WorldItemRecord& acRecord) noexcept;
    [[nodiscard]] static uint64_t CellKeyOf(const GameId& acCellId) noexcept;

private:
    void OnDropRequest(const PacketEvent<RequestWorldItemDrop>& acMessage) noexcept;
    void OnPickupRequest(const PacketEvent<RequestWorldItemPickup>& acMessage) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

    // The sender's own character, or an NPC whose authority lease the sender holds.
    [[nodiscard]] bool IsActorControlledBy(uint32_t aServerId, const Player* apPlayer) const noexcept;
    // Takes back from the claimer's actor what its engine added but the server did not grant.
    void RollBackOptimisticPickup(Player* apPlayer, uint32_t aPickerServerId, const Inventory::Entry& acItem, int32_t aCount) const noexcept;
    void BuildSnapshot(NotifyWorldItemSnapshot& aOut) const noexcept;

    World& m_world;
    WorldItemRegistry m_registry{};
    uint64_t m_dropsAccepted{0};
    uint64_t m_dropsRefused{0};
    uint64_t m_claimsGranted{0};
    uint64_t m_claimsLost{0};

    entt::scoped_connection m_dropConnection;
    entt::scoped_connection m_pickupConnection;
    entt::scoped_connection m_joinConnection;
    entt::scoped_connection m_leaveConnection;
};
