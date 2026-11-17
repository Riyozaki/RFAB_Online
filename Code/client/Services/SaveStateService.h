#pragma once
#include <Coop/ClientTimeline.h>
#include <Coop/DurableLease.h>
#include <Coop/SKSEBridge.h>
#include <ThreadAffinity.h>
#include <memory>
#include <optional>

struct World;
// These callbacks read/write owned byte snapshots only. They never touch EnTT or retain actors.
struct SaveStateBridge
{
    struct Loaded
    {
        std::optional<Coop::CharacterState> State;
        bool Success{}, Corrupt{}, FromSave{};
    };
    static RFAB_ANY_THREAD bool Installed() noexcept;
    static RFAB_ANY_THREAD bool SaveAllowed() noexcept;
    static RFAB_ANY_THREAD void Registered() noexcept;
    static RFAB_ANY_THREAD void Save(Coop::SKSE::Serialization*);
    static RFAB_ANY_THREAD void Load(Coop::SKSE::Serialization*);
    static RFAB_ANY_THREAD void Revert(Coop::SKSE::Serialization*);
    static RFAB_ANY_THREAD void Message(uint32_t, bool);
    static std::optional<Loaded> TakeCompleted();
    static void Publish(const Coop::CharacterState&, bool aSaveAllowed);
    static bool ConsumeFault() noexcept;
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
    bool CanSave() const noexcept { return m_timeline.CanSave(); }
    bool Begin(Coop::EffectKind, Coop::EffectId);
    bool Applied(Coop::EffectId);
    bool Commit(Coop::EffectId);
    bool AbortBeforeNative(Coop::EffectId);
    bool StampStorage(Coop::SavedStorage);
    void Uncertain(Coop::EffectId);
    void PollFault();
    const Coop::CharacterState& State() const noexcept { return m_timeline.State(); }
private:
    bool Persist(const Coop::NativeReceipt&);
    void Publish();
    World& m_world;
    std::unique_ptr<Coop::DurableFile> m_journal;
    std::unique_ptr<Coop::DurableLease> m_lease;
    Coop::ClientTimeline m_timeline;
    bool m_loadedRecord{};
};
