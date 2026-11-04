// Behaviour of the shared-XP ledger (Code/common/CoopExperienceLedger.h).
//
// RFAB awards experience to every party member equally, but each player carries a
// personal RFAB_XP_Multiplier (Adventurer's Ring, Standing Stone, ...). The wire therefore
// carries a 1x *base* award: the sender divides out its own multiplier, each receiver
// multiplies by its own. Quest-stage XP is the hard part — it can arrive both as a local
// synced quest stage and as a remote NotifySyncRfabExperience, in either order, and must
// be counted exactly once. None of this had a behavioural test before.
//
// Every case below is a thing a player would notice: XP doubled, XP missing, a personal
// multiplier applied twice, or wallet churn while the perks menu is open leaking XP.

#include <catch2/catch.hpp>

#include <CoopExperienceLedger.h>

#include <cmath>
#include <limits>

using namespace CoopExperienceLedger;

namespace
{
constexpr uint64_t t0 = 1000; // arbitrary "now" baseline in ms

// Drive the tracker to an initialized wallet baseline without broadcasting anything.
ClientRfabXpTracker InitializedAt(int32_t aWallet, uint64_t aNowMs = 500)
{
    ClientRfabXpTracker tracker;
    const auto first = tracker.EvaluateLocalWalletPoll(aWallet, true, false, 1.0f, aNowMs);
    REQUIRE(first.BroadcastBaseXp == 0);
    REQUIRE(first.RefundToApply == 0);
    REQUIRE(tracker.IsInitialized());
    return tracker;
}
} // namespace

TEST_CASE("XP normalize: the sender divides out its personal multiplier and snaps to Requiem's 5s",
          "[rfab][xp][normalize]")
{
    // No gain / negative gain never normalizes to a positive base.
    REQUIRE(NormalizeSenderBaseXp(0, 2.0f) == 0);
    REQUIRE(NormalizeSenderBaseXp(-50, 2.0f) == 0);

    // A 1x sender passes the award through untouched (it is already the base).
    REQUIRE(NormalizeSenderBaseXp(103, 1.0f) == 103);

    // A 2x sender who saw 206 locally really earned a 103 base -> snapped to 105.
    REQUIRE(NormalizeSenderBaseXp(206, 2.0f) == 105);

    // A 0.5x sender who saw 50 locally earned a 100 base.
    REQUIRE(NormalizeSenderBaseXp(50, 0.5f) == 100);

    // Invalid multipliers (zero, negative, NaN, inf) must not divide-by-zero or poison the
    // award; the raw delta is treated as the base instead.
    REQUIRE(NormalizeSenderBaseXp(100, 0.0f) == 100);
    REQUIRE(NormalizeSenderBaseXp(100, -1.0f) == 100);
    REQUIRE(NormalizeSenderBaseXp(100, std::numeric_limits<float>::quiet_NaN()) == 100);
    REQUIRE(NormalizeSenderBaseXp(100, std::numeric_limits<float>::infinity()) == 100);
}

TEST_CASE("XP scale: each receiver applies its own multiplier with a 5-XP floor",
          "[rfab][xp][scale]")
{
    REQUIRE(ScaleReceiverPersonalXp(0, 2.0f) == 0);
    // Above the wire ceiling is rejected outright (a hostile/buggy base must not mint XP).
    REQUIRE(ScaleReceiverPersonalXp(kMaxRfabXpDelta + 1, 1.0f) == 0);

    // 1x receiver keeps the base exactly.
    REQUIRE(ScaleReceiverPersonalXp(100, 1.0f) == 100);

    // 2x receiver doubles a 100 base.
    REQUIRE(ScaleReceiverPersonalXp(100, 2.0f) == 200);

    // 0.1x receiver of a 100 base gets 10 (snapped to a multiple of 5).
    REQUIRE(ScaleReceiverPersonalXp(100, 0.1f) == 10);

    // A tiny award that would scale below 1 still grants the 5-XP floor, never 0.
    REQUIRE(ScaleReceiverPersonalXp(3, 0.1f) == 5);

    // Invalid multiplier falls back to the unmodified base.
    REQUIRE(ScaleReceiverPersonalXp(100, 0.0f) == 100);
    REQUIRE(ScaleReceiverPersonalXp(100, std::numeric_limits<float>::quiet_NaN()) == 100);
}

