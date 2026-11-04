// Behaviour of the downed-on-knee / ally-revive rules (Code/common/Downed/Revive.h).
//
// These rules were extracted verbatim from DownedStateService so they could be exercised in
// the fast loop instead of only in a live game. Every case below pins the exact behaviour the
// service had before the split, so the extraction is provably faithful and a future edit that
// changes the mechanic fails here rather than in a playtest.

#include <catch2/catch.hpp>

#include <Downed/Revive.h>

using namespace Downed;

namespace
{
constexpr uint32_t kAllyA = 2;
constexpr uint32_t kAllyB = 3;
constexpr uint32_t kAllyC = 4;
} // namespace

TEST_CASE("Downed health gate: on the knee every heal and every DoT tick is frozen",
          "[rfab][downed][healthgate]")
{
    // System inactive (single-player / disconnected): nothing is blocked.
    REQUIRE_FALSE(ShouldBlockHealthMutation(false, false, true, false, -10.0f));
    REQUIRE_FALSE(ShouldBlockHealthMutation(false, false, true, false, +10.0f));

    // An authorized revive heal always passes, even while downed.
    REQUIRE_FALSE(ShouldBlockHealthMutation(true, true, true, false, +50.0f));

    // Downed: both directions frozen, but a zero delta is a no-op and passes.
    REQUIRE(ShouldBlockHealthMutation(true, false, true, false, +25.0f));   // potion/regen
    REQUIRE(ShouldBlockHealthMutation(true, false, true, false, -25.0f));   // poison/DoT
    REQUIRE_FALSE(ShouldBlockHealthMutation(true, false, true, false, 0.0f));

    // Post-revive grace: damage blocked, heals still land.
    REQUIRE(ShouldBlockHealthMutation(true, false, false, true, -10.0f));
    REQUIRE_FALSE(ShouldBlockHealthMutation(true, false, false, true, +10.0f));
    REQUIRE_FALSE(ShouldBlockHealthMutation(true, false, false, true, 0.0f));

    // Standing, no grace: everything passes.
    REQUIRE_FALSE(ShouldBlockHealthMutation(true, false, false, false, -10.0f));
    REQUIRE_FALSE(ShouldBlockHealthMutation(true, false, false, false, +10.0f));
}

TEST_CASE("Downed targeting: the closest downed ally in reach is chosen",
          "[rfab][downed][targeting]")
{
    // Local player at the origin; reach of 100 units.
    const DownedCandidate candidates[] = {
        {kAllyA, 30.0f, 0.0f, 0.0f, true},   // downed, 30 away
        {kAllyB, 10.0f, 0.0f, 0.0f, true},   // downed, 10 away  <- nearest
        {kAllyC, 5.0f, 0.0f, 0.0f, false},   // closer but NOT downed -> ignored
    };

    REQUIRE(SelectNearestDownedAlly(0, 0, 0, 100.0f, candidates, 3) == kAllyB);
}

TEST_CASE("Downed targeting: standing allies, id 0, and out-of-range allies are skipped",
          "[rfab][downed][targeting]")
{
    const DownedCandidate candidates[] = {
        {0, 1.0f, 0.0f, 0.0f, true},          // id 0 -> skipped
        {kAllyA, 1.0f, 0.0f, 0.0f, false},    // not downed -> skipped
        {kAllyB, 500.0f, 0.0f, 0.0f, true},   // downed but out of reach -> skipped
    };
    REQUIRE(SelectNearestDownedAlly(0, 0, 0, 100.0f, candidates, 3) == 0);

    // A candidate exactly at the range boundary is included.
    const DownedCandidate boundary[] = {{kAllyA, 100.0f, 0.0f, 0.0f, true}};
    REQUIRE(SelectNearestDownedAlly(0, 0, 0, 100.0f, boundary, 1) == kAllyA);

    // No candidates at all.
    REQUIRE(SelectNearestDownedAlly(0, 0, 0, 100.0f, nullptr, 0) == 0);
}

