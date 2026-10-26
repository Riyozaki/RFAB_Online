#include <catch2/catch.hpp>

#include <CoopAnimationState.h>
#include <CoopOwnership.h>
#include <PlaybackClock.h>
#include <client/Games/Skyrim/Havok/hkStepInfo.h>

#include <cmath>
#include <limits>
#include <random>
#include <vector>

TEST_CASE("Stage 2: SitSleep/Furniture state normalizer & replay classifier", "[rfab][stage2][animation]")
{
    using namespace CoopAnimationState;

    SECTION("Exit classifier catches combat get-up, fast furniture exit, and graph stop events")
    {
        REQUIRE(IsFurnitureOrIdleExitEvent("IdleChairExit"));
        REQUIRE(IsFurnitureOrIdleExitEvent("IdleChairExitStart"));
        REQUIRE(IsFurnitureOrIdleExitEvent("IdleChairGetUp"));
        REQUIRE(IsFurnitureOrIdleExitEvent("IdleBedGetUp"));
        REQUIRE(IsFurnitureOrIdleExitEvent("IdleFurnitureExitFast"));
        REQUIRE(IsFurnitureOrIdleExitEvent("IdleFurnitureExitSlow"));
        REQUIRE(IsFurnitureOrIdleExitEvent("IdleStop"));
        REQUIRE(IsFurnitureOrIdleExitEvent("IdleStopInstant"));
        REQUIRE(IsFurnitureOrIdleExitEvent("IdleForceDefaultState"));
        REQUIRE(IsFurnitureOrIdleExitEvent("GetUpBegin"));
        REQUIRE(IsFurnitureOrIdleExitEvent("StoolBackExit"));
        REQUIRE(IsFurnitureOrIdleExitEvent("TableBackExit"));

        REQUIRE_FALSE(IsFurnitureOrIdleExitEvent("IdleChairEnterStart"));
        REQUIRE_FALSE(IsFurnitureOrIdleExitEvent("IdleChairEnterInstant"));
        REQUIRE_FALSE(IsFurnitureOrIdleExitEvent("IdleChairVar1"));
        REQUIRE_FALSE(IsFurnitureOrIdleExitEvent("attackStart"));
        REQUIRE_FALSE(IsFurnitureOrIdleExitEvent(""));
    }

    SECTION("Mid-air sitting bandit fighting without nearby furniture snaps to standing root")
    {
        const uint32_t sittingFlags1 = (3u << kSitSleepShift); // kIsSitting
        const uint32_t drawnWeaponFlags2 = (3u << kWeaponStateShift);

        const auto dec = EvaluateRemoteSitSleepTransition(
            sittingFlags1,
            sittingFlags1,
            drawnWeaponFlags2,
            /*aHasValidNearbyFurniture=*/false,
            "weaponSwing",
            /*aHorizontalSpeedUnitsPerSec=*/140.0f);

        REQUIRE(GetSitSleepState(dec.NormalizedFlags1) == 0u);
        REQUIRE(dec.ForceDefaultIdleGraph);
    }

    SECTION("Calm tavern NPC legitimately sitting on nearby chair keeps sitting state")
    {
        const uint32_t sittingFlags1 = (3u << kSitSleepShift);
        const uint32_t sheathedFlags2 = 0u;

        const auto dec = EvaluateRemoteSitSleepTransition(
            0u,
            sittingFlags1,
            sheathedFlags2,
            /*aHasValidNearbyFurniture=*/true,
            "IdleChairEnterInstant",
            0.0f);

        REQUIRE(GetSitSleepState(dec.NormalizedFlags1) == 3u);
        REQUIRE_FALSE(dec.ForceDefaultIdleGraph);
    }

    SECTION("Missed exit packet (sit -> normal without exit event) triggers ForceDefaultIdleGraph")
    {
        const uint32_t prevSittingFlags1 = (3u << kSitSleepShift);
        const uint32_t nowStandingFlags1 = 0u;
        const uint32_t drawnWeaponFlags2 = (3u << kWeaponStateShift);

        const auto missedExit = EvaluateRemoteSitSleepTransition(
            prevSittingFlags1,
            nowStandingFlags1,
            drawnWeaponFlags2,
            false,
            "weaponDraw",
            0.0f);
        REQUIRE(missedExit.ForceDefaultIdleGraph);

        const auto explicitExit = EvaluateRemoteSitSleepTransition(
            prevSittingFlags1,
            nowStandingFlags1,
            drawnWeaponFlags2,
            false,
            "IdleChairGetUp",
            0.0f);
        REQUIRE_FALSE(explicitExit.ForceDefaultIdleGraph);
    }

    SECTION("Local ownership sanitizer clears stuck SitSleepState when furniture is absent")
    {
        uint32_t flags1 = (3u << kSitSleepShift) | (2u << kMeleeAttackShift) | (1u << kKnockStateShift);
        uint32_t flags2 = (2u << kWeaponStateShift) | kRecoilMask | kStaggeredMask;

        const bool forceExit = NormalizeLocalOwnershipActorState(flags1, flags2, /*aHasValidNearbyFurniture=*/false);
        REQUIRE(forceExit);
        REQUIRE(GetSitSleepState(flags1) == 0u);
        REQUIRE(GetMeleeAttackState(flags1) == 0u);
        REQUIRE((flags1 & kKnockStateMask) == 0u);
        REQUIRE(GetWeaponState(flags2) == 3u);
        REQUIRE((flags2 & (kRecoilMask | kStaggeredMask)) == 0u);
    }

    SECTION("Implicit replay reset prunes obsolete sitting action followed by standing combat")
    {
        const uint32_t sittingState1 = (3u << kSitSleepShift);
        const uint32_t standingState1 = 0u;
        const uint32_t drawnState2 = (3u << kWeaponStateShift);

        std::vector<ReplayEntryView> chain = {
            {"IdleChairEnterStart", sittingState1, 0u},
            {"ActionDraw", standingState1, drawnState2},
            {"attackStart", standingState1 | (1u << kMeleeAttackShift), drawnState2},
        };

        REQUIRE(FindImplicitFurnitureResetIndex(chain) == 1);
    }
}

