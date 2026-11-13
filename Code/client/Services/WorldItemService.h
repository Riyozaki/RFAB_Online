#pragma once

#include <Structs/Inventory.h>
#include <Structs/WorldItemState.h>
#include <common/Session/Lifecycle.h>
#include <common/Session/WorldItems.h>

#include <cstdint>
#include <unordered_map>

struct World;
struct TransportService;
struct UpdateEvent;
struct InventoryChangeEvent;
struct NotifyWorldItemSpawn;
struct NotifyWorldItemDespawn;
struct NotifyWorldItemSnapshot;
struct TESObjectREFR;

/**
 * Client half of the server-owned dropped items (architecture step 4, common/Session/WorldItems.h;
 * server: Code/server/Services/WorldItemService.cpp).
 *
 * The server's registry is the truth; this service mirrors it (WorldItems::ClientMirror) and keeps each
 * item bound to the ONE engine reference that shows it here:
 *
 *  - Local drop (Actor::DropObject hook -> InventoryChangeEvent with the created reference): with
 *    Session::Feature::kServerOwnedWorldItems on, RequestWorldItemDrop replaces the legacy
 *    RequestInventoryChanges(Drop). The reference stays where the engine put it and is adopted when the
 *    server's echo carries our cookie. Off, or if the request cannot be built: the legacy path.
 *  - Local pickup of a bound reference: RequestWorldItemPickup, a claim the server arbitrates (first
 *    processed wins, the loser's optimistic pickup is rolled back by NotifyInventoryChanges). Claims are
 *    sent whatever the feature switch says: an item that exists is always arbitrated. An unbound
 *    temporary reference keeps the legacy path (common/DroppedItemMatch.h).
 *  - NotifyWorldItemSpawn / Despawn / Snapshot: create, rebuild or delete references BY ID. Nobody
 *    matches stacks by distance any more. A revision gap asks for a snapshot
 *    (Session::Resync::Scope::kWorldItems).
 *  - Materialization: only in cells the engine has attached (never TESWorldSpace::LoadCell), within
 *    WorldItems::kExteriorMaterializeRadius, a few per tick, with back-off and quarantine.
 *
 * Every reference this service owns is SkipSave: it belongs to the server, a save game must never
 * bring back a second copy. Session::ParticipantId::kWorldItems (stage kWorldModel, after kInventory).
 * Main thread only.
 */
struct WorldItemService final : Session::IParticipant
{
    using Mirror = WorldItems::ClientMirror<WorldItemState>;

    WorldItemService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~WorldItemService() noexcept = default;

    TP_NOCOPYMOVE(WorldItemService);

    /**
     * InventoryService hands every connected Drop event of a synced actor here first. Returns true when
     * the server-owned path took it (a RequestWorldItemDrop / RequestWorldItemPickup was sent), false
     * for the legacy RequestInventoryChanges.
     */
    bool HandleLocalDrop(const InventoryChangeEvent& acEvent, uint32_t aActorServerId) noexcept;
    bool HandleLocalPickup(const InventoryChangeEvent& acEvent, uint32_t aPickerServerId) noexcept;

    [[nodiscard]] const Mirror& GetMirror() const noexcept { return m_mirror; }
    // Does this reference show a server-owned item here? The legacy DroppedItemMatch guess must never
    // delete one. Main thread.
    [[nodiscard]] bool IsBoundReference(uint32_t aFormId) const noexcept { return m_mirror.FindByFormId(aFormId) != WorldItems::kNoItem; }

    void OnSessionBegin(const Session::Transition& acTransition) noexcept override;
    void OnSessionEnd(const Session::Transition& acTransition) noexcept override;
    void OnWorldLoadBegin(const Session::Transition& acTransition) noexcept override;

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnSpawn(const NotifyWorldItemSpawn& acMessage) noexcept;
    void OnDespawn(const NotifyWorldItemDespawn& acMessage) noexcept;
    void OnSnapshot(const NotifyWorldItemSnapshot& acMessage) noexcept;

    void NoteRevision(WorldItems::RevisionCheck aCheck) noexcept;
    // Where a wire item lies, in the mirror's key space.
    [[nodiscard]] static WorldItems::Placement PlacementOf(const WorldItemState& acState) noexcept;
    // Where the local player stands; false while there is no usable player/cell.
    [[nodiscard]] bool LocalPlacement(WorldItems::Placement& aOut) const noexcept;
    [[nodiscard]] bool CanTouchEngine() const noexcept;

    // The live reference bound to an item, or nullptr when it is gone or shows another base form.
    [[nodiscard]] TESObjectREFR* ResolveBound(uint32_t aFormId, const WorldItemState& acState) const noexcept;
    [[nodiscard]] int32_t ShownCount(WorldItems::ItemId aId) const noexcept;
    // Marks a reference as server-owned: never written to a save game.
    static void MarkServerOwned(TESObjectREFR* apReference) noexcept;
    // Deletes a reference this service bound (spawned or adopted). Main thread, not while loading.
    void DeleteBoundReference(uint32_t aFormId) noexcept;

    void ValidateBindings(uint64_t aNowMs) noexcept;
    void MaterializeInRange(uint64_t aNowMs) noexcept;
    enum class MaterializeResult : uint8_t
    {
        kCreated,
        kNotNow,  // not attached here / outside the loaded grid: tried again later, not a failure
        kFailed,  // the engine could not create it (unknown base form, spawn failed): back-off
    };
    [[nodiscard]] MaterializeResult Materialize(WorldItems::ItemId aId, const Mirror::Record& acRecord) noexcept;

    [[nodiscard]] static uint64_t NowMs() noexcept;
    [[nodiscard]] uint32_t NextCookie() noexcept;

    // A pickup of an own drop whose echo has not come yet: claimed once the echo binds the reference,
    // sent the legacy way if the drop is never registered.
    struct DeferredPickup
    {
        uint32_t PickerServerId{0};
        Inventory::Entry Item{};
        bool WholeStack{true};
        uint64_t CreatedMs{0};
    };
    bool SendClaim(WorldItems::ItemId aId, uint32_t aPickerServerId, const Inventory::Entry& acItem, bool aWholeStack) noexcept;
    void ClaimDeferredPickup(uint32_t aCookie, WorldItems::ItemId aId) noexcept;
    void FlushExpiredDeferredPickups(uint64_t aNowMs) noexcept;
    void SendLegacyPickup(const DeferredPickup& acPickup) const noexcept;

    World& m_world;
    TransportService& m_transport;
    Mirror m_mirror{};
    uint32_t m_nextCookie{1};
    double m_tickAccumulatorSec{0.0};
    uint64_t m_lastValidationMs{0};
    // Item ids whose bound reference looked gone at the last validation: released only at the next
    // one, so the queued event of a whole-stack pickup still finds its binding.
    std::unordered_map<WorldItems::ItemId, uint64_t> m_suspectedLost{};
    std::unordered_map<uint32_t, DeferredPickup> m_deferredPickups{};  // by drop cookie
    uint32_t m_claimsSent{0};
    uint32_t m_materialized{0};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_spawnConnection;
    entt::scoped_connection m_despawnConnection;
    entt::scoped_connection m_snapshotConnection;
};
