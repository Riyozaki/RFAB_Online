#include <TiltedOnlinePCH.h>
#include <Services/SaveStateService.h>
#include <World.h>
#include <atomic>
#include <random>

namespace
{
std::mutex s_mutex;
std::optional<SaveStateBridge::Loaded> s_loading;
bool s_complete{}, s_seenRecord{};
std::shared_ptr<const std::vector<uint8_t>> s_snapshot;
std::atomic_bool s_installed{}, s_saveAllowed{true}, s_fault{};
std::string CharacterKey()
{
    std::random_device random;
    return fmt::format("{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
}
}
bool SaveStateBridge::Installed() noexcept { return s_installed.load(std::memory_order_acquire); }
bool SaveStateBridge::SaveAllowed() noexcept { return s_saveAllowed.load(std::memory_order_acquire); }
void SaveStateBridge::Registered() noexcept { s_installed.store(true, std::memory_order_release); }
void SaveStateBridge::Save(Coop::SKSE::Serialization* api)
{
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    { std::lock_guard lock(s_mutex); bytes = s_snapshot; }
    if (!bytes || !SaveAllowed() || !api ||
        !api->WriteRecord(Coop::SKSE::kRecord, 1, bytes->data(), static_cast<uint32_t>(bytes->size())))
    { s_fault.store(true, std::memory_order_release); s_saveAllowed.store(false, std::memory_order_release); }
}
void SaveStateBridge::Load(Coop::SKSE::Serialization* api)
{
    Loaded loaded;
    loaded.Success = true;
    uint32_t type{}, version{}, length{};
    bool found{};
    if (!api) loaded.Corrupt = true;
    while (api && api->GetNextRecordInfo(&type, &version, &length))
    {
        if (type != Coop::SKSE::kRecord) continue; // SKSE skips the remainder of unknown records
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
            scale.Form = form; // official SKSE old-load-order -> current-load-order resolution
        }
        if (!loaded.State->Valid()) loaded.Corrupt = true;
    }
    loaded.FromSave = found && !loaded.Corrupt;
    std::lock_guard lock(s_mutex);
    s_seenRecord = found;
    s_loading = std::move(loaded);
}
void SaveStateBridge::Revert(Coop::SKSE::Serialization*)
{
    std::lock_guard lock(s_mutex);
    s_loading = Loaded{}; s_complete = false; s_seenRecord = false;
    s_saveAllowed.store(false, std::memory_order_release);
}
void SaveStateBridge::Message(uint32_t type, bool success)
{
    if (type == 2) // PreLoadGame: early atomic fence even for a path not reaching our outer detour
    {
        {
            std::lock_guard lock(s_mutex);
            s_loading = Loaded{}; s_complete = false; s_seenRecord = false;
            s_saveAllowed.store(false, std::memory_order_release);
        }
        World::SignalWorldLoad(); // only atomics + queue invalidation, not engine/ECS teardown
    }
    else if (type == 3 || type == 7) // confirmed PostLoadGame / NewGame
    {
        {
            std::lock_guard lock(s_mutex);
            if (!s_loading || type == 7) s_loading = Loaded{};
            s_loading->Success = type == 7 || success;
            s_loading->FromSave = type != 7 && s_seenRecord && !s_loading->Corrupt;
            s_complete = true;
        }
        if (type == 7) World::SignalWorldLoad();
    }
}
std::optional<SaveStateBridge::Loaded> SaveStateBridge::TakeCompleted()
{
    std::lock_guard lock(s_mutex);
    if (!s_complete) return {};
    s_complete = false;
    auto result = std::move(s_loading); s_loading.reset();
    return result;
}
void SaveStateBridge::Publish(const Coop::CharacterState& state, bool allowed)
{
    const auto bytes = state.Encode();
    auto snapshot = bytes ? std::make_shared<const std::vector<uint8_t>>(*bytes) : nullptr;
    { std::lock_guard lock(s_mutex); s_snapshot = std::move(snapshot); }
    s_saveAllowed.store(allowed && bytes.has_value(), std::memory_order_release);
}
bool SaveStateBridge::ConsumeFault() noexcept { return s_fault.exchange(false, std::memory_order_acq_rel); }

SaveStateService::SaveStateService(World& world) noexcept
    : m_world(world), m_timeline([this](const auto& receipt) { return Persist(receipt); }) {}
bool SaveStateService::Persist(const Coop::NativeReceipt& receipt)
{
    const auto bytes = receipt.Encode();
    return bytes && m_journal && m_journal->Write(*bytes);
}
void SaveStateService::Publish() { SaveStateBridge::Publish(m_timeline.State(), m_timeline.CanSave()); }
bool SaveStateService::Ready() const noexcept
{
    return SaveStateBridge::Installed() && m_loadedRecord && m_timeline.Ready();
}
void SaveStateService::Adopt(SaveStateBridge::Loaded loaded)
{
    if (!RFAB_EXPECT_MAIN_THREAD()) return;
    m_loadedRecord = loaded.Success && loaded.FromSave && !loaded.Corrupt;
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
    Adopt({std::nullopt, true, false, false});
}
bool SaveStateService::Begin(Coop::EffectKind kind, Coop::EffectId id)
{
    if (!Ready()) return false;
    const bool result = m_timeline.Begin(kind, id); Publish(); return result;
}
bool SaveStateService::Applied(Coop::EffectId id)
{
    const bool result = m_timeline.Applied(id); Publish(); return result;
}
bool SaveStateService::Commit(Coop::EffectId id)
{
    const bool result = m_timeline.Commit(id); Publish(); return result;
}
bool SaveStateService::AbortBeforeNative(Coop::EffectId id)
{
    const bool result = m_timeline.AbortBeforeNative(id); Publish(); return result;
}
void SaveStateService::Uncertain(Coop::EffectId id) { m_timeline.Uncertain(id); Publish(); }
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