TEST_CASE("Downed targeting: distance is measured in 3D",
          "[rfab][downed][targeting]")
{
    const DownedCandidate candidates[] = {
        {kAllyA, 60.0f, 0.0f, 0.0f, true},    // 60 away horizontally
        {kAllyB, 0.0f, 0.0f, 50.0f, true},    // 50 away vertically <- nearer in 3D
    };
    REQUIRE(SelectNearestDownedAlly(0, 0, 0, 100.0f, candidates, 2) == kAllyB);
}

TEST_CASE("Revive channel: holding E on a downed ally starts, then accumulates, then completes a bar",
          "[rfab][downed][channel]")
{
    ReviveChannel channel;
    REQUIRE_FALSE(channel.Active);
    REQUIRE(channel.Progress() == 0.0f);

    // First held tick starts the channel and asks the service to send StartChannel.
    auto step = channel.Update(kAllyA, true, 0.0, 4.0);
    REQUIRE(step.Action == ChannelAction::Start);
    REQUIRE(step.TargetPlayerId == kAllyA);
    REQUIRE(channel.Active);
    REQUIRE(channel.TargetPlayerId == kAllyA);
    REQUIRE(channel.DurationSec == 4.0);

    // Subsequent held ticks accumulate elapsed time without sending anything.
    step = channel.Update(kAllyA, true, 1.0, 4.0);
    REQUIRE(step.Action == ChannelAction::None);
    REQUIRE(channel.ElapsedSec == Approx(1.0));
    REQUIRE(channel.Progress() == Approx(0.25f));

    step = channel.Update(kAllyA, true, 1.0, 4.0);
    REQUIRE(channel.Progress() == Approx(0.5f));

    // Progress is clamped at 1 even if elapsed overshoots the duration.
    channel.Update(kAllyA, true, 10.0, 4.0);
    REQUIRE(channel.Progress() == 1.0f);
}

TEST_CASE("Revive channel: releasing E cancels and reports the previous target",
          "[rfab][downed][channel]")
{
    ReviveChannel channel;
    channel.Update(kAllyA, true, 0.0, 4.0);
    channel.Update(kAllyA, true, 2.0, 4.0);

    // Key released -> Cancel with the target we were channeling, and the channel resets.
    const auto step = channel.Update(kAllyA, false, 0.0, 4.0);
    REQUIRE(step.Action == ChannelAction::Cancel);
    REQUIRE(step.TargetPlayerId == kAllyA);
    REQUIRE_FALSE(channel.Active);
    REQUIRE(channel.TargetPlayerId == 0);
    REQUIRE(channel.ElapsedSec == 0.0);
    REQUIRE(channel.Progress() == 0.0f);

    // Releasing again while idle sends nothing.
    const auto idle = channel.Update(0, false, 0.0, 4.0);
    REQUIRE(idle.Action == ChannelAction::None);
}

TEST_CASE("Revive channel: walking away from the target (no ally in reach) cancels",
          "[rfab][downed][channel]")
{
    ReviveChannel channel;
    channel.Update(kAllyA, true, 0.0, 4.0);

    // Still holding E but no downed ally in reach anymore -> cancel.
    const auto step = channel.Update(0, true, 0.0, 4.0);
    REQUIRE(step.Action == ChannelAction::Cancel);
    REQUIRE(step.TargetPlayerId == kAllyA);
    REQUIRE_FALSE(channel.Active);
}

TEST_CASE("Revive channel: switching to a different downed ally retargets with a fresh Start",
          "[rfab][downed][channel]")
{
    ReviveChannel channel;
    channel.Update(kAllyA, true, 0.0, 4.0);
    channel.Update(kAllyA, true, 2.0, 4.0);
    REQUIRE(channel.ElapsedSec == Approx(2.0));

    // A nearer downed ally appears; the channel retargets and resets elapsed.
    const auto step = channel.Update(kAllyB, true, 0.0, 4.0);
    REQUIRE(step.Action == ChannelAction::Start);
    REQUIRE(step.TargetPlayerId == kAllyB);
    REQUIRE(channel.TargetPlayerId == kAllyB);
    REQUIRE(channel.ElapsedSec == 0.0);
}

