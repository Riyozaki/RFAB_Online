#pragma once

#include <Structs/Inventory.h>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Coop
{
// Persisted form identities use plugin filename + local form, NEVER a server mod index or EnTT id.
struct StableForm
{
    std::string Plugin;
    uint32_t Local{};
    bool Light{};
    bool operator==(const StableForm&) const = default;
};
std::optional<std::string> NormalizePlugin(std::string_view aName);
std::optional<std::string> StorageKey(std::string_view aCampaign, const StableForm& aBacking);
bool Transferable(const Inventory::Entry& aItem) noexcept;
int64_t ExactCount(const Inventory& aItems, const Inventory::Entry& aItem) noexcept;
bool SameInventoryItems(const Inventory&, const Inventory&) noexcept;

class InventoryLedger
{
public:
    enum class Status : uint8_t { Prepared, Committed, Rejected, Conflict, Quarantined };
    struct Endpoint
    {
        Inventory Items;
        uint64_t Revision{1};
        uint32_t Epoch{};
        bool Storage{}, NeedsImport{}, Quarantined{};
    };
    struct Request
    {
        std::string Character, Source, Destination;
        uint64_t Nonce{}, SourceRevision{}, DestinationRevision{};
        uint32_t SourceEpoch{}, DestinationEpoch{};
        Inventory::Entry Item;
        bool operator==(const Request&) const = default;
    };
    struct Record
    {
        uint64_t Id{}, Deadline{};
        Request Action;
        Status State{Status::Prepared};
    };
    struct State
    {
        std::string Campaign;
        uint64_t NextId{1};
        std::map<std::string, Endpoint> Endpoints;
        std::map<uint64_t, Record> Records;
    };
    using Commit = std::function<bool(const State&)>;
    static constexpr size_t kMaxRecords = 8192, kMaxEndpoints = 4096, kMaxItems = 32768;

    InventoryLedger(std::string aCampaign, Commit aCommit);
    const State& Snapshot() const noexcept { return m_state; }
    const Endpoint* Find(const std::string& aKey) const noexcept;
    const Record* Find(uint64_t aId) const noexcept;
    bool Faulted() const noexcept { return m_faulted; }
    bool Locked(const std::string& aKey) const noexcept;
    // All mutations publish ONLY after durable commit. No callback means failure, not a memory-only success.
    bool Register(std::string aKey, Endpoint aEndpoint);
    bool Observe(const std::string& aKey, const Inventory& aItems, uint32_t aEpoch);
    bool ImportStorage(const std::string& aKey, const Inventory& aItems, bool aConfirmed);
    Record Prepare(const Request& aRequest, uint64_t aNow);
    Record Receipt(uint64_t aId, const std::string& aCharacter, bool aApplied, bool aUncertain);
    bool Expire(uint64_t aNow);
    // Startup never replays an unacknowledged native operation. It locks its endpoints for explicit recovery.
    bool Restore(State aState);

    using NameForm = std::function<std::optional<StableForm>(GameId)>;
    using ResolveForm = std::function<std::optional<GameId>(const StableForm&)>;
    static std::optional<std::vector<uint8_t>> Encode(const State&, const NameForm&);
    static std::optional<State> Decode(std::span<const uint8_t>, const ResolveForm&);

private:
    bool Publish(State aNext);
    static bool ValidState(const State& aState) noexcept;
    State m_state;
    Commit m_commit;
    bool m_faulted{};
};

// Per-world client receipts. Before calling Skyrim, Executing is latched; a duplicate Prepared
// can only resend the old receipt or report uncertainty, never call RemoveItem a second time.
class TransferParticipant
{
public:
    enum class Stage { Executing, Applied, NotApplied, Uncertain, Committed };
    enum class Decision { Execute, ResendApplied, ResendRejected, Quarantine, Ignore };
    Decision Prepared(uint64_t aOperation);
    void Applied(uint64_t aOperation, bool aApplied, bool aUncertain);
    void Committed(uint64_t aOperation);
    void Clear() { m_receipts.clear(); }
private:
    std::map<uint64_t, Stage> m_receipts;
};
} // namespace Coop
