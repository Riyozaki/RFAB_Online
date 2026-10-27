#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace CoopExperienceLedger
{
inline constexpr uint32_t kMaxRfabXpDelta = 1'000'000u;
inline constexpr uint64_t kQuestXpWindowDurationMs = 4000ULL;
inline constexpr uint64_t kPrematureRemoteXpLookbackMs = 3000ULL;

/**
 * Normalizes a local RFAB XP gain by dividing out the sender's personal RFAB_XP_Multiplier
 * (e.g. Adventurer's Ring / Standing Stone) and rounding Requiem's >= 5 awards to the nearest 5,
 * so each party peer scales from an exact 1x base award using their own personal multiplier.
 */
[[nodiscard]] inline int32_t NormalizeSenderBaseXp(int32_t aLocalDelta, float aSenderMultiplier) noexcept
{
    if (aLocalDelta <= 0)
        return 0;

    int32_t baseDelta = aLocalDelta;
    if (std::isfinite(aSenderMultiplier) && aSenderMultiplier > 0.01f && std::fabs(aSenderMultiplier - 1.0f) > 0.01f)
    {
        baseDelta = static_cast<int32_t>(std::lround(static_cast<double>(aLocalDelta) / static_cast<double>(aSenderMultiplier)));
        if (baseDelta >= 5)
            baseDelta = ((baseDelta + 2) / 5) * 5;
    }

    return baseDelta > 0 ? baseDelta : 0;
}

/**
 * Scales an incoming 1x base RFAB XP packet by the receiver's personal RFAB_XP_Multiplier,
 * snapping positive scaled grants to Requiem's 5-XP granularity with a minimum floor of 5 XP.
 */
[[nodiscard]] inline int32_t ScaleReceiverPersonalXp(uint32_t aRawBaseXp, float aReceiverMultiplier) noexcept
{
    if (aRawBaseXp == 0 || aRawBaseXp > kMaxRfabXpDelta)
        return 0;

    const int32_t rawIncoming = static_cast<int32_t>(aRawBaseXp);
    int32_t delta = rawIncoming;
    if (std::isfinite(aReceiverMultiplier) && aReceiverMultiplier > 0.0f && std::fabs(aReceiverMultiplier - 1.0f) > 0.01f)
    {
        const int32_t scaled = static_cast<int32_t>(static_cast<float>(rawIncoming) * aReceiverMultiplier);
        delta = scaled - (scaled % 5);
        if (delta <= 0 && rawIncoming > 0)
            delta = 5;
    }

    return delta > 0 ? delta : 0;
}

/**
 * Deterministic client-side ledger for bidirectional RFAB XP synchronization:
 * - Suppresses wallet baseline churn while CustomSkills / RFAB_Menu ("CustomMenu") is open.
 * - Deduplicates quest stage XP across BOTH packet arrival orders:
 *   (A) Local synced quest stage runs first -> absorbs matching incoming NotifySyncRfabExperience.
 *   (B) NotifySyncRfabExperience arrives first -> refunds premature credit when synced quest stage fires locally.
 */
class ClientRfabXpTracker
{
public:
    struct LocalPollResult
    {
        int32_t RefundToApply{0};
        uint32_t BroadcastBaseXp{0};
    };

    void Reset() noexcept
    {
        m_initialized = false;
        m_remoteQuestXpWindowUntilMs = 0;
        m_hasRemoteQuestXpWindow = false;
        m_localQuestXpGainedInWindow = 0;
        m_lastAppliedRemoteXpAmount = 0;
        m_lastAppliedRemoteXpTimeMs = 0;
        m_hasLastAppliedRemoteXp = false;
        m_creditToRefundOnLocalQuestXp = 0;
    }

    void OnRemoteQuestStageApplied(uint64_t aNowMs) noexcept
    {
        m_remoteQuestXpWindowUntilMs = aNowMs + kQuestXpWindowDurationMs;
        m_hasRemoteQuestXpWindow = true;

        if (m_hasLastAppliedRemoteXp &&
            m_lastAppliedRemoteXpAmount > 0 &&
            aNowMs >= m_lastAppliedRemoteXpTimeMs &&
            (aNowMs - m_lastAppliedRemoteXpTimeMs) <= kPrematureRemoteXpLookbackMs)
        {
            m_creditToRefundOnLocalQuestXp += m_lastAppliedRemoteXpAmount;
            m_lastAppliedRemoteXpAmount = 0;
            m_hasLastAppliedRemoteXp = false;
        }
    }

    [[nodiscard]] int32_t EvaluateIncomingRemoteXp(
        uint32_t aRawExperience,
        float aReceiverMultiplier,
        uint64_t aNowMs) noexcept
    {
        int32_t delta = ScaleReceiverPersonalXp(aRawExperience, aReceiverMultiplier);
        if (delta <= 0)
            return 0;

        if (IsInQuestWindow(aNowMs) && m_localQuestXpGainedInWindow > 0)
        {
            const int32_t absorbed = std::min(delta, m_localQuestXpGainedInWindow);
            m_localQuestXpGainedInWindow -= absorbed;
            delta -= absorbed;
            if (delta <= 0)
                return 0;
        }

        return delta;
    }

    void CommitAppliedRemoteXp(int32_t aCurrentWalletBefore, int32_t aAppliedDelta, uint64_t aNowMs) noexcept
    {
        if (!m_initialized)
        {
            m_lastWallet = aCurrentWalletBefore;
            m_initialized = true;
        }

        m_lastWallet += aAppliedDelta;
        m_lastAppliedRemoteXpAmount = aAppliedDelta;
        m_lastAppliedRemoteXpTimeMs = aNowMs;
        m_hasLastAppliedRemoteXp = true;
    }

    void CommitAuthoritativeDelta(int32_t aNewWalletTotal, int32_t aAppliedDelta, uint64_t aNowMs) noexcept
    {
        m_lastWallet = aNewWalletTotal;
        m_initialized = true;
        m_lastAppliedRemoteXpAmount = aAppliedDelta;
        m_lastAppliedRemoteXpTimeMs = aNowMs;
        m_hasLastAppliedRemoteXp = true;
    }

    [[nodiscard]] LocalPollResult EvaluateLocalWalletPoll(
        int32_t aCurrentWallet,
        bool aIsConnected,
        bool aIsCustomSkillMenuOpen,
        float aSenderMultiplier,
        uint64_t aNowMs) noexcept
    {
        LocalPollResult out{};
        if (!aIsConnected)
        {
            m_initialized = false;
            return out;
        }

        if (!IsInQuestWindow(aNowMs))
        {
            m_localQuestXpGainedInWindow = 0;
            m_creditToRefundOnLocalQuestXp = 0;
        }

        if (!m_initialized)
        {
            m_lastWallet = aCurrentWallet;
            m_initialized = true;
            return out;
        }

        if (aIsCustomSkillMenuOpen)
        {
            m_lastWallet = aCurrentWallet;
            return out;
        }

        int32_t delta = aCurrentWallet - m_lastWallet;
        m_lastWallet = aCurrentWallet;

        if (delta <= 0)
            return out;

        if (IsInQuestWindow(aNowMs))
        {
            if (m_creditToRefundOnLocalQuestXp > 0)
            {
                const int32_t refund = std::min(delta, m_creditToRefundOnLocalQuestXp);
                out.RefundToApply = refund;
                delta -= refund;
            }

            if (delta > 0)
                m_localQuestXpGainedInWindow += delta;

            return out;
        }

        const int32_t baseDelta = NormalizeSenderBaseXp(delta, aSenderMultiplier);
        if (baseDelta > 0)
        {
            out.BroadcastBaseXp = static_cast<uint32_t>(
                std::min<int32_t>(baseDelta, static_cast<int32_t>(kMaxRfabXpDelta)));
        }

        return out;
    }

    void CommitLocalRefundApplied(int32_t aRefundAmount, int32_t aAdjustedWalletTotal) noexcept
    {
        if (aRefundAmount <= 0)
            return;

        m_creditToRefundOnLocalQuestXp = std::max(0, m_creditToRefundOnLocalQuestXp - aRefundAmount);
        m_lastWallet = aAdjustedWalletTotal;
    }

    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] int32_t GetLastWallet() const noexcept { return m_lastWallet; }
    [[nodiscard]] int32_t GetLocalQuestXpGainedInWindow() const noexcept { return m_localQuestXpGainedInWindow; }
    [[nodiscard]] int32_t GetCreditToRefundOnLocalQuestXp() const noexcept { return m_creditToRefundOnLocalQuestXp; }

private:
    [[nodiscard]] bool IsInQuestWindow(uint64_t aNowMs) const noexcept
    {
        return m_hasRemoteQuestXpWindow && aNowMs <= m_remoteQuestXpWindowUntilMs;
    }

    int32_t m_lastWallet{0};
    bool m_initialized{false};
    uint64_t m_remoteQuestXpWindowUntilMs{0};
    bool m_hasRemoteQuestXpWindow{false};
    int32_t m_localQuestXpGainedInWindow{0};
    int32_t m_lastAppliedRemoteXpAmount{0};
    uint64_t m_lastAppliedRemoteXpTimeMs{0};
    bool m_hasLastAppliedRemoteXp{false};
    int32_t m_creditToRefundOnLocalQuestXp{0};
};
} // namespace CoopExperienceLedger