TEST_CASE("XP round-trip: one base award, every player scaled by their own multiplier",
          "[rfab][xp][roundtrip]")
{
    // Sender with a 2x ring sees 200 locally -> the wire base is 100.
    const int32_t base = NormalizeSenderBaseXp(200, 2.0f);
    REQUIRE(base == 100);

    // The same base reaches a 1x player as 100 and a 3x player as 300. Neither sees the
    // sender's 2x applied to them — that was divided out on the way to the wire.
    REQUIRE(ScaleReceiverPersonalXp(static_cast<uint32_t>(base), 1.0f) == 100);
    REQUIRE(ScaleReceiverPersonalXp(static_cast<uint32_t>(base), 3.0f) == 300);
    // And the sender's own client, scaling the base by its 2x, reproduces what it saw.
    REQUIRE(ScaleReceiverPersonalXp(static_cast<uint32_t>(base), 2.0f) == 200);
}

TEST_CASE("XP quest dedup, order A: local quest stage first absorbs the matching remote packet",
          "[rfab][xp][dedup]")
{
    auto tracker = InitializedAt(0);

    // The synced quest stage lands locally and opens the dedup window.
    tracker.OnRemoteQuestStageApplied(t0);

    // The local wallet then jumps by the quest award (100). It is held in the window, not
    // broadcast — the remote copy is expected to arrive and be absorbed.
    const auto localPoll = tracker.EvaluateLocalWalletPoll(100, true, false, 1.0f, t0 + 100);
    REQUIRE(localPoll.BroadcastBaseXp == 0);
    REQUIRE(localPoll.RefundToApply == 0);
    REQUIRE(tracker.GetLocalQuestXpGainedInWindow() == 100);

    // The remote NotifySyncRfabExperience for the same quest arrives: fully absorbed.
    const int32_t applied = tracker.EvaluateIncomingRemoteXp(100, 1.0f, t0 + 200);
    REQUIRE(applied == 0);
    REQUIRE(tracker.GetLocalQuestXpGainedInWindow() == 0);
}

TEST_CASE("XP quest dedup, order A: a remote award larger than the local one only absorbs the overlap",
          "[rfab][xp][dedup]")
{
    auto tracker = InitializedAt(0);
    tracker.OnRemoteQuestStageApplied(t0);

    // Local quest award was 60.
    const auto localPoll = tracker.EvaluateLocalWalletPoll(60, true, false, 1.0f, t0 + 100);
    REQUIRE(localPoll.BroadcastBaseXp == 0);
    REQUIRE(tracker.GetLocalQuestXpGainedInWindow() == 60);

    // Remote says 100: 60 is the duplicate and absorbed, the extra 40 is genuine and applied.
    const int32_t applied = tracker.EvaluateIncomingRemoteXp(100, 1.0f, t0 + 200);
    REQUIRE(applied == 40);
    REQUIRE(tracker.GetLocalQuestXpGainedInWindow() == 0);
}

