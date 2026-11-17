#pragma once
#include <Structs/CoopProtocol.h>
#include <Coop/ResetParticipant.h>
#include <Coop/Readiness.h>
#include <Games/Events.h>
#include <map>
#include <string>
#include <atomic>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct HitEvent;
struct NotifyCoopAction;
struct WorldLoadedEvent;
struct Actor;

struct CoopService
{
    CoopService(World&, entt::dispatcher&, TransportService&) noexcept;
    ~CoopService() noexcept;
    TP_NOCOPYMOVE(CoopService);
    bool Active() const noexcept { return m_active.load(std::memory_order_acquire); }
    bool Downed(uint32_t aServerId) const noexcept;
    bool Supports(uint32_t aCapabilities) const noexcept { return Active() && (m_capabilities & aCapabilities) == aCapabilities; }
    uint64_t Session() const noexcept { return m_session; }
    uint64_t Generation() const noexcept { return m_generation; }
    void SendAction(Coop::Body aBody) noexcept { Send(std::move(aBody)); }
    void HoldInventory(bool aHold) noexcept { m_inventoryHeld = aHold; SetPaused(m_paused); }
    void OnWorldLoaded(const WorldLoadedEvent&) noexcept;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnConnected(const ConnectedEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnHit(const HitEvent&) noexcept;
    void OnPacket(const NotifyCoopAction&) noexcept;
    void Apply(const Coop::DownedState&) noexcept;
    void Apply(const Coop::EncounterState&) noexcept;
    void Apply(const Coop::ResetPlan&) noexcept;
    template <class T> void Apply(const T&) noexcept {}
    void Send(Coop::Body) noexcept;
    void ApplyHealth(uint32_t aActor, uint32_t aEpoch, uint64_t aRevision, float aHealth) noexcept;
    void RetargetOnce(uint32_t aDownedActor) noexcept;
    void ObserveEnemies() noexcept;
    void UpdateResetReadiness() noexcept;
    void SetPaused(bool) noexcept;
    Coop::Presence Observe() noexcept;
    bool CanInteract() const noexcept;
    uint32_t LocalActor() const noexcept;
    uint64_t Now() const noexcept;

    World& m_world;
    TransportService& m_transport;
    std::atomic_bool m_active{false};
    uint64_t m_session{}, m_generation{}, m_operation{}, m_serverOperation{}, m_helloAt{}, m_lastPoll{};
    uint64_t m_safeSince{}, m_lastAnchor{}, m_hold{}, m_holdCounter{}, m_recoverUntil{}, m_recoverySession{};
    uint64_t m_encounter{}, m_lastEnemyScan{};
    Coop::ReadinessGate m_resetReadiness;
    std::map<uint32_t, uint32_t> m_reportedEnemies;
    Coop::ResetParticipant m_reset;
    Coop::ResetPlan m_pendingReset;
    uint32_t m_capabilities{}, m_target{}, m_hitSerial{};
    bool m_paused{}, m_inventoryHeld{};
    std::string m_character;
    Coop::Pose m_safeOrigin;
    std::map<uint32_t, Coop::DownedState> m_downed;
    std::map<uint32_t, uint32_t> m_appliedRevision, m_appliedFormId;
    entt::scoped_connection m_update, m_connected, m_disconnected, m_packet, m_hit, m_loaded;
};
