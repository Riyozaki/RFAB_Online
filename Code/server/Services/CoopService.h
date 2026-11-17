#pragma once
#include <Coop/Encounter.h>
#include <Coop/InventoryLedger.h>
#include <Coop/StorageCatalog.h>
#include <Coop/DurableFile.h>
#include <Coop/DurableLease.h>
#include <Events/PacketEvent.h>
#include <Messages/RequestCoopAction.h>
#include <map>
#include <set>
#include <memory>
#include <string>

struct World;
struct Player;
struct UpdateEvent;
struct PlayerLeaveEvent;

struct CoopService
{
    CoopService(World&, entt::dispatcher&);
    ~CoopService() = default;
    TP_NOCOPYMOVE(CoopService);

    static bool Enabled() noexcept;
    bool Manages(uint32_t aActor) const noexcept;
    bool IsDowned(uint32_t aActor) const noexcept;
    void Tick(uint64_t aNowMs) noexcept;
    uint64_t Session() const noexcept { return m_session; }
    uint64_t Generation() const noexcept { return m_generation; }
    bool UsesTransactionalInventory(Player*) const noexcept;
    bool InventoryLocked(uint32_t aEntity) const noexcept;
    bool AllowsLegacyInventory(Player*, uint32_t aEntity) const noexcept;
    bool ObserveInventory(uint32_t aEntity, Inventory& aContent, uint32_t aEpoch) noexcept;
    // Returns the durable contents/revision on re-registration; never seeds a new personal snapshot over it.
    bool BindObjectInventory(uint32_t aEntity) noexcept;

private:
    struct Client
    {
        uint32_t PlayerId{}, Actor{}, Group{}, Capabilities{};
        uint64_t LastOperation{};
        std::map<uint64_t, std::vector<uint8_t>> Retries; // bounded exact envelopes, idempotent inventory only
        std::string Character;
        bool Downed{};
    };
    struct Group
    {
        Coop::EncounterRuntime Encounter;
        std::set<uint32_t> Enemies;
        uint64_t LastEncounter{};
    };
    struct Scaling
    {
        float Base{}, Maximum{};
    };
    void OnPacket(const PacketEvent<RequestCoopAction>&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnLeave(const PlayerLeaveEvent&) noexcept;
    void Hello(Player*, const Coop::Packet&, const Coop::Hello&) noexcept;
    void Handle(Player*, Client&, const Coop::Presence&) noexcept;
    void Handle(Player*, Client&, const Coop::Revive&) noexcept;
    void Handle(Player*, Client&, const Coop::Anchor&) noexcept;
    void Handle(Player*, Client&, const Coop::ResetReceipt&) noexcept;
    void Handle(Player*, Client&, const Coop::EnemyContact&) noexcept;
    void Handle(Player*, Client&, const Coop::Transfer&) noexcept;
    void Handle(Player*, Client&, const Coop::TransferReceipt&) noexcept;
    void Handle(Player*, Client&, const Coop::InventoryQuery&) noexcept;
    void Handle(Player*, Client&, const Coop::StorageOpen&) noexcept;
    void InitializeInventory() noexcept;
    void TickInventory() noexcept;
    bool InventoryReady() const noexcept;
    bool RestoreInventoryFor(Player*) noexcept;
    bool BindInventory(uint32_t aEntity, const std::string& aKey, bool aOwnBag) noexcept;
    std::optional<std::string> InventoryKey(uint32_t aEntity) const;
    std::optional<Coop::StableForm> NameForm(GameId) const;
    bool ItemAvailable(Player*, const Inventory::Entry&) const noexcept;
    bool CanAccessInventory(Player*, const Client&, uint32_t aEntity, uint8_t aReason) const noexcept;
    void SendInventory(Player*, uint32_t aEntity) noexcept;
    Coop::TransferReply TransferReply(const Coop::InventoryLedger::Record&, uint32_t aRequester) const;
    void PublishTransfer(const Coop::InventoryLedger::Record&, uint32_t aRequester, uint32_t aGroup) noexcept;
    template <class T> void Handle(Player*, Client&, const T&) noexcept {} // no implicit support for unimplemented adapters
    void Flush(uint32_t aGroup) noexcept;
    void Send(Player*, Coop::Body) noexcept;
    void Broadcast(uint32_t aGroup, const Coop::Body&) noexcept;
    void FillRecovery(Coop::DownedState&) noexcept;
    void FillReset(uint32_t aGroup, Coop::ResetPlan&) noexcept;
    uint32_t AdvanceEpoch(uint32_t aActor, float aHealth) noexcept;
    uint64_t Now() const noexcept;

    World& m_world;
    uint64_t m_session{}, m_generation{1}, m_now{}, m_sequence{};
    std::map<uint32_t, Client> m_clients;
    std::map<uint32_t, Group> m_groups;
    std::map<std::string, std::pair<uint32_t, uint32_t>> m_characters;
    std::map<uint32_t, Scaling> m_scaling;
    std::string m_campaign;
    std::unique_ptr<Coop::InventoryLedger> m_inventory;
    std::unique_ptr<Coop::DurableFile> m_inventoryFile;
    std::unique_ptr<Coop::DurableLease> m_inventoryLease;
    std::optional<std::vector<uint8_t>> m_pendingInventory;
    Coop::StorageCatalog m_storageCatalog;
    std::map<uint32_t, std::string> m_inventoryBindings;
    std::map<std::string, std::string> m_storageAccess; // canonical access ref -> canonical backing ref
    struct StorageAccess { GameId Point{}; uint64_t Seen{}; };
    std::map<uint32_t, std::map<std::string, StorageAccess>> m_openStorage;
    entt::scoped_connection m_packetConnection, m_updateConnection, m_leaveConnection;
};