TEST_CASE("Revive channel: server confirmation clears only the matching target",
          "[rfab][downed][channel]")
{
    ReviveChannel channel;
    channel.Update(kAllyA, true, 0.0, 4.0);

    // A confirmation for a different player must not clear our channel.
    REQUIRE_FALSE(channel.ClearIfTarget(kAllyB));
    REQUIRE(channel.Active);

    // The server confirms our target revived -> channel clears.
    REQUIRE(channel.ClearIfTarget(kAllyA));
    REQUIRE_FALSE(channel.Active);
    REQUIRE(channel.TargetPlayerId == 0);

    // Clearing an already-idle channel is a no-op.
    REQUIRE_FALSE(channel.ClearIfTarget(kAllyA));
}

TEST_CASE("Revive channel: a degenerate duration never divides by zero",
          "[rfab][downed][channel]")
{
    ReviveChannel channel;
    channel.Update(kAllyA, true, 0.0, 0.0); // misconfigured duration
    channel.Update(kAllyA, true, 1.0, 0.0);
    REQUIRE(channel.Active);
    REQUIRE(channel.Progress() == 0.0f); // guarded, not inf/nan
}

TEST_CASE("Revive heal: a fraction of effective max, floored above zero, never damages",
          "[rfab][downed][heal]")
{
    // 30% of a 100 max from 0 HP -> 30 HP, a +30 heal.
    auto heal = ComputeReviveHeal(100.0f, 0.0f, 0.0f, 0.30f);
    REQUIRE(heal.TargetHp == Approx(30.0f));
    REQUIRE(heal.HealDelta == Approx(30.0f));

    // Temporary modifier counts toward effective max: 100 + 50 = 150, 30% -> 45.
    heal = ComputeReviveHeal(100.0f, 50.0f, 0.0f, 0.30f);
    REQUIRE(heal.TargetHp == Approx(45.0f));

    // Fraction is clamped into [0.05, 1.0].
    heal = ComputeReviveHeal(100.0f, 0.0f, 0.0f, 0.0f);   // -> 5%
    REQUIRE(heal.TargetHp == Approx(5.0f));
    heal = ComputeReviveHeal(100.0f, 0.0f, 0.0f, 2.0f);   // -> 100%
    REQUIRE(heal.TargetHp == Approx(100.0f));

    // Effective max floored at 10; target floored at 1 so the player always leaves bleedout.
    heal = ComputeReviveHeal(1.0f, 0.0f, 0.0f, 0.05f);    // max->10, 5% = 0.5 -> floor 1
    REQUIRE(heal.TargetHp == Approx(1.0f));
    REQUIRE(heal.HealDelta == Approx(1.0f));

    // Already above the target: heal delta is 0, never a negative (damaging) heal.
    heal = ComputeReviveHeal(100.0f, 0.0f, 80.0f, 0.30f); // target 30, current 80
    REQUIRE(heal.TargetHp == Approx(30.0f));
    REQUIRE(heal.HealDelta == 0.0f);
}

TEST_CASE("Revive grace: the invulnerability window is clamped to [0, 15] seconds",
          "[rfab][downed][grace]")
{
    REQUIRE(ClampGraceSec(-5.0) == 0.0);
    REQUIRE(ClampGraceSec(0.0) == 0.0);
    REQUIRE(ClampGraceSec(3.0) == 3.0);
    REQUIRE(ClampGraceSec(15.0) == 15.0);
    REQUIRE(ClampGraceSec(99.0) == 15.0);
}
