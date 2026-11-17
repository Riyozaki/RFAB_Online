#pragma once
#include <Coop/RewardMailbox.h>
#include <Coop/RewardLedger.h>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>

namespace Coop
{
struct PersonalRewardRequest
{
    uint64_t Epoch{}, Token{};
    uint32_t Wallet{}, Multiplier{}, Quantum{};
    bool operator==(const PersonalRewardRequest&) const = default;
};
// A Papyrus receiver submits property form IDs, not native pointers or a caller-selected XP
// amount. One request per already reserved host ticket; the main thread uses the OFFERED base.
class PersonalRewardMailbox
{
public:
    void NewEpoch(uint64_t epoch)
    {
        std::lock_guard lock(m_mutex);
        if (epoch > m_epoch) { m_epoch = epoch; m_last = 0; m_request.reset(); m_taken = false; }
    }
    bool Submit(const PersonalRewardRequest& request)
    {
        std::lock_guard lock(m_mutex);
        if (request.Epoch != m_epoch || !request.Epoch || !request.Token || !request.Wallet || !request.Multiplier ||
            request.Wallet == request.Multiplier || !request.Quantum || request.Quantum > 1000) return false;
        if (request.Token == m_last) return m_request == request;
        if (request.Token < m_last || (m_request && !m_taken)) return false;
        m_last = request.Token; m_request = request; m_taken = false; return true;
    }
    std::optional<PersonalRewardRequest> Take(uint64_t epoch, uint64_t token)
    {
        std::lock_guard lock(m_mutex);
        if (epoch != m_epoch || !m_request || m_taken || m_request->Token != token) return {};
        m_taken = true; return m_request;
    }
private:
    mutable std::mutex m_mutex;
    uint64_t m_epoch{}, m_last{};
    std::optional<PersonalRewardRequest> m_request;
    bool m_taken{};
};
struct PersonalRewardPlan
{
    int32_t Final{};
    float WalletBefore{}, WalletAfter{}, EngineBefore{}, EngineAfter{};
};
// Explicit recipe: Papyrus single-precision multiply -> int truncation -> quantum rounding.
// A native RFAB.dll or changed script formula must provide its OWN adapter, not claim this one.
inline std::optional<int32_t> PapyrusPersonalRewardXP(int32_t base, float multiplier, uint32_t quantum)
{
    if (base <= 0 || base > RewardLedger::kMaxBaseXP || !std::isfinite(multiplier) || multiplier < 0.f || !quantum || quantum > 1000) return {};
    const float multiplied = static_cast<float>(base) * multiplier;
    if (!std::isfinite(multiplied) || multiplied > static_cast<double>(RewardLedger::kMaxFinalXP) + quantum) return {};
    const auto truncated = static_cast<int64_t>(multiplied);
    const auto final = truncated - truncated % quantum;
    if (final > RewardLedger::kMaxFinalXP) return {};
    return static_cast<int32_t>(final);
}
// Forward calculation only. The two actual native stores are distinct and both are read back.
// Refuse a float pool too large to represent this EXACT increment rather than silently losing XP.
inline std::optional<PersonalRewardPlan> PlanPersonalReward(int32_t base, float multiplier, float wallet, float engine, uint32_t quantum)
{
    const auto final = PapyrusPersonalRewardXP(base, multiplier, quantum);
    if (!final || !std::isfinite(wallet) || !std::isfinite(engine) || wallet < 0.f || engine < 0.f) return {};
    const float nextWallet = static_cast<float>(static_cast<double>(wallet) + *final);
    const float nextEngine = static_cast<float>(static_cast<double>(engine) + *final);
    if (!std::isfinite(nextWallet) || !std::isfinite(nextEngine) ||
        static_cast<double>(nextWallet) - wallet != *final || static_cast<double>(nextEngine) - engine != *final) return {};
    return PersonalRewardPlan{*final, wallet, nextWallet, engine, nextEngine};
}
}
