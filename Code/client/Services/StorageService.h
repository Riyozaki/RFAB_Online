#pragma once
#include <Coop/StorageCatalog.h>
#include <Structs/CoopProtocol.h>
#include <ThreadAffinity.h>
#include <map>

struct World;
struct ActivateEvent;
struct CoopActionEvent;
struct UpdateEvent;
struct DisconnectedEvent;
struct WorldLoadedEvent;

// The access reference is only a nearby permission. The hidden backing has ONE campaign
// identity even when its own cell is unloaded and two players open different city access points.
struct StorageService
{
    StorageService(World&, entt::dispatcher&) noexcept;
    TP_NOCOPYMOVE(StorageService);
    bool Available() const noexcept { return m_valid; }
    uint64_t Signature() const noexcept { return m_valid ? m_catalog.Signature() : 0; }
    bool IsAccess(uint32_t form) const noexcept { return m_access.contains(form); }
    static RFAB_ANY_THREAD bool IsBacking(uint32_t form) noexcept;
    bool EnsureBacking(uint32_t form) noexcept;
    // Explicit local console actions, never remote commands or an inferred "first opener wins".
    void ConfirmImport() noexcept;
    void ConfirmAttach() noexcept;
private:
    struct Open
    {
        uint32_t Access{};
        uint64_t Began{}, LastSend{}, Operation{};
        bool SawMenu{}, Attach{}, Import{}, Notified{};
        Coop::StorageState Desired;
        Inventory ImportItems;
    };
    void OnLoaded(const WorldLoadedEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnActivate(const ActivateEvent&) noexcept;
    void OnPacket(const CoopActionEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void Reconcile(uint32_t backing, Open) noexcept;
    bool NearAccess(uint32_t form) const noexcept;
    static uint64_t Now() noexcept;
    World& m_world;
    Coop::StorageCatalog m_catalog;
    bool m_valid{}, m_executing{};
    uint32_t m_lastOpen{};
    std::map<uint32_t, uint32_t> m_access;
    std::map<uint32_t, Coop::StableForm> m_backings;
    std::map<uint32_t, Open> m_open;
    entt::scoped_connection m_loaded, m_disconnected, m_activate, m_packet, m_update;
};
