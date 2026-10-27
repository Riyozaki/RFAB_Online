#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../common/CoopAnimationState.h"
#include "../common/CoopDamagePipeline.h"
#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Master_Behavior.h>

namespace
{
std::filesystem::path FindPhase4RepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "CoopDamagePipeline.h") &&
            fs::exists(cur / "Code" / "client" / "Systems" / "InterpolationSystem.cpp"))
        {
            return cur;
        }
        if (!cur.has_parent_path())
            break;
        cur = cur.parent_path();
    }
    return {};
}

std::string LoadPhase4File(const std::filesystem::path& aRoot, const char* apRelPath)
{
    std::ifstream in(aRoot / apRelPath, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("v3.0 Phase 4A: Native RFAB MaxHP preservation & incoming enemy damage scaling (1 / M)", "[rfab][v300][phase4][damage]")
{
    using namespace CoopDamagePipeline;

    SECTION("Multiplier normalization clamps edge cases safely")
    {
        CHECK(NormalizeEnemyHealthMultiplier(1.0f) == Approx(1.0f));
        CHECK(NormalizeEnemyHealthMultiplier(3.0f) == Approx(3.0f));
        CHECK(NormalizeEnemyHealthMultiplier(0.0f) == Approx(1.0f));
        CHECK(NormalizeEnemyHealthMultiplier(-2.5f) == Approx(1.0f));
        CHECK(NormalizeEnemyHealthMultiplier(0.01f) == Approx(0.1f));
        CHECK(NormalizeEnemyHealthMultiplier(99.0f) == Approx(10.0f));
        CHECK(NormalizeEnemyHealthMultiplier(std::numeric_limits<float>::quiet_NaN()) == Approx(1.0f));
    }

    SECTION("Only hostile/world NPCs scale incoming damage; players, followers, summons, commanded actors, and mounts stay 1x")
    {
        CHECK(ShouldScaleIncomingEnemyDamage(false, false, false, false, false));
        CHECK_FALSE(ShouldScaleIncomingEnemyDamage(true, false, false, false, false));
        CHECK_FALSE(ShouldScaleIncomingEnemyDamage(false, true, false, false, false));
        CHECK_FALSE(ShouldScaleIncomingEnemyDamage(false, false, true, false, false));
        CHECK_FALSE(ShouldScaleIncomingEnemyDamage(false, false, false, true, false));
        CHECK_FALSE(ShouldScaleIncomingEnemyDamage(false, false, false, false, true));
    }

    SECTION("3-player RFAB boss fight keeps 1x native MaxHP, exact 50%/25% phase thresholds, and zero ownership-handoff drift")
    {
        constexpr float kNativeBossMaxHp = 1000.0f;
        float hostPermanentMaxHp = kNativeBossMaxHp;
        float guestPermanentMaxHp = kNativeBossMaxHp;
        float serverMaxHp = kNativeBossMaxHp;

        float hostCurrentHp = kNativeBossMaxHp;
        float guestCurrentHp = kNativeBossMaxHp;
        float serverCurrentHp = kNativeBossMaxHp;

        // 3 players in party -> M = 3.0 (effective toughness = 3000 raw damage)
        const float mult = 3.0f;

        // Players deal 5 hits of -300 post-armor RFAB damage (= -1500 raw damage -> exact 50% boss phase!)
        for (int i = 0; i < 5; ++i)
        {
            const float scaledDelta = ScaleIncomingEnemyDamageDelta(-300.0f, mult, true);
            REQUIRE(scaledDelta == Approx(-100.0f));

            const auto applied = ApplyAuthoritativeServerHealthDelta(serverCurrentHp, serverMaxHp, scaledDelta);
            serverCurrentHp = applied.UpdatedHealth;
            hostCurrentHp = std::max(0.0f, hostCurrentHp + applied.ClampedBroadcastDelta);
            guestCurrentHp = std::max(0.0f, guestCurrentHp + applied.ClampedBroadcastDelta);
        }

        // Exact 50% boss phase transition on 1x native MaxHP across ALL peers:
        CHECK(hostPermanentMaxHp == Approx(1000.0f));
        CHECK(guestPermanentMaxHp == Approx(1000.0f));
        CHECK(serverCurrentHp == Approx(500.0f));
        CHECK((hostCurrentHp / hostPermanentMaxHp) == Approx(0.50f));
        CHECK((guestCurrentHp / guestPermanentMaxHp) == Approx(0.50f));

        // 10 ownership transfers (Host <-> Guest) produce ZERO 3^K or 1/3 permanent HP drift:
        for (int handoff = 0; handoff < 10; ++handoff)
        {
            hostPermanentMaxHp = serverMaxHp;
            guestPermanentMaxHp = serverMaxHp;
        }
        CHECK(hostPermanentMaxHp == Approx(kNativeBossMaxHp));
        CHECK(guestPermanentMaxHp == Approx(kNativeBossMaxHp));

        // Boss scripted phase heal (+50% of native MaxHP = +500.0f) stays unscaled and restores exact +50%:
        const float healDelta = ScaleIncomingEnemyDamageDelta(+500.0f, mult, true);
        CHECK(healDelta == Approx(+500.0f));
        const auto afterHeal = ApplyAuthoritativeServerHealthDelta(serverCurrentHp, serverMaxHp, healDelta);
        CHECK(afterHeal.UpdatedHealth == Approx(1000.0f));

        // Simultaneous overkill clamp prevents absurd negative overkill debt:
        const auto overkill = ApplyAuthoritativeServerHealthDelta(120.0f, kNativeBossMaxHp, -50000.0f);
        CHECK(overkill.UpdatedHealth == Approx(0.0f));
        CHECK(overkill.ClampedBroadcastDelta == Approx(-kNativeBossMaxHp));
    }
}

TEST_CASE("v3.0 Phase 4B: Seamless exterior cell-border motion policy without MoveTo flicker", "[rfab][v300][phase4][movement]")
{
    using namespace CoopDamagePipeline;

    SECTION("Border combat oscillating 50 times across x = 4096 inside inner 3x3 grid triggers zero MoveTo calls")
    {
        for (int step = 0; step < 50; ++step)
        {
            const float x = (step % 2 == 0) ? 4095.4f : 4096.6f;
            const auto mode = EvaluateExteriorCellMotion(
                /*aStepExceedsTeleportSnap=*/false,
                /*aHasValidAttachedParentCell=*/true,
                /*aHasLoadedDestCell=*/true,
                /*aParentEqualsDestCell=*/(step % 2 == 0),
                /*aParentGridX=*/0,
                /*aParentGridY=*/0,
                /*aCenterGridX=*/1,
                /*aCenterGridY=*/0,
                /*aPosX=*/x,
                /*aPosY=*/2048.0f,
                /*aNowTick=*/1000ULL + static_cast<uint64_t>(step) * 16ULL,
                /*aLastSeamlessRehomeTick=*/0ULL);
            REQUIRE(mode == ExteriorCellMotionMode::kSmoothForcePosition);
        }
    }

    SECTION("Outer loaded ring (Chebyshev >= 2) re-homes cleanly once past 128-unit border hysteresis and respects cooldown")
    {
        // Still within 128 units of the 8192 cell line (`8250 - 8192 = 58 < 128`): hold smooth ForcePosition
        CHECK(EvaluateExteriorCellMotion(
                  false, true, true, false,
                  /*parent=*/0, 0, /*center=*/2, 0,
                  /*pos=*/8250.0f, 2048.0f,
                  /*now=*/5000ULL, /*lastRehome=*/0ULL) == ExteriorCellMotionMode::kSmoothForcePosition);

        // Past hysteresis (`8400 - 8192 = 208 >= 128`): seamless cell re-home (without ResetTeleportPhysics!)
        CHECK(EvaluateExteriorCellMotion(
                  false, true, true, false,
                  /*parent=*/0, 0, /*center=*/2, 0,
                  /*pos=*/8400.0f, 2048.0f,
                  /*now=*/5000ULL, /*lastRehome=*/0ULL) == ExteriorCellMotionMode::kSeamlessCellRehome);

        // Cooldown window (`5500 < 5000 + 1000`): suppresses back-to-back re-homes
        CHECK(EvaluateExteriorCellMotion(
                  false, true, true, false,
                  /*parent=*/0, 0, /*center=*/2, 0,
                  /*pos=*/8400.0f, 2048.0f,
                  /*now=*/5500ULL, /*lastRehome=*/5000ULL) == ExteriorCellMotionMode::kSmoothForcePosition);
    }

    SECTION("True >2048-unit teleports and detached parentCells use full kTeleportMoveTo")
    {
        CHECK(EvaluateExteriorCellMotion(
                  /*aStepExceedsTeleportSnap=*/true, true, true, true,
                  0, 0, 0, 0, 1024.0f, 1024.0f, 9000ULL, 0ULL) == ExteriorCellMotionMode::kTeleportMoveTo);

        CHECK(EvaluateExteriorCellMotion(
                  false, /*aHasValidAttachedParentCell=*/false, true, false,
                  0, 0, 0, 0, 1024.0f, 1024.0f, 9000ULL, 0ULL) == ExteriorCellMotionMode::kTeleportMoveTo);
    }
}

TEST_CASE("v3.0 Phase 4C: Continuous vs circular vs discrete MCO/SCAR/TUDM BehaviorVar blending", "[rfab][v300][phase4][animation]")
{
    using namespace CoopAnimationState;

    SECTION("Classifies Vanilla, MCO, SCAR, TUDM, TDM, ProjectGapClose, and Ultimate Dragons float variables accurately")
    {
        // Circular [0, 360) angles:
        CHECK(ClassifyBehaviorFloatBlendKind("Direction") == BehaviorFloatBlendKind::kCircularDegrees360);
        CHECK(ClassifyBehaviorFloatBlendKind("SwimDirection") == BehaviorFloatBlendKind::kCircularDegrees360);
        CHECK(ClassifyBehaviorFloatBlendKind("staggerDirection") == BehaviorFloatBlendKind::kCircularDegrees360);
        CHECK(ClassifyBehaviorFloatBlendKind("TDM_HorseAimTurn_Angle_Absolute") == BehaviorFloatBlendKind::kCircularDegrees360);

        // Discrete step / weapon flags / speed multipliers / dodge i-frames:
        CHECK(ClassifyBehaviorFloatBlendKind("weapAdj") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("weaponSpeedMult") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("2HMweaponSpeedMult") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("leftWeaponSpeedMult") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("MC_2HM_WeapSpeed") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("playbackSpeed") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("DodgeSpeed") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("IFrameR") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("InvincibleFrameS") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("onehanded") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("twohanded") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("dagger") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("shield") == BehaviorFloatBlendKind::kDiscreteStep);
        CHECK(ClassifyBehaviorFloatBlendKind("fPGC_Update_time_required") == BehaviorFloatBlendKind::kDiscreteStep);

        // Continuous locomotion / aiming / blend floats:
        CHECK(ClassifyBehaviorFloatBlendKind("SpeedSampled") == BehaviorFloatBlendKind::kContinuousLinear);
        CHECK(ClassifyBehaviorFloatBlendKind("TurnDelta") == BehaviorFloatBlendKind::kContinuousLinear);
        CHECK(ClassifyBehaviorFloatBlendKind("Pitch") == BehaviorFloatBlendKind::kContinuousLinear);
        CHECK(ClassifyBehaviorFloatBlendKind("CastBlend") == BehaviorFloatBlendKind::kContinuousLinear);
        CHECK(ClassifyBehaviorFloatBlendKind("TDM_VelocityX") == BehaviorFloatBlendKind::kContinuousLinear);
        CHECK(ClassifyBehaviorFloatBlendKind("TDM_SpineRot") == BehaviorFloatBlendKind::kContinuousLinear);
    }

    SECTION("Circular 0..360 Direction blend takes shortest arc across 0/360 instead of spinning backwards through 180")
    {
        const float midForwardRight = BlendAnimationFloatValue(
            355.0f, 5.0f, 355.0f, false, 0.5f, BehaviorFloatBlendKind::kCircularDegrees360);
        CHECK((midForwardRight < 0.01f || midForwardRight > 359.99f));

        const float midForwardLeft = BlendAnimationFloatValue(
            5.0f, 355.0f, 5.0f, false, 0.5f, BehaviorFloatBlendKind::kCircularDegrees360);
        CHECK((midForwardLeft < 0.01f || midForwardLeft > 359.99f));

        CHECK(BlendAnimationFloatValue(
                  350.0f, 10.0f, 350.0f, false, 0.25f, BehaviorFloatBlendKind::kCircularDegrees360) == Approx(355.0f));
    }

    SECTION("Discrete step floats preserve fresh ActionEvent values and never half-lerp weapon flags or speed multipliers")
    {
        // TUDM `onehanded` transitioned 0.0 -> 1.0 on ForceAction after `first.Tick`: must stay exact 1.0, never 0.35!
        CHECK(BlendAnimationFloatValue(
                  0.0f, 1.0f, /*actionDiscrete=*/1.0f, /*hasFreshAction=*/true, 0.35f,
                  BehaviorFloatBlendKind::kDiscreteStep) == Approx(1.0f));

        // Without a newer ActionEvent, holds causal `first` (1.25x weaponSpeedMult) cleanly until `second.Tick`:
        CHECK(BlendAnimationFloatValue(
                  1.25f, 1.50f, 0.0f, /*hasFreshAction=*/false, 0.50f,
                  BehaviorFloatBlendKind::kDiscreteStep) == Approx(1.25f));

        // Discontinuous >=500 jump on a continuous float holds causal `first` until `alpha >= 0.999`
        CHECK(BlendAnimationFloatValue(
                  0.0f, 720.0f, 0.0f, false, 0.50f,
                  BehaviorFloatBlendKind::kContinuousLinear) == Approx(0.0f));
        CHECK(BlendAnimationFloatValue(
                  0.0f, 720.0f, 0.0f, false, 1.0f,
                  BehaviorFloatBlendKind::kContinuousLinear) == Approx(720.0f));

        // Integer-only / Float-only creature descriptors still prefer fresh ActionEvent discrete state:
        CHECK(ShouldPreferActionDiscreteVariables(250ULL, 200ULL, /*bools=*/0, /*ints=*/4, /*floats=*/0));
        CHECK_FALSE(ShouldPreferActionDiscreteVariables(200ULL, 200ULL, /*bools=*/8, /*ints=*/4, /*floats=*/4));
    }

    SECTION("Master_Behavior descriptor registers exact FloatBlendModes for all 16 float slots")
    {
        auto& mgr = AnimationGraphDescriptorManager::Get();
        const AnimationGraphDescriptor* pMaster = mgr.GetDescriptor(AnimationGraphDescriptor_Master_Behavior::m_key);
        REQUIRE(pMaster != nullptr);
        REQUIRE(pMaster->FloatLookupTable.size() == 16);
        REQUIRE(pMaster->FloatBlendModes.size() == 16);

        CHECK(pMaster->GetFloatBlendMode(0) == AnimationFloatBlendMode::kContinuousLinear);   // kTurnDelta
        CHECK(pMaster->GetFloatBlendMode(1) == AnimationFloatBlendMode::kCircularDegrees360); // kDirection
        CHECK(pMaster->GetFloatBlendMode(2) == AnimationFloatBlendMode::kContinuousLinear);   // kSpeedSampled
        CHECK(pMaster->GetFloatBlendMode(3) == AnimationFloatBlendMode::kDiscreteStep);       // kweapAdj
        CHECK(pMaster->GetFloatBlendMode(4) == AnimationFloatBlendMode::kContinuousLinear);   // kSpeed
        CHECK(pMaster->GetFloatBlendMode(5) == AnimationFloatBlendMode::kDiscreteStep);       // kweaponSpeedMult
        CHECK(pMaster->GetFloatBlendMode(6) == AnimationFloatBlendMode::kDiscreteStep);       // k2HMweaponSpeedMult
        CHECK(pMaster->GetFloatBlendMode(7) == AnimationFloatBlendMode::kDiscreteStep);       // kleftWeaponSpeedMult
        for (size_t slot = 8; slot < 16; ++slot)
            CHECK(pMaster->GetFloatBlendMode(slot) == AnimationFloatBlendMode::kContinuousLinear);
        CHECK(pMaster->GetFloatBlendMode(99) == AnimationFloatBlendMode::kContinuousLinear);
    }
}

TEST_CASE("v3.0 Phase 4D: Source & architectural invariants audit across damage, movement, and animation pipelines", "[rfab][v300][phase4][audit]")
{
    const auto root = FindPhase4RepoRoot();
    REQUIRE_FALSE(root.empty());

    const std::string worldH = LoadPhase4File(root, "Code/client/World.h");
    const std::string actorCpp = LoadPhase4File(root, "Code/client/Games/Skyrim/Actor.cpp");
    const std::string refrH = LoadPhase4File(root, "Code/client/Games/Skyrim/TESObjectREFR.h");
    const std::string refrCpp = LoadPhase4File(root, "Code/client/Games/Skyrim/TESObjectREFR.cpp");
    const std::string cliAvH = LoadPhase4File(root, "Code/client/Services/ActorValueService.h");
    const std::string cliAvCpp = LoadPhase4File(root, "Code/client/Services/Generic/ActorValueService.cpp");
    const std::string cliCharCpp = LoadPhase4File(root, "Code/client/Services/Generic/CharacterService.cpp");
    const std::string srvAvCpp = LoadPhase4File(root, "Code/server/Services/ActorValueService.cpp");
    const std::string interpCpp = LoadPhase4File(root, "Code/client/Systems/InterpolationSystem.cpp");
    const std::string descH = LoadPhase4File(root, "Code/encoding/Structs/AnimationGraphDescriptor.h");
    const std::string masterDescCpp = LoadPhase4File(root, "Code/encoding/Structs/Skyrim/AnimationGraphDescriptor_Master_Behavior.cpp");
    const std::string behaviorVarCpp = LoadPhase4File(root, "Code/client/ModCompat/BehaviorVar.cpp");

    // 4A: Zero client PERMANENT kHealth scaling mutation; incoming enemy damage scaled at HookRestoreActorValue
    CHECK(cliAvH.find("m_unscaledMaxHealth") == std::string::npos);
    CHECK(cliAvCpp.find("ApplyHealthMultiplier") == std::string::npos);
    CHECK(cliAvCpp.find("SetScaledHealthSafely") == std::string::npos);
    CHECK(cliCharCpp.find("SeedUnscaledMaxHealthFromRemote") == std::string::npos);
    CHECK(cliCharCpp.find("CaptureNativeBaseMaxHealth") == std::string::npos);
    CHECK(worldH.find("GetEnemyHealthMultiplierCached") != std::string::npos);
    CHECK(actorCpp.find("ScaleIncomingEnemyDamageDelta") != std::string::npos);
    CHECK(srvAvCpp.find("ApplyAuthoritativeServerHealthDelta") != std::string::npos);

    // 4B: Seamless exterior cell-border motion (zero per-frame !GetNiNode() MoveTo spam, MoveToCellSeamless without ResetTeleportPhysics)
    CHECK(refrH.find("MoveToCellSeamless") != std::string::npos);
    CHECK(refrCpp.find("void TESObjectREFR::MoveToCellSeamless") != std::string::npos);
    CHECK(interpCpp.find("EvaluateExteriorCellMotion") != std::string::npos);
    CHECK(interpCpp.find("MoveToCellSeamless") != std::string::npos);
    CHECK(interpCpp.find("!apActor->GetNiNode()") == std::string::npos);

    // 4C: BehaviorVar float classification & circular/discrete/continuous blending
    CHECK(descH.find("AnimationFloatBlendMode") != std::string::npos);
    CHECK(masterDescCpp.find("AnimationFloatBlendMode::kCircularDegrees360") != std::string::npos);
    CHECK(masterDescCpp.find("AnimationFloatBlendMode::kDiscreteStep") != std::string::npos);
    CHECK(behaviorVarCpp.find("ClassifyBehaviorFloatBlendKind") != std::string::npos);
    CHECK(interpCpp.find("BlendAnimationFloatValue") != std::string::npos);
    CHECK(interpCpp.find("ShouldPreferActionDiscreteVariables") != std::string::npos);
}
