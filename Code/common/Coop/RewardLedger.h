#pragma once

#include <Coop/InventoryLedger.h>
#include <Structs/CoopProtocol.h>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Coop
{
// Identity of the CAUSE, before any personal multiplier or wallet/spending operation.
// Kill: exact reference + server-validated life/death; quest: quest + cycle/reward slot;
// book: canonical book form, once per campaign. Equal amounts are never identities.
struct RewardIdentity
{
    RewardKind Kind{};
    StableForm Source;
    std::optional<StableForm> Context;
    uint64_t Incarnation{}, Occurrence{};
    bool operator==(const RewardIdentity&) const = default;
};
std::optional<std::string> SemanticRewardKey(const RewardIdentity&);
uint64_t AdvanceRewardDigest(uint64_t aPrevious, uint64_t aReward, int32_t aBaseXP) noexcept;

class RewardLedger
{
public:
    enum class Stage : uint8_t { Pending, Offered, Committed, Rejected, Uncertain };
    struct Account
    {
        uint64_t Revision{}, Digest{};
        bool Blocked{};
    };
    struct Delivery
    {
        Stage State{Stage::Pending};
        uint64_t Deadline{}, BeforeRevision{}, BeforeDigest{};
        int32_t FinalXP{}; // diagnostic/receipt only: never another player's multiplier or wallet
        bool operator==(const Delivery&) const = default;
    };
    struct Record
    {
        uint64_t Id{};
        RewardIdentity Identity;
        int32_t BaseXP{};
        std::map<std::string, Delivery> Recipients;
    };
    struct State
    {
        std::string Campaign;
        uint64_t NextId{1};
        std::map<std::string, Account> Accounts;
        std::map<uint64_t, Record> Records;
    };
    static constexpr size_t kMaxRecords = 8192, kMaxAccounts = 256;
    static constexpr int32_t kMaxBaseXP = 1000000, kMaxFinalXP = 100000000;
    static constexpr uint64_t kOfferTimeoutMs = 15000;
    using Commit = std::function<bool(const State&)>;

    RewardLedger(std::string aCampaign, Commit aCommit);
    const State& Snapshot() const noexcept { return m_state; }
    bool Faulted() const noexcept { return m_faulted; }
    const Record* Find(uint64_t) const noexcept;
    const Record* Find(const RewardIdentity&) const;
    const Account* FindAccount(const std::string&) const noexcept;
    // Reconnect is a comparison, NEVER an XP catch-up. Unknown applied state blocks admission.
    bool Attach(const std::string& aCharacter, uint64_t aRevision, uint64_t aDigest);
    std::optional<uint64_t> Create(RewardIdentity, int32_t aBaseXP, const std::vector<std::string>& aRecipients);
    // Persist Offered BEFORE publishing a grant. Exactly one native grant per character at a time.
    std::optional<Record> OfferNext(const std::string&, uint64_t aNow);
    bool Receipt(uint64_t, const std::string&, bool aApplied, bool aUncertain, int32_t aFinalXP,
        uint64_t aRevision, uint64_t aDigest);
    bool Disconnect(const std::string&);
    bool Expire(uint64_t aNow);
    // Pending (never sent) is cancelled; Offered is quarantined, not replayed after a restart.
    bool Restore(State);
    static std::optional<std::vector<uint8_t>> Encode(const State&);
    static std::optional<State> Decode(std::span<const uint8_t>);
private:
    bool Publish(State);
    static bool ValidState(const State&);
    State m_state;
    Commit m_commit;
    bool m_faulted{};
};
} // namespace Coop
