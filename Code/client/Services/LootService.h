#pragma once
#include <Coop/InventoryLedger.h>
#include <Coop/DurableFile.h>
#include <Structs/CoopProtocol.h>
#include <deque>
#include <map>
#include <set>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct WorldLoadedEvent;
struct CoopActionEvent;
struct TESObjectREFR;
struct TESBoundObject;
struct ExtraDataList;
enum class ITEM_REMOVE_REASON;

// Network decisions never retain native pointers. They re-resolve forms and the exact live
// ExtraDataList, execute one original source->destination transfer and verify both endpoints.
struct LootService
{
    LootService(World&, entt::dispatcher&, TransportService&) noexcept;
    TP_NOCOPYMOVE(LootService);
    bool Intercept(TESObjectREFR*, TESBoundObject*, int32_t, ITEM_REMOVE_REASON, ExtraDataList*, TESObjectREFR*) noexcept;
    bool AcceptSnapshot(uint32_t aEntity, uint32_t aEpoch, const Inventory&) noexcept;
    bool AcceptDelta(uint32_t aEntity, uint32_t aEpoch, uint64_t aRevision, const Inventory::Entry&) noexcept;
    uint64_t Revision(uint32_t aEntity) const noexcept;
    bool Blocked() const noexcept { return m_blocked; }

private:
    enum class Stage { Bindings, Sent, Applied };
    struct Pending
    {
        uint32_t SourceForm{}, DestinationForm{};
        uint64_t Began{}, LastSend{}, NativeOperation{};
        ITEM_REMOVE_REASON Reason{};
        Coop::Transfer Request;
        Inventory BeforeSource, BeforeDestination;
        Stage State{Stage::Bindings};
    };
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnWorldLoaded(const WorldLoadedEvent&) noexcept;
    void OnPacket(const CoopActionEvent&) noexcept;
    void Apply(const Coop::InventoryState&) noexcept;
    void Apply(const Coop::TransferReply&) noexcept;
    template <class T> void Apply(const T&) noexcept {}
    void Pump() noexcept;
    void Finish(bool aQuarantine, const char* aReason) noexcept;
    void Execute(Pending, const Coop::TransferReply&) noexcept;
    bool WriteReceipt(const Pending&, uint8_t aStage) noexcept;
    bool InitializeReceipts() noexcept;
    uint32_t ServerId(uint32_t aForm) const noexcept;
    static uint64_t Now() noexcept;

    World& m_world;
    TransportService& m_transport;
    Coop::TransferParticipant m_participant;
    std::deque<Pending> m_pending;
    std::map<uint32_t, Coop::InventoryState> m_states;
    std::map<uint32_t, uint64_t> m_queried;
    uint64_t m_nonce{};
    bool m_blocked{}, m_receiptsChecked{};
    entt::scoped_connection m_update, m_packet, m_disconnected, m_loaded;
};