TEST_CASE("Stage 2: Whiterun guard & RFAB Fortify-Health armor death protection", "[rfab][stage2][ownership]")
{
    SECTION("Unwounded Whiterun guard whose RFAB armor finished equipping keeps 520/520 HP")
    {
        bool claimedDead = false;
        float claimedHp = 520.0f;
        float claimedMaxHp = 520.0f;

        CoopOwnership::ReconcileClaimedStateWithMax(
            /*aServerIsDead=*/false,
            /*aServerCurrentHealth=*/220.0f,
            /*aServerMaxHealth=*/220.0f,
            /*aHasAuthoritativeHealth=*/true,
            claimedDead,
            claimedHp,
            claimedMaxHp);

        REQUIRE_FALSE(claimedDead);
        REQUIRE(claimedHp == Approx(520.0f));
        REQUIRE(claimedMaxHp == Approx(520.0f));
    }

    SECTION("Genuinely wounded boss preserves combat damage ratio without resurrecting or healing")
    {
        bool claimedDead = false;
        float claimedHp = 2000.0f;
        float claimedMaxHp = 2000.0f;

        CoopOwnership::ReconcileClaimedStateWithMax(
            /*aServerIsDead=*/false,
            /*aServerCurrentHealth=*/400.0f,
            /*aServerMaxHealth=*/1000.0f,
            /*aHasAuthoritativeHealth=*/true,
            claimedDead,
            claimedHp,
            claimedMaxHp);

        REQUIRE_FALSE(claimedDead);
        REQUIRE(claimedHp == Approx(800.0f)); // 40% of 2000
    }

    SECTION("Dead server state always overrides stale live local claim")
    {
        bool claimedDead = false;
        float claimedHp = 520.0f;
        float claimedMaxHp = 520.0f;

        CoopOwnership::ReconcileClaimedStateWithMax(
            /*aServerIsDead=*/true,
            /*aServerCurrentHealth=*/0.0f,
            /*aServerMaxHealth=*/520.0f,
            /*aHasAuthoritativeHealth=*/true,
            claimedDead,
            claimedHp,
            claimedMaxHp);

        REQUIRE(claimedDead);
        REQUIRE(claimedHp == Approx(0.0f));
    }

    SECTION("ComputeSafeHealthStepPlan never transiently drives live actor health <= 0 when scaling down")
    {
        // Actor currently has Permanent=1500, DamageMod=-1100 -> CurrentHealth=400.
        // Naive PERMANENT=520 first would yield 520 - 1100 = -580 <= 0 -> instant engine death!
        float permMax = 1500.0f;
        float damageMod = -1100.0f;
        auto getCurrentHp = [&]() { return permMax + damageMod; };
        auto forcePermanent = [&](float targetPerm) { permMax = targetPerm; };
        auto forceDamage = [&](float targetCur) { damageMod = targetCur - permMax; };

        const auto plan = CoopOwnership::ComputeSafeHealthStepPlan(
            /*aCurrentPermanentMax=*/permMax,
            /*aDesiredPermanentMax=*/520.0f,
            /*aDesiredCurrentHealth=*/220.0f,
            /*aKeepActorAlive=*/true);

        REQUIRE(plan.AdjustDamageBeforePermanent);
        forceDamage(plan.DamageModeTarget);
        REQUIRE(getCurrentHp() > 0.0f); // Intermediate health is 1200 > 0 (safe!)
        forcePermanent(plan.PermanentModeTarget);
        REQUIRE(permMax == Approx(520.0f));
        REQUIRE(getCurrentHp() == Approx(220.0f));
    }

    SECTION("IsValidHandoffCoordinates blocks unloaded-form (0,0,0) origin warp on exterior guards")
    {
        REQUIRE_FALSE(CoopOwnership::IsValidHandoffCoordinates(
            /*aHasWorldOrCell=*/false, true, 0.f, 0.f, 0.f, true, 19122.f, -7151.f, -3578.f));
        REQUIRE_FALSE(CoopOwnership::IsValidHandoffCoordinates(
            /*aHasWorldOrCell=*/true, /*aIsExterior=*/true, 0.f, 0.f, 0.f, true, 19122.f, -7151.f, -3578.f));
        REQUIRE(CoopOwnership::IsValidHandoffCoordinates(
            /*aHasWorldOrCell=*/true, /*aIsExterior=*/false, 0.f, 0.f, 0.f, true, 120.f, -80.f, 0.f));
        REQUIRE(CoopOwnership::IsValidHandoffCoordinates(
            /*aHasWorldOrCell=*/true, /*aIsExterior=*/true, 19122.f, -7151.f, -3578.f, true, 19100.f, -7140.f, -3578.f));
    }
}

