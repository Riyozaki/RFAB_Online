#include <Coop/ClientTimeline.h>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <type_traits>

namespace Coop
{
namespace
{
bool CharacterKey(std::string_view key)
{
    if (key.size() != 32) return false;
    for (const char c : key) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
struct Codec
{
    std::vector<uint8_t> Bytes;
    std::span<const uint8_t> Input;
    bool Reading{}, Valid{true};
    template <class T> void Number(T& value)
    {
        if (!Valid) return;
        if constexpr (std::is_same_v<T, float>)
        {
            auto bits = std::bit_cast<uint32_t>(value); Number(bits);
            if (Reading) value = std::bit_cast<float>(bits);
            Valid &= std::isfinite(value);
        }
        else
        {
            constexpr size_t size = sizeof(T);
            uint64_t bits = static_cast<uint64_t>(value);
            if (Reading)
            {
                if (Input.size() < size) { Valid = false; return; }
                bits = 0;
                for (size_t i = 0; i < size; ++i) bits |= uint64_t(Input[i]) << (8 * i);
                Input = Input.subspan(size); value = static_cast<T>(bits);
            }
            else for (size_t i = 0; i < size; ++i) Bytes.push_back(static_cast<uint8_t>(bits >> (8 * i)));
        }
    }
    void Character(std::string& key)
    {
        if (!Valid) return;
        if (Reading)
        {
            if (Input.size() < 32) { Valid = false; return; }
            key.assign(reinterpret_cast<const char*>(Input.data()), 32); Input = Input.subspan(32);
        }
        else Bytes.insert(Bytes.end(), key.begin(), key.end());
        Valid &= CharacterKey(key);
    }
    void State(CharacterState& state)
    {
        uint32_t version = 2; Number(version);
        if (version != 1 && version != 2) { Valid = false; return; }
        Character(state.Character); Number(state.Cursor);
        uint32_t count = static_cast<uint32_t>(state.Scales.size()); Number(count);
        if (!Valid || count > 8192) { Valid = false; return; }
        if (Reading) state.Scales.resize(count);
        for (auto& scale : state.Scales)
        { Number(scale.Form); Number(scale.Version); Number(scale.Life); Number(scale.Base); Number(scale.Maximum); }
        if (version == 2)
        {
            uint32_t storages = static_cast<uint32_t>(state.Storages.size()); Number(storages);
            if (!Valid || storages > 1024) { Valid = false; return; }
            if (Reading) state.Storages.resize(storages);
            for (auto& storage : state.Storages)
            {
                Number(storage.Form); Number(storage.Revision);
                uint8_t length = static_cast<uint8_t>(storage.Campaign.size()); Number(length);
                if (!Valid || !length || length > 48) { Valid = false; return; }
                if (Reading)
                {
                    if (Input.size() < length) { Valid = false; return; }
                    storage.Campaign.assign(reinterpret_cast<const char*>(Input.data()), length); Input = Input.subspan(length);
                }
                else Bytes.insert(Bytes.end(), storage.Campaign.begin(), storage.Campaign.end());
            }
        }
    }
    void Receipt(NativeReceipt& receipt)
    {
        uint32_t version = 1; Number(version);
        if (version != 1) { Valid = false; return; }
        Character(receipt.Character); Number(receipt.Id.Session); Number(receipt.Id.Generation); Number(receipt.Id.Operation);
        Number(receipt.Kind); Number(receipt.Stage); Number(receipt.Before); Number(receipt.After);
    }
};
}
bool CharacterState::Valid() const noexcept
{
    if (!CharacterKey(Character) || Scales.size() > 8192 || Storages.size() > 1024) return false;
    std::set<uint32_t> storageForms;
    for (const auto& storage : Storages)
    {
        if (!storage.Form || !storage.Revision || !storageForms.insert(storage.Form).second ||
            storage.Campaign.empty() || storage.Campaign.size() > 48) return false;
        for (auto c : storage.Campaign)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    std::set<uint32_t> forms;
    std::set<uint64_t> lives;
    for (const auto& s : Scales)
        if (!s.Form || !s.Life || s.Life >= (1ull << 60) || s.Version != 1 || !std::isfinite(s.Base) || s.Base <= 0.f ||
            !std::isfinite(s.Maximum) || s.Maximum != double(s.Base) * 2.0 ||
            !forms.insert(s.Form).second || !lives.insert(s.Life).second) return false;
    return true;
}
std::optional<std::vector<uint8_t>> CharacterState::Encode() const
{
    if (!Valid()) return {};
    auto copy = *this; Codec codec; codec.State(copy);
    if (!codec.Valid) return {};
    return std::move(codec.Bytes);
}
std::optional<CharacterState> CharacterState::Decode(std::span<const uint8_t> bytes)
{
    Codec codec; codec.Reading = true; codec.Input = bytes;
    CharacterState state; codec.State(state);
    if (!codec.Valid || !codec.Input.empty() || !state.Valid()) return {};
    return state;
}
bool NativeReceipt::Valid() const noexcept
{
    return CharacterKey(Character) && Id.Valid() && Kind >= EffectKind::Transfer && Kind <= EffectKind::Storage &&
        Stage >= EffectStage::Prepared && Stage <= EffectStage::Uncertain && Before < std::numeric_limits<uint64_t>::max() && After == Before + 1;
}
std::optional<std::vector<uint8_t>> NativeReceipt::Encode() const
{
    if (!Valid()) return {};
    auto copy = *this; Codec codec; codec.Receipt(copy);
    if (!codec.Valid) return {};
    return std::move(codec.Bytes);
}
std::optional<NativeReceipt> NativeReceipt::Decode(std::span<const uint8_t> bytes)
{
    Codec codec; codec.Reading = true; codec.Input = bytes;
    NativeReceipt receipt; codec.Receipt(receipt);
    if (!codec.Valid || !codec.Input.empty() || !receipt.Valid()) return {};
    return receipt;
}
bool ClientTimeline::Load(CharacterState state, const std::optional<NativeReceipt>& receipt)
{
    m_state = std::move(state); m_receipt = receipt; m_loaded = true;
    m_faulted = !m_state.Valid();
    if (receipt)
    {
        const auto& r = *receipt;
        const bool terminal = r.Stage == EffectStage::Committed || r.Stage == EffectStage::Rejected;
        const auto expected = r.Stage == EffectStage::Rejected ? r.Before : r.After;
        m_faulted |= !r.Valid() || !terminal || r.Character != m_state.Character || expected != m_state.Cursor;
    }
    else m_faulted |= m_state.Cursor != 0;
    return !m_faulted;
}
bool ClientTimeline::CanSave() const noexcept
{
    return Ready() && (!m_receipt || m_receipt->Stage == EffectStage::Committed || m_receipt->Stage == EffectStage::Rejected);
}
bool ClientTimeline::Publish(NativeReceipt next)
{
    if (m_faulted || !next.Valid()) return false;
    try
    {
        if (!m_persist || !m_persist(next)) { m_faulted = true; return false; }
        m_receipt = std::move(next); return true;
    }
    catch (...) { m_faulted = true; return false; }
}
bool ClientTimeline::Begin(EffectKind kind, EffectId id)
{
    if (!CanSave() || !id.Valid() || m_state.Cursor == std::numeric_limits<uint64_t>::max() ||
        (m_receipt && m_receipt->Id == id)) return false;
    return Publish({m_state.Character, id, kind, EffectStage::Prepared, m_state.Cursor, m_state.Cursor + 1});
}
bool ClientTimeline::Applied(EffectId id)
{
    if (!Ready() || !m_receipt || m_receipt->Id != id || m_receipt->Stage != EffectStage::Prepared) return false;
    auto next = *m_receipt; next.Stage = EffectStage::Applied;
    if (!Publish(next)) return false;
    m_state.Cursor = next.After; return true;
}
bool ClientTimeline::Commit(EffectId id)
{
    if (!Ready() || !m_receipt || m_receipt->Id != id) return false;
    if (m_receipt->Stage == EffectStage::Committed) return true;
    if (m_receipt->Stage != EffectStage::Applied) return false;
    auto next = *m_receipt; next.Stage = EffectStage::Committed;
    return Publish(next);
}
bool ClientTimeline::AbortBeforeNative(EffectId id)
{
    if (!Ready() || !m_receipt || m_receipt->Id != id || m_receipt->Stage != EffectStage::Prepared) return false;
    auto next = *m_receipt; next.Stage = EffectStage::Rejected;
    return Publish(next);
}
void ClientTimeline::Uncertain(EffectId id) noexcept
{
    if (m_receipt && m_receipt->Id == id && !m_faulted)
    { auto next = *m_receipt; next.Stage = EffectStage::Uncertain; Publish(next); }
    m_faulted = true; // even a failed quarantine write cannot reopen this process
}
bool ClientTimeline::StampScale(SavedScale scale)
{
    if (!Ready() || !m_receipt || m_receipt->Kind != EffectKind::Scale || m_receipt->Stage != EffectStage::Prepared) return false;
    auto next = m_state;
    bool replaced = false;
    for (const auto& old : next.Scales)
        if (old.Form == scale.Form)
        {
            // A new ownership epoch/session is not a native respawn. Until an explicit
            // coordinated reset adapter retires this life, never replace it implicitly.
            if (old != scale) return false;
            replaced = true;
            break;
        }
    if (!replaced) next.Scales.push_back(scale);
    if (!next.Valid()) return false;
    m_state = std::move(next); return true;
}
bool ClientTimeline::StampStorage(SavedStorage storage)
{
    if (!Ready() || !m_receipt || !((m_receipt->Kind == EffectKind::Storage && m_receipt->Stage == EffectStage::Prepared) ||
        (m_receipt->Kind == EffectKind::Transfer && m_receipt->Stage == EffectStage::Applied))) return false;
    auto next = m_state;
    bool replaced{};
    for (auto& old : next.Storages)
        if (old.Form == storage.Form)
        {
            if (old.Campaign != storage.Campaign || old.Revision > storage.Revision) return false;
            old = storage; replaced = true; break;
        }
    if (!replaced) next.Storages.push_back(std::move(storage));
    if (!next.Valid()) return false;
    m_state = std::move(next); return true;
}
} // namespace Coop
