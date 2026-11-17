#include <Coop/SaveMailbox.h>
#include <limits>

namespace Coop
{
uint64_t SaveMailbox::Epoch() const { std::lock_guard lock(m_mutex); return m_epoch; }
void SaveMailbox::FaultLocked() { m_fault = m_faultSignal = true; m_allowed = false; }
void SaveMailbox::Fault() { std::lock_guard lock(m_mutex); FaultLocked(); }
bool SaveMailbox::ConsumeFault()
{
    std::lock_guard lock(m_mutex);
    const bool value = m_faultSignal;
    m_faultSignal = false;
    return value;
}
uint64_t SaveMailbox::BeginLoadLocked()
{
    if (m_epoch == std::numeric_limits<uint64_t>::max()) { FaultLocked(); return 0; }
    ++m_epoch;
    m_loading = true;
    m_finished = m_taken = m_recordSubmitted = m_allowed = false;
    m_record = {};
    m_completed.reset();
    m_snapshot.reset();
    m_saveSnapshot.reset();
    // A main-thread load invalidates pending effects after disconnect/queue invalidation. No
    // old completion or serializer may subsequently publish into this new load's generation.
    m_saveOpen = m_outerSave = m_saveObserved = m_serializing = m_native = false;
    m_fault = m_faultSignal = false;
    return m_epoch;
}
uint64_t SaveMailbox::BeginLoad()
{
    std::lock_guard lock(m_mutex);
    if ((m_loading && !m_taken) || m_saveOpen) return 0;
    return BeginLoadLocked();
}
uint64_t SaveMailbox::NativePreLoad()
{
    std::lock_guard lock(m_mutex);
    if (m_loading && !m_finished) return m_epoch;
    const bool overlap = m_saveOpen || (m_loading && !m_taken);
    const auto epoch = BeginLoadLocked();
    if (overlap) FaultLocked(); // bypassed outer gate: never certify an overlapping native load
    return epoch;
}
void SaveMailbox::SubmitRecord(uint64_t epoch, Loaded loaded)
{
    std::lock_guard lock(m_mutex);
    if (!epoch || epoch != m_epoch || !m_loading || m_finished) return;
    if (m_recordSubmitted) { m_record.Corrupt = true; return; }
    m_recordSubmitted = true;
    loaded.Epoch = epoch;
    m_record = std::move(loaded);
}
void SaveMailbox::CompleteLoad(uint64_t epoch, bool success, bool newGame)
{
    std::lock_guard lock(m_mutex);
    if (newGame)
    {
        if (!m_loading || m_finished)
        {
            const bool overlap = m_saveOpen || (m_loading && !m_taken);
            epoch = BeginLoadLocked();
            if (overlap) FaultLocked();
        }
        else epoch = m_epoch;
    }
    if (!epoch || epoch != m_epoch || !m_loading || m_finished) return;
    Loaded loaded = newGame ? Loaded{} : std::move(m_record);
    loaded.Corrupt |= m_fault || (loaded.FromSave && !loaded.State) || (loaded.State && !loaded.State->Valid());
    loaded.Success = success;
    loaded.FromSave = success && !newGame && loaded.FromSave && !loaded.Corrupt;
    loaded.Epoch = m_epoch;
    if (!success || loaded.Corrupt) FaultLocked();
    m_completed = std::move(loaded);
    m_finished = true;
}
void SaveMailbox::RejectLoad(uint64_t epoch)
{
    std::lock_guard lock(m_mutex);
    if (!epoch || epoch != m_epoch || !m_loading || m_taken) return;
    // Even a contradictory inner success followed by outer failure is not readiness proof.
    m_completed = Loaded{std::nullopt, false, false, false, epoch};
    m_finished = true;
    FaultLocked();
}
std::optional<SaveMailbox::Loaded> SaveMailbox::TakeCompleted()
{
    std::lock_guard lock(m_mutex);
    if (!m_completed) return {};
    m_taken = true;
    auto completed = std::move(m_completed);
    m_completed.reset();
    return completed; // fenced until the MAIN THREAD publishes its receipt/character adoption
}
bool SaveMailbox::Publish(uint64_t epoch, std::shared_ptr<const std::vector<uint8_t>> bytes, bool allowed)
{
    std::lock_guard lock(m_mutex);
    if (!epoch || epoch != m_epoch || (m_loading && !m_taken)) return false;
    if (m_saveOpen)
    {
        // Quarantine/revocation cannot replace the PINNED bytes of an in-flight save, but it
        // must still close FUTURE saves. Otherwise EndSave could reopen an old allowed flag
        // after a native/receipt failure that occurred while that pin was held.
        if (!allowed) m_allowed = false;
        return false;
    }
    m_loading = false;
    m_snapshot = std::move(bytes);
    if (allowed && (!m_snapshot || m_snapshot->empty())) FaultLocked();
    m_allowed = allowed && m_snapshot && !m_snapshot->empty() && !m_fault;
    return true;
}
bool SaveMailbox::SaveAllowedLocked() const
{
    return m_allowed && m_snapshot && !m_loading && !m_saveOpen && !m_native && !m_fault;
}
bool SaveMailbox::SaveAllowed() const { std::lock_guard lock(m_mutex); return SaveAllowedLocked(); }
bool SaveMailbox::ReadyForEpoch(uint64_t epoch) const
{
    std::lock_guard lock(m_mutex);
    return epoch && epoch == m_epoch && m_snapshot && !m_snapshot->empty() && !m_loading && !m_fault;
}
std::optional<uint64_t> SaveMailbox::BeginNative(uint64_t epoch)
{
    std::lock_guard lock(m_mutex);
    if (!epoch || epoch != m_epoch || !SaveAllowedLocked() || m_sequence == std::numeric_limits<uint64_t>::max()) return {};
    m_nativeEpoch = epoch;
    m_nativeTicket = ++m_sequence;
    m_native = true;
    return m_nativeTicket;
}
bool SaveMailbox::OwnsNative(uint64_t epoch, uint64_t ticket) const
{
    std::lock_guard lock(m_mutex);
    return m_native && epoch == m_epoch && epoch == m_nativeEpoch && ticket && ticket == m_nativeTicket && !m_fault;
}
void SaveMailbox::EndNative(uint64_t epoch, uint64_t ticket)
{
    std::lock_guard lock(m_mutex);
    if (epoch == m_epoch && epoch == m_nativeEpoch && ticket && ticket == m_nativeTicket) m_native = false;
}
std::optional<uint64_t> SaveMailbox::BeginSave()
{
    std::lock_guard lock(m_mutex);
    if (!SaveAllowedLocked() || m_sequence == std::numeric_limits<uint64_t>::max()) return {};
    m_saveTicket = ++m_sequence;
    m_saveEpoch = m_epoch;
    m_saveOpen = m_outerSave = true;
    m_saveObserved = false;
    m_saveSnapshot = m_snapshot;
    return m_saveTicket;
}
void SaveMailbox::FinishSaveLocked()
{
    m_saveOpen = false;
    m_saveSnapshot.reset();
}
void SaveMailbox::EndSave(uint64_t ticket, bool accepted)
{
    std::lock_guard lock(m_mutex);
    if (!ticket || ticket != m_saveTicket || m_saveEpoch != m_epoch || !m_saveOpen) return;
    m_outerSave = false;
    // An accepted request may queue its serialization later. Do NOT reopen native work merely
    // because the outer bool returned: keep the immutable snapshot pinned until its callback.
    if ((!accepted && !m_saveObserved) || (m_saveObserved && !m_serializing)) FinishSaveLocked();
    if (!accepted && m_saveObserved) FaultLocked();
}
bool SaveMailbox::BeginSerialization()
{
    std::lock_guard lock(m_mutex);
    // A SaveGame message cannot cancel an ESS already being written. Refuse to manufacture a
    // co-save for an untracked/bypassed save path, and quarantine rather than infer success.
    if (m_loading || m_fault || !m_saveOpen || m_saveObserved || m_serializing ||
        m_sequence == std::numeric_limits<uint64_t>::max()) { FaultLocked(); return false; }
    m_serialTicket = ++m_sequence;
    m_saveObserved = m_serializing = true;
    return m_saveSnapshot != nullptr;
}
SaveMailbox::SaveData SaveMailbox::SaveSnapshot() const
{
    std::lock_guard lock(m_mutex);
    if (m_loading || m_fault || !m_serializing || m_saveEpoch != m_epoch) return {};
    return {m_saveEpoch, m_serialTicket, m_saveSnapshot};
}
void SaveMailbox::EndSerialization(const SaveData& save, bool success)
{
    std::lock_guard lock(m_mutex);
    if (!m_serializing || save.Epoch != m_epoch || save.Epoch != m_saveEpoch || save.Ticket != m_serialTicket) return;
    if (!success) FaultLocked();
    m_serializing = false;
    if (!m_outerSave) FinishSaveLocked();
}
}
