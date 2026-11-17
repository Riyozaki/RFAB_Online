#include <Coop/InventoryLedger.h>
#include <Coop/DurableFile.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <type_traits>

namespace Coop
{
std::optional<std::string> NormalizePlugin(std::string_view aName)
{
    if (aName.empty() || aName.size() > 120 || aName == "." || aName == "..") return {};
    std::string name(aName);
    for (auto& c : name)
    {
        const auto u = static_cast<unsigned char>(c);
        if (u < 32 || u == 127 || c == '/' || c == '\\' || c == ':' || c == '|') return {};
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    if (!name.ends_with(".esm") && !name.ends_with(".esp") && !name.ends_with(".esl")) return {};
    if (name.front() == ' ' || name.back() == ' ') return {};
    return name;
}
std::optional<std::string> StorageKey(std::string_view aCampaign, const StableForm& aBacking)
{
    if (aCampaign.empty() || aCampaign.size() > 48 || !aBacking.Local || aBacking.Local > (aBacking.Light ? 0xFFFu : 0xFFFFFFu)) return {};
    for (auto c : aCampaign)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return {};
    const auto plugin = NormalizePlugin(aBacking.Plugin);
    if (!plugin) return {};
    return std::string(aCampaign) + "|" + *plugin + (aBacking.Light ? "|l|" : "|s|") + std::to_string(aBacking.Local);
}
bool Transferable(const Inventory::Entry& aItem) noexcept
{
    if (!aItem.BaseId || aItem.BaseId.ModId >= 0xFFFFFFFEu || aItem.Count <= 0 || aItem.IsQuestItem || aItem.IsWorn() ||
        aItem.HasUnsupportedExtra || (aItem.ExtraPresence & ~uint16_t(255)) || aItem.ExtraTextDisplayName.size() > 160 || !std::isfinite(aItem.ExtraCharge) ||
        !std::isfinite(aItem.ExtraHealth) || aItem.EnchantData.Effects.size() > 64 ||
        aItem.ExtraOwnerId.ModId >= 0xFFFFFFFEu || aItem.ExtraUniqueBaseId.ModId >= 0xFFFFFFFEu) return false;
    for (const auto& effect : aItem.EnchantData.Effects)
        if (!effect.EffectId || effect.EffectId.ModId >= 0xFFFFFFFEu || !std::isfinite(effect.Magnitude) || !std::isfinite(effect.RawCost)) return false;
    if (aItem.ExtraEnchantId.ModId == 0xFFFFFFFEu ||
        (aItem.ExtraEnchantId.ModId == 0xFFFFFFFFu && aItem.EnchantData.Effects.empty())) return false;
    return aItem.ExtraSoulLevel >= 0 && aItem.ExtraSoulLevel <= 5;
}
int64_t ExactCount(const Inventory& aItems, const Inventory::Entry& aItem) noexcept
{
    int64_t count{};
    for (const auto& item : aItems.Entries)
        if (item.CanBeMerged(aItem)) count += item.Count;
    return count;
}

bool SameInventoryItems(const Inventory& a, const Inventory& b) noexcept
{
    if (!a.IsValid() || !b.IsValid()) return false;
    for (const auto& entry : a.Entries) if (ExactCount(a, entry) != ExactCount(b, entry)) return false;
    for (const auto& entry : b.Entries) if (ExactCount(a, entry) != ExactCount(b, entry)) return false;
    return true;
}

InventoryLedger::InventoryLedger(std::string aCampaign, Commit aCommit) : m_commit(std::move(aCommit))
{
    m_state.Campaign = std::move(aCampaign);
}
const InventoryLedger::Endpoint* InventoryLedger::Find(const std::string& aKey) const noexcept
{
    const auto it = m_state.Endpoints.find(aKey);
    return it == m_state.Endpoints.end() ? nullptr : &it->second;
}
const InventoryLedger::Record* InventoryLedger::Find(uint64_t aId) const noexcept
{
    const auto it = m_state.Records.find(aId);
    return it == m_state.Records.end() ? nullptr : &it->second;
}
bool InventoryLedger::Locked(const std::string& aKey) const noexcept
{
    const auto* endpoint = Find(aKey);
    if (!endpoint || endpoint->Quarantined || m_faulted) return true;
    for (const auto& [_, record] : m_state.Records)
        if ((record.State == Status::Prepared || record.State == Status::Quarantined) &&
            (record.Action.Source == aKey || record.Action.Destination == aKey)) return true;
    return false;
}
bool InventoryLedger::ValidState(const State& aState) noexcept
{
    if (aState.Campaign.empty() || aState.Campaign.size() > 48 || !aState.NextId ||
        aState.Endpoints.size() > kMaxEndpoints || aState.Records.size() > kMaxRecords) return false;
    size_t count{};
    for (const auto& [key, endpoint] : aState.Endpoints)
    {
        if (key.empty() || key.size() > 200 || !endpoint.Revision || !endpoint.Items.IsValid()) return false;
        count += endpoint.Items.Entries.size();
        if (endpoint.Items.Entries.size() > 4096 || count > kMaxItems) return false;
        for (size_t i = 0; i < endpoint.Items.Entries.size(); ++i)
            for (size_t j = 0; j < i; ++j)
                if (endpoint.Items.Entries[i].CanBeMerged(endpoint.Items.Entries[j])) return false;
    }
    std::set<std::pair<std::string, uint64_t>> nonces;
    for (const auto& [id, record] : aState.Records)
    {
        const auto& r = record.Action;
        if (!id || record.Id != id || id >= aState.NextId || record.State > Status::Quarantined || !r.Nonce ||
            r.Character.empty() || r.Character.size() > 96 || r.Source == r.Destination || !Transferable(r.Item) ||
            !aState.Endpoints.contains(r.Source) || !aState.Endpoints.contains(r.Destination) ||
            !nonces.emplace(r.Character, r.Nonce).second) return false;
        if (record.State == Status::Prepared)
        {
            const auto& s = aState.Endpoints.at(r.Source);
            const auto& d = aState.Endpoints.at(r.Destination);
            if (s.Revision != r.SourceRevision || d.Revision != r.DestinationRevision || s.Epoch != r.SourceEpoch ||
                d.Epoch != r.DestinationEpoch || ExactCount(s.Items, r.Item) < r.Item.Count) return false;
        }
    }
    return true;
}
bool InventoryLedger::Publish(State aNext)
{
    if (m_faulted || !ValidState(aNext)) return false;
    try
    {
        if (!m_commit || !m_commit(aNext)) { m_faulted = true; return false; }
        m_state = std::move(aNext);
        return true;
    }
    catch (...) { m_faulted = true; return false; }
}
bool InventoryLedger::Register(std::string aKey, Endpoint aEndpoint)
{
    if (m_faulted || Find(aKey)) return false;
    auto next = m_state;
    next.Endpoints.emplace(std::move(aKey), std::move(aEndpoint));
    return Publish(std::move(next));
}
bool InventoryLedger::Observe(const std::string& aKey, const Inventory& aItems, uint32_t aEpoch)
{
    const auto* old = Find(aKey);
    if (!old || old->Storage || Locked(aKey) || !aItems.IsValid()) return false;
    if (SameInventoryItems(old->Items, aItems) && old->Items.CurrentMagicEquipment == aItems.CurrentMagicEquipment && old->Epoch == aEpoch) return true;
    if (old->Revision == std::numeric_limits<uint64_t>::max()) return false;
    auto next = m_state;
    auto& endpoint = next.Endpoints.at(aKey);
    endpoint.Items = aItems;
    endpoint.Epoch = aEpoch;
    ++endpoint.Revision;
    return Publish(std::move(next));
}
bool InventoryLedger::ImportStorage(const std::string& aKey, const Inventory& aItems, bool aConfirmed)
{
    const auto* old = Find(aKey);
    if (!aConfirmed || !old || !old->Storage || !old->NeedsImport || Locked(aKey) || !aItems.IsValid() ||
        old->Revision == std::numeric_limits<uint64_t>::max()) return false;
    for (const auto& item : aItems.Entries) if (!Transferable(item)) return false;
    auto next = m_state;
    auto& endpoint = next.Endpoints.at(aKey);
    endpoint.Items = aItems;
    endpoint.NeedsImport = false;
    ++endpoint.Revision;
    return Publish(std::move(next));
}
InventoryLedger::Record InventoryLedger::Prepare(const Request& aRequest, uint64_t aNow)
{
    // A persisted Prepared may still exist in the last good snapshot after a failed receipt
    // write. Never re-authorize native execution from that stale copy once durability faulted.
    if (m_faulted) return {0, 0, aRequest, Status::Quarantined};
    for (const auto& [_, record] : m_state.Records)
        if (record.Action.Character == aRequest.Character && record.Action.Nonce == aRequest.Nonce)
        {
            if (record.Action == aRequest) return record;
            return {0, 0, aRequest, Status::Conflict}; // nonce collision cannot replace a previous receipt
        }
    Record result{0, 0, aRequest, Status::Rejected};
    const auto* source = Find(aRequest.Source);
    const auto* destination = Find(aRequest.Destination);
    if (m_faulted) { result.State = Status::Quarantined; return result; }
    if (!aRequest.Nonce || aRequest.Character.empty() || aRequest.Character.size() > 96 ||
        aRequest.Source == aRequest.Destination || !source || !destination || !Transferable(aRequest.Item) ||
        source->NeedsImport || destination->NeedsImport || m_state.Records.size() >= kMaxRecords ||
        m_state.NextId == std::numeric_limits<uint64_t>::max()) return result;
    if (Locked(aRequest.Source) || Locked(aRequest.Destination) || source->Revision != aRequest.SourceRevision ||
        destination->Revision != aRequest.DestinationRevision || source->Epoch != aRequest.SourceEpoch ||
        destination->Epoch != aRequest.DestinationEpoch)
    { result.State = Status::Conflict; return result; }
    Inventory from = source->Items, to = destination->Items;
    auto removal = aRequest.Item;
    removal.Count = -removal.Count;
    if (!from.AddOrRemoveEntry(removal) || !to.AddOrRemoveEntry(aRequest.Item)) return result;
    if (source->Revision == std::numeric_limits<uint64_t>::max() || destination->Revision == std::numeric_limits<uint64_t>::max()) return result;
    auto next = m_state;
    result.Id = next.NextId++;
    result.Deadline = aNow > std::numeric_limits<uint64_t>::max() - 3000 ? std::numeric_limits<uint64_t>::max() : aNow + 3000;
    result.State = Status::Prepared;
    next.Records.emplace(result.Id, result);
    if (!Publish(std::move(next))) result.State = Status::Quarantined;
    return result;
}
InventoryLedger::Record InventoryLedger::Receipt(uint64_t aId, const std::string& aCharacter, bool aApplied, bool aUncertain)
{
    const auto* old = Find(aId);
    if (!old || old->Action.Character != aCharacter) return {0, 0, {}, Status::Rejected};
    if (m_faulted) { auto result = *old; result.State = Status::Quarantined; return result; }
    // Quarantined is terminal for the automatic path, even if a very late positive ACK arrives.
    if (old->State != Status::Prepared) return *old;
    auto next = m_state;
    auto& record = next.Records.at(aId);
    if (aUncertain) record.State = Status::Quarantined;
    else if (!aApplied) record.State = Status::Rejected;
    else
    {
        const auto& request = record.Action;
        auto& source = next.Endpoints.at(request.Source);
        auto& destination = next.Endpoints.at(request.Destination);
        auto removal = request.Item;
        removal.Count = -removal.Count;
        auto from = source.Items, to = destination.Items;
        if (source.Epoch != request.SourceEpoch || destination.Epoch != request.DestinationEpoch ||
            source.Revision != request.SourceRevision || destination.Revision != request.DestinationRevision ||
            !from.AddOrRemoveEntry(removal) || !to.AddOrRemoveEntry(request.Item))
            record.State = Status::Quarantined;
        else
        {
            source.Items = std::move(from); destination.Items = std::move(to);
            ++source.Revision; ++destination.Revision;
            record.State = Status::Committed;
        }
    }
    auto result = record;
    if (!Publish(std::move(next))) result.State = Status::Quarantined;
    return result;
}
bool InventoryLedger::Expire(uint64_t aNow)
{
    auto next = m_state;
    bool changed{};
    for (auto& [_, record] : next.Records)
        if (record.State == Status::Prepared && aNow >= record.Deadline)
        { record.State = Status::Quarantined; changed = true; }
    return !changed || Publish(std::move(next));
}
bool InventoryLedger::Restore(State aState)
{
    if (aState.Campaign != m_state.Campaign || !ValidState(aState)) { m_faulted = true; return false; }
    for (auto& [_, record] : aState.Records)
        if (record.State == Status::Prepared) record.State = Status::Quarantined;
    return Publish(std::move(aState));
}

namespace
{
struct StateCodec
{
    std::vector<uint8_t> Out;
    std::span<const uint8_t> In;
    bool Reading{}, Valid{true};
    size_t Elements{};
    InventoryLedger::NameForm Name;
    InventoryLedger::ResolveForm Resolve;

    template <class T> void Number(T& value)
    {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        if (!Valid) return;
        if constexpr (std::is_same_v<T, float>)
        {
            auto bits = std::bit_cast<uint32_t>(value);
            Number(bits);
            if (Reading) value = std::bit_cast<float>(bits);
            Valid = Valid && std::isfinite(value);
        }
        else
        {
            constexpr size_t width = std::is_same_v<T, bool> ? 1 : sizeof(T);
            uint64_t bits = static_cast<uint64_t>(value);
            if (Reading)
            {
                if (In.size() < width) { Valid = false; return; }
                bits = 0;
                for (size_t i = 0; i < width; ++i) bits |= uint64_t(In[i]) << (8 * i);
                In = In.subspan(width);
                if constexpr (std::is_same_v<T, bool>) if (bits > 1) Valid = false;
                value = static_cast<T>(bits);
            }
            else
                for (size_t i = 0; i < width; ++i) Out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
        }
    }
    void String(std::string& value, uint32_t limit = 200)
    {
        uint32_t size = static_cast<uint32_t>(value.size());
        Number(size);
        if (!Valid || size > limit || (Reading && size > In.size())) { Valid = false; return; }
        if (Reading) { value.assign(reinterpret_cast<const char*>(In.data()), size); In = In.subspan(size); }
        else Out.insert(Out.end(), value.begin(), value.end());
        if (value.find('\0') != std::string::npos) Valid = false;
    }
    uint32_t Count(size_t size, uint32_t limit)
    {
        uint32_t count = static_cast<uint32_t>(size);
        Number(count);
        Elements += count;
        if (count > limit || Elements > 262144) Valid = false;
        return Valid ? count : 0;
    }
    void Form(GameId& value, bool dynamicEnchantment = false)
    {
        uint8_t tag = !value ? 0 : (value.ModId == 0xFFFFFFFFu && dynamicEnchantment ? 2 : 1);
        Number(tag);
        if (!Valid) return;
        if (!tag) { if (Reading) value = {}; return; }
        if (tag == 2 && dynamicEnchantment) { if (Reading) value = GameId(0xFFFFFFFFu, 1); return; }
        if (tag != 1) { Valid = false; return; }
        StableForm form;
        if (!Reading)
        {
            const auto named = Name ? Name(value) : std::nullopt;
            if (!named) { Valid = false; return; }
            form = *named;
        }
        String(form.Plugin, 120); Number(form.Local); Number(form.Light);
        const auto normalized = NormalizePlugin(form.Plugin);
        if (!Valid || !normalized || !form.Local || form.Local > (form.Light ? 0xFFFu : 0xFFFFFFu)) { Valid = false; return; }
        form.Plugin = *normalized;
        if (Reading)
        {
            const auto resolved = Resolve ? Resolve(form) : std::nullopt;
            if (!resolved || !*resolved || resolved->ModId >= 0xFFFFFFFEu) { Valid = false; return; }
            value = *resolved;
        }
    }
    void Item(Inventory::Entry& value)
    {
        Form(value.BaseId); Number(value.Count); Number(value.ExtraCharge);
        Form(value.ExtraEnchantId, true); Number(value.ExtraEnchantCharge); Number(value.EnchantData.IsWeapon);
        const auto count = Count(value.EnchantData.Effects.size(), 64);
        if (Reading) value.EnchantData.Effects.resize(count);
        for (auto& effect : value.EnchantData.Effects)
        { Number(effect.Magnitude); Number(effect.Area); Number(effect.Duration); Number(effect.RawCost); Form(effect.EffectId); }
        Number(value.ExtraHealth); Form(value.ExtraPoisonId); Number(value.ExtraPoisonCount); Number(value.ExtraSoulLevel);
        Number(value.ExtraEnchantRemoveUnequip); Number(value.ExtraWorn); Number(value.ExtraWornLeft); Number(value.IsQuestItem);
        Number(value.ExtraPresence); Form(value.ExtraOwnerId); Form(value.ExtraUniqueBaseId); Number(value.ExtraUniqueId); Number(value.HasUnsupportedExtra);
        std::string name(value.ExtraTextDisplayName.data(), value.ExtraTextDisplayName.size());
        String(name, 160);
        if (Reading) value.ExtraTextDisplayName.assign(name.begin(), name.end());
    }
    void Items(Inventory& value)
    {
        const auto count = Count(value.Entries.size(), 4096);
        if (Reading) value.Entries.resize(count);
        for (auto& entry : value.Entries) Item(entry);
        Form(value.CurrentMagicEquipment.LeftHandSpell); Form(value.CurrentMagicEquipment.RightHandSpell); Form(value.CurrentMagicEquipment.Shout);
    }
    void Request(InventoryLedger::Request& value)
    {
        String(value.Character, 96); String(value.Source); String(value.Destination);
        Number(value.Nonce); Number(value.SourceRevision); Number(value.DestinationRevision);
        Number(value.SourceEpoch); Number(value.DestinationEpoch); Item(value.Item);
    }
    void State(InventoryLedger::State& value)
    {
        uint32_t schema = 2;
        Number(schema);
        if (schema != 2) { Valid = false; return; }
        String(value.Campaign, 48); Number(value.NextId);
        const auto endpoints = Count(value.Endpoints.size(), InventoryLedger::kMaxEndpoints);
        if (Reading)
            for (uint32_t i = 0; i < endpoints && Valid; ++i)
            {
                std::string key; InventoryLedger::Endpoint e;
                String(key); Items(e.Items); Number(e.Revision); Number(e.Epoch);
                Number(e.Storage); Number(e.NeedsImport); Number(e.Quarantined);
                if (!value.Endpoints.emplace(std::move(key), std::move(e)).second) Valid = false;
            }
        else
            for (auto& [key, e] : value.Endpoints)
            {
                auto copy = key; String(copy); Items(e.Items); Number(e.Revision); Number(e.Epoch);
                Number(e.Storage); Number(e.NeedsImport); Number(e.Quarantined);
            }
        const auto records = Count(value.Records.size(), InventoryLedger::kMaxRecords);
        if (Reading)
            for (uint32_t i = 0; i < records && Valid; ++i)
            {
                InventoryLedger::Record r;
                Number(r.Id); Number(r.Deadline); Number(r.State); Request(r.Action);
                if (!value.Records.emplace(r.Id, std::move(r)).second) Valid = false;
            }
        else
            for (auto& [_, r] : value.Records) { Number(r.Id); Number(r.Deadline); Number(r.State); Request(r.Action); }
    }
};
} // namespace
std::optional<std::vector<uint8_t>> InventoryLedger::Encode(const State& aState, const NameForm& aName)
{
    if (!ValidState(aState)) return {};
    auto copy = aState;
    StateCodec codec;
    codec.Name = aName;
    codec.State(copy);
    if (!codec.Valid || codec.Out.size() > DurableFile::kMaxBytes) return {};
    return std::move(codec.Out);
}
std::optional<InventoryLedger::State> InventoryLedger::Decode(std::span<const uint8_t> aBytes, const ResolveForm& aResolve)
{
    if (aBytes.size() > DurableFile::kMaxBytes) return {};
    State state;
    StateCodec codec;
    codec.In = aBytes; codec.Reading = true; codec.Resolve = aResolve;
    codec.State(state);
    if (!codec.Valid || !codec.In.empty() || !ValidState(state)) return {};
    return state;
}
TransferParticipant::Decision TransferParticipant::Prepared(uint64_t aOperation)
{
    if (!aOperation) return Decision::Ignore;
    if (const auto it = m_receipts.find(aOperation); it != m_receipts.end())
    {
        switch (it->second)
        {
        case Stage::Applied: return Decision::ResendApplied;
        case Stage::NotApplied: return Decision::ResendRejected;
        case Stage::Executing: case Stage::Uncertain: return Decision::Quarantine;
        case Stage::Committed: return Decision::Ignore;
        }
    }
    if (m_receipts.size() >= InventoryLedger::kMaxRecords) return Decision::Quarantine;
    m_receipts.emplace(aOperation, Stage::Executing);
    return Decision::Execute;
}
void TransferParticipant::Applied(uint64_t aOperation, bool aApplied, bool aUncertain)
{
    const auto it = m_receipts.find(aOperation);
    if (it == m_receipts.end() || it->second != Stage::Executing) return;
    it->second = aUncertain ? Stage::Uncertain : aApplied ? Stage::Applied : Stage::NotApplied;
}
void TransferParticipant::Committed(uint64_t aOperation)
{
    const auto it = m_receipts.find(aOperation);
    if (it != m_receipts.end() && it->second == Stage::Applied) it->second = Stage::Committed;
}
} // namespace Coop
