#pragma once
#include <Coop/RewardBridge.h>
#include <mutex>
#include <optional>
#include <vector>

namespace Coop
{
// Engine/Papyrus threads only exchange bounded owned values. They never inspect ECS, the
// network, a wallet, or a cached actor. A stale completion cannot release a newer native grant.
class RewardMailbox
{
public:
    struct Event { uint64_t Nonce{}; Rewards::Source Source; };
    static constexpr size_t kCapacity = 64;
    void NewEpoch(uint64_t);
    bool Register(uint64_t, const Rewards::Adapter&);
    std::optional<Rewards::Adapter> Adapter(uint64_t) const;
    void Configure(uint64_t, Rewards::Mode);
    Rewards::Mode Mode() const;
    Rewards::Submission Submit(uint64_t, const Rewards::Source&);
    std::vector<Event> Take(uint64_t);
    bool BeginApplication(uint64_t aEpoch, uint64_t aToken);
    // Papyrus events are asynchronous. Only one invocation may claim a dispatched ticket;
    // before EVERY pool write it must recheck that load/teardown has not revoked the claim.
    bool ClaimApplication(uint64_t aEpoch, uint64_t aToken);
    bool CanApply(uint64_t aEpoch, uint64_t aToken) const;
    bool Complete(uint64_t aEpoch, const Rewards::Completion&);
    std::optional<Rewards::Completion> Completion(uint64_t, uint64_t) const;
    bool FinishApplication(uint64_t, uint64_t);
    bool ConsumeFault();
    bool HasPending() const;
    void Block();
private:
    void Reset(uint64_t);
    void Fault();
    mutable std::mutex m_mutex;
    uint64_t m_epoch{}, m_sequence{}, m_token{};
    Rewards::Mode m_mode{Rewards::Mode::Offline};
    std::optional<Rewards::Adapter> m_adapter;
    std::optional<Rewards::Completion> m_completion;
    std::vector<Event> m_events;
    bool m_fault{}, m_broken{}, m_claimed{};
};
std::optional<int32_t> PersonalRewardXP(int32_t aBase, double aMultiplier, uint32_t aQuantum = 5) noexcept;
} // namespace Coop
