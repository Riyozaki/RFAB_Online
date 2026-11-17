#pragma once
#include <Coop/ClientTimeline.h>
#include <Coop/SaveMailbox.h>
#include <Coop/DurableLease.h>
#include <Coop/SKSEBridge.h>
#include <ThreadAffinity.h>
#include <memory>
#include <optional>
#include <utility>

struct World;
// These callbacks read/write owned byte snapshots only. They never touch EnTT or retain actors.
struct SaveStateBridge
{
    using Loaded = Coop::SaveMailbox::Loaded;
    static RFAB_ANY_THREAD bool Installed() noexcept;
    static RFAB_ANY_THREAD bool SaveAllowed() noexcept;
    static RFAB_ANY_THREAD bool ReadyForEpoch(uint64_t) noexcept;
    static RFAB_ANY_THREAD uint64_t Epoch() noexcept;
    static RFAB_ANY_THREAD uint64_t BeginLoadRequest() noexcept;
    static RFAB_ANY_THREAD void RejectLoadRequest(uint64_t) noexcept;
    static RFAB_ANY_THREAD std::optional<uint64_t> BeginSave() noexcept;
    static RFAB_ANY_THREAD void EndSave(uint64_t, bool) noexcept;
    static RFAB_ANY_THREAD std::optional<uint64_t> BeginNative(uint64_t) noexcept;
    static RFAB_ANY_THREAD bool OwnsNative(uint64_t, uint64_t) noexcept;
    static RFAB_ANY_THREAD void EndNative(uint64_t, uint64_t) noexcept;
    static RFAB_ANY_THREAD void Registered() noexcept;
    static RFAB_ANY_THREAD void Save(Coop::SKSE::Serialization*);
    static RFAB_ANY_THREAD void Load(Coop::SKSE::Serialization*);
    static RFAB_ANY_THREAD void Revert(Coop::SKSE::Serialization*);
    static RFAB_ANY_THREAD void Message(uint32_t, bool);
    static std::optional<Loaded> TakeCompleted();
    static bool Publish(const Coop::CharacterState&, bool aSaveAllowed, uint64_t aEpoch);
    static bool ConsumeFault() noexcept;
};
// Short, non-journaled application of an ALREADY stamped life. It still excludes a save/load
// admission and cannot release a new generation's reservation after native reentry.
class SaveNativePin
{
public:
    SaveNativePin() = default;
    SaveNativePin(uint64_t epoch, uint64_t ticket) : m_epoch(epoch), m_ticket(ticket) {}
    SaveNativePin(const SaveNativePin&) = delete;
    SaveNativePin& operator=(const SaveNativePin&) = delete;
    SaveNativePin(SaveNativePin&& other) noexcept : m_epoch(other.m_epoch), m_ticket(std::exchange(other.m_ticket, 0)) {}
    ~SaveNativePin() { SaveStateBridge::EndNative(m_epoch, m_ticket); }
    explicit operator bool() const noexcept { return SaveStateBridge::OwnsNative(m_epoch, m_ticket); }
private:
    uint64_t m_epoch{}, m_ticket{};
};
struct SaveStateService
{
    explicit SaveStateService(World&) noexcept;
    TP_NOCOPYMOVE(SaveStateService);
    // Called explicitly BEFORE WorldLoaded is dispatched, independent of sink connection order.
    void Adopt(SaveStateBridge::Loaded);
    void AdoptWithoutBridge();
    const std::string& Character() const noexcept { return m_timeline.State().Character; }
    bool Ready() const noexcept;
    bool CanSave() const noexcept { return Ready() && m_timeline.CanSave() && SaveStateBridge::SaveAllowed(); }
    SaveNativePin PinNative() noexcept;
    bool NativeAdmitted(Coop::EffectId) const noexcept;
    bool Begin(Coop::EffectKind, Coop::EffectId);
    bool Applied(Coop::EffectId);
    bool Commit(Coop::EffectId);
    bool AbortBeforeNative(Coop::EffectId);
    bool StampStorage(Coop::SavedStorage);
    bool StampScale(Coop::SavedScale);
    bool StampReward(Coop::SavedRewards);
    void Quarantine();
    void Uncertain(Coop::EffectId);
    void Uncertain(Coop::EffectId, uint64_t aExpectedEpoch);
    void PollFault();
    const Coop::CharacterState& State() const noexcept { return m_timeline.State(); }
private:
    bool Persist(const Coop::NativeReceipt&);
    bool Publish();
    bool OwnsNative() const noexcept;
    void ReleaseNative() noexcept;
    World& m_world;
    std::unique_ptr<Coop::DurableFile> m_journal;
    std::unique_ptr<Coop::DurableLease> m_lease;
    Coop::ClientTimeline m_timeline;
    uint64_t m_publicationEpoch{}, m_nativeTicket{};
    bool m_loadedRecord{};
};
