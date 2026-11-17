#include <Coop/RewardLedger.h>
#include <Coop/DurableFile.h>
#include <algorithm>
#include <limits>
#include <set>
#include <type_traits>

namespace Coop
{
namespace
{
bool Character(std::string_view value)
{
    if (value.empty() || value.size() > 96) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
bool Normalize(RewardIdentity& identity)
{
    const auto source = NormalizePlugin(identity.Source.Plugin);
    if (!source) return false;
    identity.Source.Plugin = *source;
    if (identity.Context)
    {
        const auto context = NormalizePlugin(identity.Context->Plugin);
        if (!context) return false;
        identity.Context->Plugin = *context;
    }
    return SemanticRewardKey(identity).has_value();
}
struct Codec
{
    std::vector<uint8_t> Bytes;
    std::span<const uint8_t> Input;
    bool Reading{}, Valid{true};
    template <class T> void Number(T& value)
    {
        if (!Valid) return;
        uint64_t bits = static_cast<uint64_t>(value);
        if (Reading)
        {
            if (Input.size() < sizeof(T)) { Valid = false; return; }
            bits = 0;
            for (size_t i = 0; i < sizeof(T); ++i) bits |= uint64_t(Input[i]) << (8 * i);
            Input = Input.subspan(sizeof(T)); value = static_cast<T>(bits);
            if constexpr (std::is_same_v<T, bool>) if (bits > 1) Valid = false;
        }
        else for (size_t i = 0; i < sizeof(T); ++i) Bytes.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    }
    void Text(std::string& value, uint16_t limit)
    {
        uint16_t size = static_cast<uint16_t>(value.size());
        if (!Reading && value.size() > limit) Valid = false;
        Number(size);
        if (!Valid || !size || size > limit) { Valid = false; return; }
        if (Reading)
        {
            if (Input.size() < size) { Valid = false; return; }
            value.assign(reinterpret_cast<const char*>(Input.data()), size); Input = Input.subspan(size);
        }
        else Bytes.insert(Bytes.end(), value.begin(), value.end());
    }
    void Form(StableForm& value) { Text(value.Plugin, 120); Number(value.Local); Number(value.Light); }
    void Identity(RewardIdentity& value)
    {
        Number(value.Kind); Form(value.Source);
        bool context = value.Context.has_value(); Number(context);
        if (Reading && context) value.Context.emplace();
        if (context) Form(*value.Context);
        Number(value.Incarnation); Number(value.Occurrence);
    }
    void State(RewardLedger::State& state)
    {
        uint32_t version = 1; Number(version);
        if (version != 1) { Valid = false; return; }
        Text(state.Campaign, 48); Number(state.NextId);
        uint32_t accounts = static_cast<uint32_t>(state.Accounts.size()); Number(accounts);
        if (!Valid || accounts > RewardLedger::kMaxAccounts) { Valid = false; return; }
        if (Reading)
        {
            for (uint32_t i = 0; i < accounts && Valid; ++i)
            {
                std::string character; Text(character, 96);
                RewardLedger::Account account; Number(account.Revision); Number(account.Digest); Number(account.Blocked);
                if (!state.Accounts.emplace(std::move(character), account).second) Valid = false;
            }
        }
        else for (auto& [key, account] : state.Accounts)
        {
            auto character = key; Text(character, 96); Number(account.Revision); Number(account.Digest); Number(account.Blocked);
        }
        uint32_t records = static_cast<uint32_t>(state.Records.size()); Number(records);
        if (!Valid || records > RewardLedger::kMaxRecords) { Valid = false; return; }
        auto record = [&](RewardLedger::Record& r) {
            Number(r.Id); Identity(r.Identity); Number(r.BaseXP);
            uint8_t count = static_cast<uint8_t>(r.Recipients.size()); Number(count);
            if (!Valid || !count || count > 2) { Valid = false; return; }
            auto delivery = [&](RewardLedger::Delivery& d) {
                Number(d.State); Number(d.Deadline); Number(d.BeforeRevision); Number(d.BeforeDigest); Number(d.FinalXP);
            };
            if (Reading)
                for (uint8_t i = 0; i < count && Valid; ++i)
                {
                    std::string name; Text(name, 96); RewardLedger::Delivery d; delivery(d);
                    if (!r.Recipients.emplace(std::move(name), d).second) Valid = false;
                }
            else for (auto& [key, d] : r.Recipients) { auto name = key; Text(name, 96); delivery(d); }
        };
        if (Reading)
            for (uint32_t i = 0; i < records && Valid; ++i)
            {
                RewardLedger::Record r; record(r);
                if (!state.Records.emplace(r.Id, std::move(r)).second) Valid = false;
            }
        else for (auto& [_, r] : state.Records) record(r);
    }
};
}
std::optional<std::string> SemanticRewardKey(const RewardIdentity& identity)
{
    const auto source = StorageKey("reward", identity.Source);
    const auto context = identity.Context ? StorageKey("context", *identity.Context) : std::optional<std::string>{"-"};
    if (!source || !context || identity.Kind > RewardKind::Custom || !identity.Occurrence) return {};
    if (identity.Kind == RewardKind::Kill && !identity.Incarnation) return {};
    if (identity.Kind == RewardKind::Book && (identity.Incarnation || identity.Occurrence != 1 || identity.Context)) return {};
    return std::to_string(static_cast<uint8_t>(identity.Kind)) + "|" + *source + "|" + *context + "|" +
        std::to_string(identity.Incarnation) + "|" + std::to_string(identity.Occurrence);
}
uint64_t AdvanceRewardDigest(uint64_t previous, uint64_t reward, int32_t base) noexcept
{
    uint64_t hash = previous ? previous : 14695981039346656037ull;
    for (const auto value : {reward, static_cast<uint64_t>(static_cast<uint32_t>(base))})
        for (size_t i = 0; i < 8; ++i) { hash ^= static_cast<uint8_t>(value >> (i * 8)); hash *= 1099511628211ull; }
    return hash ? hash : 1; // mismatch detector, NOT authentication or a proof of actual native XP
}
RewardLedger::RewardLedger(std::string campaign, Commit commit) : m_commit(std::move(commit)) { m_state.Campaign = std::move(campaign); }
const RewardLedger::Record* RewardLedger::Find(uint64_t id) const noexcept
{
    const auto it = m_state.Records.find(id); return it == m_state.Records.end() ? nullptr : &it->second;
}
const RewardLedger::Record* RewardLedger::Find(const RewardIdentity& identity) const
{
    const auto key = SemanticRewardKey(identity);
    if (key) for (const auto& [_, record] : m_state.Records) if (SemanticRewardKey(record.Identity) == key) return &record;
    return nullptr;
}
const RewardLedger::Account* RewardLedger::FindAccount(const std::string& character) const noexcept
{
    const auto it = m_state.Accounts.find(character); return it == m_state.Accounts.end() ? nullptr : &it->second;
}
bool RewardLedger::ValidState(const State& state)
{
    if (!StorageKey(state.Campaign, {"skyrim.esm", 1, false}) || !state.NextId || state.NextId >= (1ull << 60) ||
        state.Accounts.size() > kMaxAccounts || state.Records.size() > kMaxRecords) return false;
    std::map<std::string, Account> rebuilt;
    std::set<std::string> identities, offered;
    for (const auto& [key, a] : state.Accounts)
    {
        if (!Character(key) || bool(a.Revision) != bool(a.Digest)) return false;
        rebuilt.emplace(key, Account{});
    }
    for (const auto& [id, record] : state.Records)
    {
        const auto key = SemanticRewardKey(record.Identity);
        auto normalized = record.Identity;
        if (!id || id != record.Id || id >= state.NextId || !key || !identities.insert(*key).second || !Normalize(normalized) ||
            normalized != record.Identity || record.BaseXP <= 0 || record.BaseXP > kMaxBaseXP ||
            record.Recipients.empty() || record.Recipients.size() > 2) return false;
        for (const auto& [character, d] : record.Recipients)
        {
            if (!rebuilt.contains(character) || d.State > Stage::Uncertain || d.FinalXP < 0 || d.FinalXP > kMaxFinalXP) return false;
            auto& account = rebuilt.at(character);
            if (d.State == Stage::Committed)
            {
                if (!d.Deadline || d.BeforeRevision != account.Revision || d.BeforeDigest != account.Digest ||
                    account.Revision == std::numeric_limits<uint64_t>::max()) return false;
                ++account.Revision; account.Digest = AdvanceRewardDigest(account.Digest, id, record.BaseXP);
            }
            else if (d.State == Stage::Offered)
            {
                if (!d.Deadline || d.BeforeRevision != account.Revision || d.BeforeDigest != account.Digest ||
                    d.FinalXP || !offered.insert(character).second) return false;
            }
            else if (d.State == Stage::Pending && (d.Deadline || d.BeforeRevision || d.BeforeDigest || d.FinalXP)) return false;
            if ((d.State == Stage::Uncertain || (d.State == Stage::Rejected && d.Deadline)) && !state.Accounts.at(character).Blocked) return false;
            if (d.State == Stage::Uncertain && !d.Deadline) return false;
        }
    }
    for (const auto& [character, a] : state.Accounts)
        if (a.Revision != rebuilt.at(character).Revision || a.Digest != rebuilt.at(character).Digest) return false;
    return true;
}
bool RewardLedger::Publish(State next)
{
    if (m_faulted || !ValidState(next)) return false;
    try
    {
        if (!m_commit || !m_commit(next)) { m_faulted = true; return false; }
        m_state = std::move(next); return true;
    }
    catch (...) { m_faulted = true; return false; }
}
bool RewardLedger::Attach(const std::string& character, uint64_t revision, uint64_t digest)
{
    if (m_faulted || !Character(character)) return false;
    if (const auto* account = FindAccount(character))
    {
        if (account->Blocked || account->Revision != revision || account->Digest != digest) return false;
        for (const auto& [_, r] : m_state.Records)
            if (const auto it = r.Recipients.find(character); it != r.Recipients.end() &&
                (it->second.State == Stage::Offered || it->second.State == Stage::Uncertain)) return false;
        return true;
    }
    if (revision || digest || m_state.Accounts.size() >= kMaxAccounts) return false;
    auto next = m_state; next.Accounts.emplace(character, Account{}); return Publish(std::move(next));
}
std::optional<uint64_t> RewardLedger::Create(RewardIdentity identity, int32_t base, const std::vector<std::string>& recipients)
{
    if (m_faulted || base <= 0 || base > kMaxBaseXP || !Normalize(identity)) return {};
    // First admissible report fixes the base AND recipient set. Different reader levels may
    // calculate different book bases; a duplicate is still this event, never a second payment.
    if (const auto* existing = Find(identity)) return existing->Id;
    if (recipients.empty() || recipients.size() > 2 || m_state.Records.size() >= kMaxRecords || m_state.NextId >= (1ull << 60) - 1) return {};
    auto next = m_state; Record record; record.Id = next.NextId++; record.Identity = std::move(identity); record.BaseXP = base;
    for (const auto& character : recipients)
    {
        const auto account = next.Accounts.find(character);
        if (account == next.Accounts.end() || account->second.Blocked || !record.Recipients.emplace(character, Delivery{}).second) return {};
    }
    const auto id = record.Id; next.Records.emplace(id, std::move(record));
    return Publish(std::move(next)) ? std::optional<uint64_t>{id} : std::nullopt;
}
std::optional<RewardLedger::Record> RewardLedger::OfferNext(const std::string& character, uint64_t now)
{
    const auto* account = FindAccount(character);
    if (m_faulted || !account || account->Blocked || now > std::numeric_limits<uint64_t>::max() - kOfferTimeoutMs ||
        account->Revision == std::numeric_limits<uint64_t>::max()) return {};
    for (const auto& [_, r] : m_state.Records)
        if (const auto it = r.Recipients.find(character); it != r.Recipients.end() && it->second.State == Stage::Offered) return {};
    for (const auto& [id, r] : m_state.Records)
    {
        const auto it = r.Recipients.find(character);
        if (it == r.Recipients.end() || it->second.State != Stage::Pending) continue;
        auto next = m_state; auto& d = next.Records.at(id).Recipients.at(character);
        d.State = Stage::Offered; d.Deadline = now + kOfferTimeoutMs;
        d.BeforeRevision = account->Revision; d.BeforeDigest = account->Digest;
        const auto offered = next.Records.at(id); // Publish REPLACES m_state; id/r/account then dangle
        if (Publish(std::move(next))) return offered;
        return {};
    }
    return {};
}
bool RewardLedger::Receipt(uint64_t id, const std::string& character, bool applied, bool uncertain, int32_t final,
    uint64_t revision, uint64_t digest)
{
    if (m_faulted || final < 0 || final > kMaxFinalXP || (!applied && final)) return false;
    const auto* record = Find(id);
    if (!record) return false;
    const auto recipient = record->Recipients.find(character);
    if (recipient == record->Recipients.end()) return false;
    const auto& old = recipient->second;
    const auto stage = uncertain ? Stage::Uncertain : applied ? Stage::Committed : Stage::Rejected;
    const auto after = old.BeforeRevision + (applied && !uncertain ? 1 : 0);
    const auto hash = applied && !uncertain ? AdvanceRewardDigest(old.BeforeDigest, id, record->BaseXP) : old.BeforeDigest;
    if (revision != after || digest != hash) return false;
    if (old.State != Stage::Offered) return old.State == stage && old.FinalXP == final && old.Deadline != 0;
    auto next = m_state; auto& d = next.Records.at(id).Recipients.at(character); d.State = stage; d.FinalXP = final;
    auto& account = next.Accounts.at(character);
    if (stage == Stage::Committed) { account.Revision = revision; account.Digest = digest; }
    else account.Blocked = true; // no silent loss or automatic replay after native refusal/uncertainty
    return Publish(std::move(next));
}
bool RewardLedger::Disconnect(const std::string& character)
{
    auto next = m_state; bool changed{};
    for (auto& [_, r] : next.Records)
        if (auto it = r.Recipients.find(character); it != r.Recipients.end())
        {
            auto& d = it->second;
            if (d.State == Stage::Offered) { d.State = Stage::Uncertain; next.Accounts.at(character).Blocked = true; changed = true; }
            else if (d.State == Stage::Pending) { d.State = Stage::Rejected; changed = true; }
        }
    return !changed || Publish(std::move(next));
}
bool RewardLedger::Expire(uint64_t now)
{
    auto next = m_state; bool changed{};
    for (auto& [_, r] : next.Records) for (auto& [character, d] : r.Recipients)
        if (d.State == Stage::Offered && now >= d.Deadline)
        { d.State = Stage::Uncertain; next.Accounts.at(character).Blocked = true; changed = true; }
    return !changed || Publish(std::move(next));
}
bool RewardLedger::Restore(State state)
{
    if (m_faulted || state.Campaign != m_state.Campaign || !ValidState(state)) { m_faulted = true; return false; }
    for (auto& [_, r] : state.Records) for (auto& [character, d] : r.Recipients)
    {
        if (d.State == Stage::Offered) { d.State = Stage::Uncertain; state.Accounts.at(character).Blocked = true; }
        else if (d.State == Stage::Pending) d.State = Stage::Rejected;
    }
    return Publish(std::move(state));
}
std::optional<std::vector<uint8_t>> RewardLedger::Encode(const State& state)
{
    if (!ValidState(state)) return {};
    auto copy = state; Codec codec; codec.State(copy);
    if (!codec.Valid || codec.Bytes.size() > DurableFile::kMaxBytes) return {};
    return std::move(codec.Bytes);
}
std::optional<RewardLedger::State> RewardLedger::Decode(std::span<const uint8_t> bytes)
{
    if (bytes.size() > DurableFile::kMaxBytes) return {};
    Codec codec; codec.Reading = true; codec.Input = bytes; State state; codec.State(state);
    if (!codec.Valid || !codec.Input.empty() || !ValidState(state)) return {};
    return state;
}
} // namespace Coop
