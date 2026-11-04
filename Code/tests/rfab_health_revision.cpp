// Behaviour of authoritative health writes (Code/common/Authority/Health.h).
//
// Why this file exists
// --------------------
// Health on the server is one number plus a revision that says how old a reading of that
// number is. Damage deltas moved the revision; the absolute writes did not - death, the
// ownership handoff baseline and the saved-value restore all wrote the number directly.
// That left a window in which the revision still described the *old* value, and the only
// rule that can hand health back (`higher value + matching revision = it healed`) then
// accepted a reading sampled before the change:
//
//     server: HP 50, revision 3      owner reads (50, revision 3)
//     server: killing blow -> HP 0   (revision stays 3)
//     snapshot (50, revision 3) arrives -> 50 > 0 and 3 == 3 -> kApply -> HP 50 again
//
// The actor is dead and its health is full: a corpse that stands up, a boss that heals for
// free, a hit that did not count. These cases pin the fix - every write that actually
// changes health moves the revision with it, and a write that changes nothing does not.
//
// What is NOT tested here: that the server call sites (ActorValueService, CharacterService)
// actually route through the component. That is compiler-visible, not runtime-visible, and
// `ActorValuesComponent` needs the entt server build. The rule below is the shipping one.

#include <catch2/catch.hpp>

#include <Authority/Health.h>

#include <cmath>
#include <limits>

using namespace Authority;

namespace
{
// Same contract as server/Components/ActorValuesComponent.h: value and revision are one
// piece of state, written only through ApplyAbsoluteHealth.
struct HealthCell
{
    float Hp{0.0f};
    uint32_t Revision{0};
    int Writes{0};

    void Set(float aTargetHp, float aMaxHealth = 0.0f) noexcept
    {
        const auto written = ApplyAbsoluteHealth(Hp, aTargetHp, aMaxHealth, Revision);
        const bool changed = written.Revision != Revision;
        Hp = written.Value;
        Revision = written.Revision;
        if (changed)
            ++Writes;
    }
};
} // namespace

TEST_CASE("Health writes: dying invalidates every reading taken before the killing blow",
          "[authority][health]")
{
    HealthCell actor;
    actor.Hp = 50.0f;
    actor.Revision = 3;

    // The owner restates what it sees; it is current.
    REQUIRE(ClassifyHealthSnapshot(actor.Hp, 50.0f, actor.Revision, 3) == SnapshotVerdict::kAlreadyCurrent);

    // The killing blow zeroes health. Before the fix this left revision at 3.
    actor.Set(0.0f, 100.0f);
    REQUIRE(actor.Hp == Approx(0.0f));
    REQUIRE(actor.Revision == 4);
    REQUIRE(actor.Writes == 1);

    // The in-flight reading that was sampled before the death can no longer be believed:
    // it would resolve to "HP 50, dead" - the corpse that stands up.
    REQUIRE(ClassifyHealthSnapshot(actor.Hp, 50.0f, actor.Revision, 3) == SnapshotVerdict::kStaleIncrease);

    // A reading taken after the death (same revision) is fine and does not loop forever.
    REQUIRE(ClassifyHealthSnapshot(actor.Hp, 0.0f, actor.Revision, 4) == SnapshotVerdict::kAlreadyCurrent);
    REQUIRE(ClassifyHealthSnapshot(actor.Hp, 0.0f, actor.Revision, 4) != SnapshotVerdict::kStaleIncrease);
}

TEST_CASE("Health writes: a write that changes nothing does not burn a revision",
          "[authority][health]")
{
    HealthCell actor;
    actor.Hp = 75.0f;
    actor.Revision = 9;

    actor.Set(75.0f, 100.0f);
    actor.Set(75.4f, 100.0f); // inside the noise band: still the same reading

    REQUIRE(actor.Revision == 9);
    REQUIRE(actor.Writes == 0);
    // Honest readings must keep working: a revision that moves for no reason turns every
    // owner into a stale sender and starves it of corrections.
    REQUIRE(ClassifyHealthSnapshot(actor.Hp, 75.0f, actor.Revision, 9) == SnapshotVerdict::kAlreadyCurrent);
}

TEST_CASE("Health writes: taking over an NPC moves the baseline revision as well",
          "[authority][health]")
{
    // The previous owner left the boss at 40 HP; the handoff baseline is the server's own
    // value, and a snapshot from the old owner must not be able to rewrite it.
    HealthCell boss;
    boss.Hp = 40.0f;
    boss.Revision = 12;

    boss.Set(55.0f, 500.0f); // saved value restored on claim
    REQUIRE(boss.Hp == Approx(55.0f));
    REQUIRE(boss.Revision == 13);

    // Old reading (40 HP on revision 12) is lower, and damage must never be lost - but it
    // is the *new* owner's job to deal it, and the server answers the old epoch explicitly
    // through the lease. What must not happen is a silent revision match.
    REQUIRE(ClassifyHealthSnapshot(boss.Hp, 55.0f, boss.Revision, 12) == SnapshotVerdict::kAlreadyCurrent);
}

TEST_CASE("Health writes: clamping at max health is still a change", "[authority][health]")
{
    HealthCell actor;
    actor.Hp = 80.0f;
    actor.Revision = 1;

    actor.Set(500.0f, 100.0f);
    REQUIRE(actor.Hp == Approx(100.0f));
    REQUIRE(actor.Revision == 2);

    actor.Set(-30.0f, 100.0f);
    REQUIRE(actor.Hp == Approx(0.0f));
    REQUIRE(actor.Revision == 3);
}

TEST_CASE("Health writes: a malformed target changes nothing at all", "[authority][health]")
{
    HealthCell actor;
    actor.Hp = 33.0f;
    actor.Revision = 5;

    actor.Set(std::nanf(""), 100.0f);
    REQUIRE(actor.Hp == Approx(33.0f));
    REQUIRE(actor.Revision == 5);

    actor.Set(std::numeric_limits<float>::infinity(), 100.0f);
    REQUIRE(actor.Hp == Approx(33.0f));
    REQUIRE(actor.Revision == 5);
}

TEST_CASE("Health writes: a heal moves the revision, and damage still lands afterwards",
          "[authority][health]")
{
    HealthCell actor;
    actor.Hp = 20.0f;
    actor.Revision = 7;

    actor.Set(90.0f, 100.0f); // owner drinking a potion, reported as an absolute value
    REQUIRE(actor.Revision == 8);

    // A teammate's hit arrives after the heal: lower health is always believed.
    REQUIRE(ClassifyHealthSnapshot(actor.Hp, 70.0f, actor.Revision, 7) == SnapshotVerdict::kApply);

    // But an old reading of the pre-heal value is not a "heal" and must not be sold as one.
    REQUIRE(ClassifyHealthSnapshot(actor.Hp, 20.0f, actor.Revision, 7) == SnapshotVerdict::kApply);
}

TEST_CASE("Health writes: the revision never goes backwards across a session",
          "[authority][health]")
{
    HealthCell actor;
    actor.Hp = 100.0f;

    const float sequence[] = {100.0f, 60.0f, 60.0f, 0.0f, 0.0f, 100.0f, 55.0f, 54.9f, 100.0f};
    uint32_t previous = actor.Revision;
    for (const float target : sequence)
    {
        actor.Set(target, 100.0f);
        REQUIRE(actor.Revision >= previous);
        previous = actor.Revision;
    }

    // Five real changes (100 -> 60 -> 0 -> 100 -> 55 -> 100); the repeating 60 and the
    // 54.9 reading inside the noise band added no revisions.
    REQUIRE(actor.Revision == 5);
}
