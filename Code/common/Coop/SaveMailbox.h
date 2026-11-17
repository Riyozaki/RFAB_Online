#pragma once
#include <Coop/ClientTimeline.h>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace Coop
{
// SKSE callbacks exchange only owned values with the main thread. Load generations, save
// snapshots and native-effect reservations share ONE mutex: check-then-Publish must not let
// a save pin the previous cursor while a paired native effect is starting.
class SaveMailbox
{
public:
    struct Loaded
    {
        std::optional<CharacterState> State;
        bool Success{}, Corrupt{}, FromSave{};
        uint64_t Epoch{};
    };
    struct SaveData
    {
        uint64_t Epoch{}, Ticket{};
        std::shared_ptr<const std::vector<uint8_t>> Bytes;
    };
    uint64_t Epoch() const;
    uint64_t BeginLoad();                 // zero refuses an overlapping load/save request
    uint64_t NativePreLoad();             // SKSE PreLoad/Revert joins the outer load request
    void SubmitRecord(uint64_t, Loaded);  // stale decoder results are never adopted
    void CompleteLoad(uint64_t, bool aSuccess, bool aNewGame = false);
    void RejectLoad(uint64_t);            // outer false, including no SKSE PostLoad callback
    std::optional<Loaded> TakeCompleted();
    bool Publish(uint64_t, std::shared_ptr<const std::vector<uint8_t>>, bool aAllowed);
    bool SaveAllowed() const;
    bool ReadyForEpoch(uint64_t) const;
    std::optional<uint64_t> BeginNative(uint64_t aEpoch);
    bool OwnsNative(uint64_t aEpoch, uint64_t aTicket) const;
    void EndNative(uint64_t aEpoch, uint64_t aTicket);
    std::optional<uint64_t> BeginSave();
    void EndSave(uint64_t aTicket, bool aAccepted);
    bool BeginSerialization();            // SKSE SaveGame message, before native save work
    SaveData SaveSnapshot() const;
    void EndSerialization(const SaveData&, bool aSuccess);
    void Fault();
    bool ConsumeFault();
private:
    uint64_t BeginLoadLocked();
    bool SaveAllowedLocked() const;
    void FinishSaveLocked();
    void FaultLocked();
    mutable std::mutex m_mutex;
    uint64_t m_epoch{1}, m_sequence{}, m_saveTicket{}, m_saveEpoch{}, m_serialTicket{};
    uint64_t m_nativeTicket{}, m_nativeEpoch{};
    bool m_loading{}, m_finished{}, m_taken{}, m_recordSubmitted{}, m_allowed{};
    bool m_saveOpen{}, m_outerSave{}, m_saveObserved{}, m_serializing{}, m_native{};
    bool m_fault{}, m_faultSignal{};
    Loaded m_record;
    std::optional<Loaded> m_completed;
    std::shared_ptr<const std::vector<uint8_t>> m_snapshot, m_saveSnapshot;
};
}