TEST_CASE("Stage 2: Adaptive PlaybackClock & hkStepInfo Havok timing guards", "[rfab][stage2][physics]")
{
    SECTION("PlaybackClock converges smoothly at 30 Hz and adapts quickly to 10 Hz post-combat slowdown")
    {
        PlaybackClock clock;
        std::mt19937 rng(42);
        std::uniform_real_distribution<double> jitterDist(-4.0, 4.0);

        double prevRenderTick = -1.0;
        int starvedCount = 0;
        uint64_t lastCapture = 0;

        for (double t = 1000.0; t <= 9000.0; t += 16.6667)
        {
            const double interval = (t >= 5000.0) ? 100.0 : 33.3333;
            if (lastCapture == 0 || (t - static_cast<double>(lastCapture)) >= interval)
            {
                lastCapture = static_cast<uint64_t>(t);
                const double arrival = t + 20.0 + jitterDist(rng);
                REQUIRE(clock.AddSample(arrival, lastCapture));
                // Duplicate capture tick must be ignored
                REQUIRE_FALSE(clock.AddSample(arrival + 1.0, lastCapture));
            }

            if (clock.IsRunning())
            {
                clock.Advance(t + 20.0);
                const double rt = clock.GetRenderTick();
                if (prevRenderTick >= 0.0)
                    REQUIRE(rt >= prevRenderTick - 0.001);
                prevRenderTick = rt;

                if (rt > static_cast<double>(lastCapture))
                    ++starvedCount;
            }
        }

        REQUIRE(clock.GetSnapshotInterval() >= 90.0);
        REQUIRE(clock.GetTargetDelay() <= PlaybackClock::cMaxTargetDelay);
        REQUIRE(starvedCount < 25);

        clock.Reset();
        REQUIRE_FALSE(clock.IsRunning());
    }

    SECTION("hkStepInfo rejects zero/NaN deltaTime and maintains finite invDeltaTime (#901)")
    {
        hkStepInfo step{};
        step.startTime = 0.f;
        step.endTime = 0.f;
        step.deltaTime = 0.f;
        step.invDeltaTime = 0.f;

        REQUIRE(step.UpdateDeltaTime(0.0f, 0.0f));
        REQUIRE(step.deltaTime == Approx(1.f / 60.f));
        REQUIRE(step.invDeltaTime == Approx(60.f));

        REQUIRE(step.UpdateDeltaTime(std::numeric_limits<float>::quiet_NaN(), -1.0f));
        REQUIRE(std::isfinite(step.deltaTime));
        REQUIRE(std::isfinite(step.invDeltaTime));
        REQUIRE(step.deltaTime > 0.0001f);

        REQUIRE(step.UpdateDeltaTime(0.01f, 0.5f)); // clamped to 0.1f max
        REQUIRE(step.deltaTime == Approx(0.1f));
        REQUIRE(step.invDeltaTime == Approx(10.0f));
    }
}