TEST_CASE("XP quest dedup, order B: remote packet first is refunded when the local stage fires",
          "[rfab][xp][dedup]")
{
    auto tracker = InitializedAt(0);

    // The remote XP arrives before the local quest stage and is applied immediately.
    const int32_t applied = tracker.EvaluateIncomingRemoteXp(100, 1.0f, t0 - 100);
    REQUIRE(applied == 100);
    tracker.CommitAppliedRemoteXp(0, applied, t0 - 100);

    // Now the synced quest stage fires locally: the premature credit is queued for refund.
    tracker.OnRemoteQuestStageApplied(t0);
    REQUIRE(tracker.GetCreditToRefundOnLocalQuestXp() == 100);

    // The local wallet jumps by the same quest award; the ledger refunds the premature 100
    // instead of counting the quest twice.
    const auto localPoll = tracker.EvaluateLocalWalletPoll(200, true, false, 1.0f, t0 + 100);
    REQUIRE(localPoll.RefundToApply == 100);
    REQUIRE(localPoll.BroadcastBaseXp == 0);

    tracker.CommitLocalRefundApplied(100, 100);
    REQUIRE(tracker.GetCreditToRefundOnLocalQuestXp() == 0);
    REQUIRE(tracker.GetLastWallet() == 100);
}

TEST_CASE("XP: wallet churn while the perks menu is open is suppressed, not broadcast",
          "[rfab][xp][menu]")
{
    auto tracker = InitializedAt(0);

    // Opening CustomSkills moves the wallet (spending/preview), but nothing may be broadcast.
    const auto whileOpen = tracker.EvaluateLocalWalletPoll(100, true, true, 1.0f, 600);
    REQUIRE(whileOpen.BroadcastBaseXp == 0);
    REQUIRE(whileOpen.RefundToApply == 0);

    // After the menu closes the baseline has tracked the churn, so no phantom award leaks.
    const auto afterClose = tracker.EvaluateLocalWalletPoll(100, true, false, 1.0f, 700);
    REQUIRE(afterClose.BroadcastBaseXp == 0);

    // A genuine gain after closing still broadcasts normally.
    const auto genuine = tracker.EvaluateLocalWalletPoll(150, true, false, 1.0f, 800);
    REQUIRE(genuine.BroadcastBaseXp == 50);
}

TEST_CASE("XP: a normal (non-quest) gain broadcasts its normalized base",
          "[rfab][xp][broadcast]")
{
    auto tracker = InitializedAt(0);

    // A 2x sender who gained 200 broadcasts a 100 base, not 200.
    const auto poll = tracker.EvaluateLocalWalletPoll(200, true, false, 2.0f, 600);
    REQUIRE(poll.BroadcastBaseXp == 100);
    REQUIRE(poll.RefundToApply == 0);
}

TEST_CASE("XP: disconnecting resets the baseline so a rejoin does not broadcast a stale jump",
          "[rfab][xp][disconnect]")
{
    auto tracker = InitializedAt(0);

    // While disconnected the tracker must not initialize or broadcast, and must forget state.
    const auto offline = tracker.EvaluateLocalWalletPoll(999, false, false, 1.0f, 600);
    REQUIRE(offline.BroadcastBaseXp == 0);
    REQUIRE_FALSE(tracker.IsInitialized());

    // Rejoining re-baselines at the current wallet; the next genuine gain broadcasts cleanly.
    const auto rejoin = tracker.EvaluateLocalWalletPoll(999, true, false, 1.0f, 700);
    REQUIRE(rejoin.BroadcastBaseXp == 0);
    REQUIRE(tracker.IsInitialized());
    const auto gain = tracker.EvaluateLocalWalletPoll(1049, true, false, 1.0f, 800);
    REQUIRE(gain.BroadcastBaseXp == 50);
}

TEST_CASE("XP: once the quest window lapses, local gains broadcast normally again",
          "[rfab][xp][window]")
{
    auto tracker = InitializedAt(0);
    tracker.OnRemoteQuestStageApplied(t0); // window closes at t0 + kQuestXpWindowDurationMs

    // Well after the window, a local gain is ordinary XP and must broadcast (the stale
    // quest-window state is cleared rather than suppressing awards forever).
    const uint64_t late = t0 + kQuestXpWindowDurationMs + 1000;
    const auto poll = tracker.EvaluateLocalWalletPoll(100, true, false, 1.0f, late);
    REQUIRE(poll.BroadcastBaseXp == 100);
    REQUIRE(tracker.GetLocalQuestXpGainedInWindow() == 0);
}
