#include <TiltedOnlinePCH.h>
#include <Services/SaveStateService.h>
#include <World.h>
#include <atomic>
#include <random>

namespace
{
Coop::SaveMailbox s_mailbox;
std::atomic_bool s_installed{};
thread_local uint64_t s_loadCallbackEpoch{};
std::string CharacterKey()
{
    std::random_device random;
    return fmt::format("{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
}
}
bool SaveStateBridge::Installed() noexcept { return s_installed.load(std::memory_order_acquire); }
bool SaveStateBridge::SaveAllowed() noexcept { return s_mailbox.SaveAllowed(); }
bool SaveStateBridge::ReadyForEpoch(uint64_t epoch) noexcept { return s_mailbox.ReadyForEpoch(epoch); }
uint64_t SaveStateBridge::Epoch() noexcept { return s_mailbox.Epoch(); }
uint64_t SaveStateBridge::BeginLoadRequest() noexcept { return s_mailbox.BeginLoad(); }
void SaveStateBridge::RejectLoadRequest(uint64_t epoch) noexcept { s_mailbox.RejectLoad(epoch); }
std::optional<uint64_t> SaveStateBridge::BeginSave() noexcept { return s_mailbox.BeginSave(); }
void SaveStateBridge::EndSave(uint64_t ticket, bool accepted) noexcept { s_mailbox.EndSave(ticket, accepted); }
std::optional<uint64_t> SaveStateBridge::BeginNative(uint64_t epoch) noexcept { return s_mailbox.BeginNative(epoch); }
bool SaveStateBridge::OwnsNative(uint64_t epoch, uint64_t ticket) noexcept { return s_mailbox.OwnsNative(epoch, ticket); }
void SaveStateBridge::EndNative(uint64_t epoch, uint64_t ticket) noexcept { s_mailbox.EndNative(epoch, ticket); }
void SaveStateBridge::Registered() noexcept { s_installed.store(true, std::memory_order_release); }
void SaveStateBridge::Save(Coop::SKSE::Serialization* api)
{
    const auto save = s_mailbox.SaveSnapshot();
    if (!save.Bytes) { s_mailbox.Fault(); return; }
    const bool result = api && api->WriteRecord(Coop::SKSE::kRecord, 1, save.Bytes->data(), static_cast<uint32_t>(save.Bytes->size()));
    s_mailbox.EndSerialization(save, result);
}
void SaveStateBridge::Load(Coop::SKSE::Serialization* api)
{
    const auto epoch = s_mailbox.Epoch();
    Loaded loaded;
    loaded.Success = true;
    uint32_t type{}, version{}, length{};
    bool found{};
    if (!api) loaded.Corrupt = true;
    while (api && api->GetNextRecordInfo(&type, &version, &length))
    {
        if (type != Coop::SKSE::kRecord) continue;
        if (found || version != 1 || length > 32u * 1024 * 1024 || length < 48)
        { loaded.Corrupt = true; break; }
        found = true;
        std::vector<uint8_t> bytes(length);
        if (api->ReadRecordData(bytes.data(), length) != length) { loaded.Corrupt = true; break; }
        loaded.State = Coop::CharacterState::Decode(bytes);
        if (!loaded.State) { loaded.Corrupt = true; break; }
        for (auto& scale : loaded.State->Scales)
        {
            uint32_t form{};
            if (!api->ResolveFormId(scale.Form, &form) || !form) { loaded.Corrupt = true; break; }
            scale.Form = form;
        }
        for (auto& storage : loaded.State->Storages)
        {
            uint32_t form{};
            if (!api->ResolveFormId(storage.Form, &form) || !form) { loaded.Corrupt = true; break; }
            storage.Form = form;
        }
        if (!loaded.State->Valid()) loaded.Corrupt = true;
    }
    loaded.FromSave = found && !loaded.Corrupt;
    s_mailbox.SubmitRecord(epoch, std::move(loaded));
}
void SaveStateBridge::Revert(Coop::SKSE::Serialization*)
{
    s_mailbox.NativePreLoad();
    World::SignalWorldLoad();
}
void SaveStateBridge::Message(uint32_t type, bool success)
{
    if (type == 2)
    {
        s_loadCallbackEpoch = s_mailbox.NativePreLoad();
        World::SignalWorldLoad(); // only atomics + queue invalidation; never engine/ECS teardown
    }
    else if (type == 3 || type == 7)
    {
        if (type == 7) World::SignalWorldLoad();
        s_mailbox.CompleteLoad(s_loadCallbackEpoch, type == 7 || success, type == 7);
        s_loadCallbackEpoch = 0;
    }
    else if (type == 4) s_mailbox.BeginSerialization(); // SaveGame, before native ESS work
}
std::optional<SaveStateBridge::Loaded> SaveStateBridge::TakeCompleted() { return s_mailbox.TakeCompleted(); }
bool SaveStateBridge::Publish(const Coop::CharacterState& state, bool allowed, uint64_t epoch)
{
    const auto bytes = state.Encode();
    auto snapshot = bytes ? std::make_shared<const std::vector<uint8_t>>(*bytes) : nullptr;
    return s_mailbox.Publish(epoch, std::move(snapshot), allowed && bytes.has_value());
}
bool SaveStateBridge::ConsumeFault() noexcept { return s_mailbox.ConsumeFault(); }

SaveStateService::SaveStateService(World& world) noexcept
    : m_world(world), m_timeline([this](const auto& receipt) { return Persist(receipt); }) {}
bool SaveStateService::Persist(const Coop::NativeReceipt& receipt)
{
    const auto bytes = receipt.Encode();
    return bytes && m_journal && m_journal->Write(*bytes);
}
bool SaveStateService::Publish() { return SaveStateBridge::Publish(m_timeline.State(), m_timeline.CanSave(), m_publicationEpoch); }
bool SaveStateService::OwnsNative() const noexcept { return SaveStateBridge::OwnsNative(m_publicationEpoch, m_nativeTicket); }
void SaveStateService::ReleaseNative() noexcept
{
    SaveStateBridge::EndNative(m_publicationEpoch, m_nativeTicket);
    m_nativeTicket = 0;
}
bool SaveStateService::Ready() const noexcept
{
    return SaveStateBridge::Installed() && m_loadedRecord && m_timeline.Ready() && SaveStateBridge::ReadyForEpoch(m_publicationEpoch);
}
void SaveStateService::Adopt(SaveStateBridge::Loaded loaded)
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !loaded.Epoch || loaded.Epoch != SaveStateBridge::Epoch()) return;
    m_publicationEpoch = loaded.Epoch;
    m_nativeTicket = 0;
    m_loadedRecord = loaded.Success && loaded.FromSave && !loaded.Corrupt;
    if (!loaded.Success || loaded.Corrupt || (loaded.FromSave && !loaded.State) || (loaded.State && !loaded.State->Valid()))
    {
        // A rejected/failed load is not a new character. Retain the old lease/journal and
        // fault it; never fabricate a fresh identity or enable saves from an old snapshot.
        m_loadedRecord = false; m_timeline.Fault(); Publish();
        Utils::ShowHudMessage("RFAB: load failed or co-save is corrupt. Co-op work remains quarantined; retry a matching save. No old actor jobs or new empty character were resumed.");
        return;
    }
    auto state = loaded.State ? std::move(*loaded.State) : Coop::CharacterState{CharacterKey(), 0, {}};
    m_lease.reset();
    m_lease = std::make_unique<Coop::DurableLease>(std::filesystem::path("Data/SKSE/Plugins/RFAB_Online/characters") / (state.Character + ".lock"));
    m_journal = std::make_unique<Coop::DurableFile>(std::filesystem::path("Data/SKSE/Plugins/RFAB_Online/characters") / (state.Character + ".receipt"));
    std::vector<uint8_t> bytes;
    const auto read = m_journal->Read(bytes);
    auto receipt = read == Coop::DurableFile::ReadResult::Loaded ? Coop::NativeReceipt::Decode(bytes) : std::nullopt;
    const bool valid = m_timeline.Load(std::move(state), receipt);
    if (!m_lease->Acquired() || !loaded.Success || loaded.Corrupt || read == Coop::DurableFile::ReadResult::Failed ||
        (read == Coop::DurableFile::ReadResult::Loaded && !receipt)) m_timeline.Fault();
    SaveStateBridge::ConsumeFault();
    Publish();
    if (!valid || !m_timeline.Ready())
        Utils::ShowHudMessage("RFAB: co-save/native receipt mismatch, corrupt record or failed load. Shared side effects locked; no automatic replay/empty bootstrap. Load a matching coordinated save.");
    else if (!m_loadedRecord)
        Utils::ShowHudMessage("RFAB: create a personal co-op save offline, then load it once to bind this character's SKSE co-save. Shared transfers remain disabled until then.");
}
void SaveStateService::AdoptWithoutBridge()
{
    const auto epoch = s_mailbox.NativePreLoad();
    s_mailbox.CompleteLoad(epoch, true);
    if (auto loaded = s_mailbox.TakeCompleted()) Adopt(std::move(*loaded));
}
bool SaveStateService::Begin(Coop::EffectKind kind, Coop::EffectId id)
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !Ready() || !m_timeline.CanSave() || m_nativeTicket) return false;
    const auto ticket = SaveStateBridge::BeginNative(m_publicationEpoch);
    if (!ticket) return false;
    m_nativeTicket = *ticket;
    const bool result = m_timeline.Begin(kind, id);
    const bool published = Publish();
    if (result && published && OwnsNative()) return true;
    // No native call was permitted. A failed journal/publication remains fail-closed; a later
    // load cannot use this old ticket to release a new character's reservation.
    if (!published) m_timeline.Fault();
    ReleaseNative();
    return false;
}
bool SaveStateService::Applied(Coop::EffectId id)
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !OwnsNative()) return false;
    const bool result = m_timeline.Applied(id);
    return Publish() && result;
}
bool SaveStateService::Commit(Coop::EffectId id)
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !Ready()) return false;
    const auto& receipt = m_timeline.Receipt();
    if (receipt && receipt->Id == id && receipt->Stage == Coop::EffectStage::Committed) return true;
    if (!OwnsNative()) return false;
    const bool result = m_timeline.Commit(id);
    const bool published = Publish();
    if (result && published) ReleaseNative(); // publish the final cursor BEFORE reopening saves
    return result && published;
}
bool SaveStateService::AbortBeforeNative(Coop::EffectId id)
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !OwnsNative()) return false;
    const bool result = m_timeline.AbortBeforeNative(id);
    const bool published = Publish();
    if (result && published) ReleaseNative();
    return result && published;
}
bool SaveStateService::StampStorage(Coop::SavedStorage storage)
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !OwnsNative()) return false;
    const bool result = m_timeline.StampStorage(std::move(storage));
    return Publish() && result;
}
bool SaveStateService::StampScale(Coop::SavedScale scale)
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !OwnsNative()) return false;
    const bool result = m_timeline.StampScale(scale);
    return Publish() && result;
}
void SaveStateService::Quarantine()
{
    if (!RFAB_EXPECT_MAIN_THREAD()) return;
    m_timeline.Fault();
    Publish();
}
void SaveStateService::Uncertain(Coop::EffectId id)
{
    if (!RFAB_EXPECT_MAIN_THREAD()) return;
    const auto& receipt = m_timeline.Receipt();
    if (!receipt || receipt->Id != id || receipt->Stage == Coop::EffectStage::Committed || receipt->Stage == Coop::EffectStage::Rejected) return;
    m_timeline.Uncertain(id);
    Publish(); // stale generation cannot publish, but its own old journal remains quarantined
}
void SaveStateService::PollFault()
{
    if (SaveStateBridge::ConsumeFault())
    {
        m_timeline.Fault(); Publish();
        Utils::ShowHudMessage("RFAB: SKSE co-save serialization failed. Shared side effects locked; do not remove the receipt file or retry old operations.");
    }
}
extern "C" __declspec(dllexport) const Coop::SKSE::Host* RFAB_GetCoopBridge(uint32_t version)
{
    static const Coop::SKSE::Host host{Coop::SKSE::kBridgeVersion, sizeof(Coop::SKSE::Host), SaveStateBridge::Registered,
        SaveStateBridge::Save, SaveStateBridge::Load, SaveStateBridge::Revert, SaveStateBridge::Message};
    return version == Coop::SKSE::kBridgeVersion ? &host : nullptr;
}
