#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include "../common/CoopDamagePipeline.h"
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/AddTargetRequest.h>
#include <Messages/NotifyAddTarget.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/NotifyHealthChangeBroadcast.h>

namespace
{
std::filesystem::path FindV310RepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "CoopDamagePipeline.h") &&
            fs::exists(cur / "Code" / "client" / "Games" / "Skyrim" / "Actor.cpp"))
        {
            return cur;
        }
        if (!cur.has_parent_path())
            break;
        cur = cur.parent_path();
    }
    return {};
}

std::string LoadV310File(const std::filesystem::path& aRoot, const char* apRelPath)
{
    std::ifstream in(aRoot / apRelPath, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("v3.1 Pillar 1: 1/M Enemy Combat Regen & ActiveEffect Heal Parity", "[rfab][v310][regen]")
{
    using namespace CoopDamagePipeline;

    SECTION("Requiem/RFAB Troll DPS check preserves exact TTK ratio instead of becoming immortal under M=3")
    {
        // Solo RFAB Troll: 400 MaxHP, +40 HP/sec ActiveEffect regen vs 100 DPS solo player:
        // Net solo DPS = 100 - 40 = 60 HP/s -> TTK = 400 / 60 = 6.6667s.
        // Without 1/M regen scaling in 2-player co-op (M = 3.0):
        //   100 raw DPS -> 33.333 scaled DPS < +40 HP/s unscaled regen -> Troll was 100% IMMORTAL!
        // With ScaleIncomingEnemyHealDelta (1/M):
        //   +40 HP/s -> +13.333 scaled HP/s; 3 players / M=3 -> exact 1:1 net DPS parity!
        constexpr float kTrollMaxHp = 400.0f;
        constexpr float kMult2Players = 3.0f;

        const float scaledPlayerDps = ScaleIncomingEnemyDamageDelta(-100.0f, kMult2Players, true);
        const float scaledTrollRegenPerSec = ScaleIncomingEnemyHealDelta(
            +40.0f,
            kTrollMaxHp,
            kMult2Players,
            /*aIsEligibleEnemyTarget=*/true,
            /*aIsActiveEffectTick=*/true);

        REQUIRE(scaledPlayerDps == Approx(-100.0f / 3.0f));
        REQUIRE(scaledTrollRegenPerSec == Approx(+40.0f / 3.0f));

        // Net HP change per second for 1 surviving solo-DPS player = -20 HP/s
        // -> exact M * TTK_solo = 3 * 6.6667s = 20.0s (preserves exact >40 raw DPS threshold, never immortal!):
        const float netPerSecSinglePlayer = scaledPlayerDps + scaledTrollRegenPerSec;
        CHECK(netPerSecSinglePlayer == Approx(-20.0f));
        CHECK((kTrollMaxHp / -netPerSecSinglePlayer) == Approx(kMult2Players * (400.0f / 60.0f)));

        // Two players dealing 2 * 100 = -200 raw DPS under M = 3.0 -> TTK = (3 * 400) / (200 - 40) = 7.5s:
        const float scaledTwoPlayerDps = ScaleIncomingEnemyDamageDelta(-200.0f, kMult2Players, true);
        const float netTwoPlayerPerSec = scaledTwoPlayerDps + scaledTrollRegenPerSec;
        CHECK((kTrollMaxHp / -netTwoPlayerPerSec) == Approx(7.5f));
    }

    SECTION("Scripted boss phase reset bursts (>= 25% native MaxHP, non-ActiveEffect) remain 1:1 percentage-exact")
    {
        constexpr float kBossMaxHp = 1200.0f;
        constexpr float kMult3Players = 4.0f;

        // +50% phase transition script via RestoreActorValue (!s_inApplyActorEffect):
        CHECK(ScaleIncomingEnemyHealDelta(+600.0f, kBossMaxHp, kMult3Players, true, /*aIsActiveEffectTick=*/false) ==
              Approx(+600.0f));

        // Enemy Restoration mage casting +600 HP Close Wounds (ActiveEffect == true!) STILL scales by 1/M:
        CHECK(ScaleIncomingEnemyHealDelta(+600.0f, kBossMaxHp, kMult3Players, true, /*aIsActiveEffectTick=*/true) ==
              Approx(+150.0f));

        // Players, followers, summons, commanded actors, and mounts never downscale healing:
        CHECK(ScaleIncomingEnemyHealDelta(+80.0f, 300.0f, kMult3Players, /*aIsEligibleEnemyTarget=*/false, true) ==
              Approx(+80.0f));
    }
}

TEST_CASE("v3.1 Pillar 2: Caster 0x14 AdjustForPerks Ratio & Pre-Adjusted Spell Wire Sync", "[rfab][v310][magic]")
{
    using namespace CoopDamagePipeline;
    using namespace TiltedPhoques;

    SECTION("Resistance cancels out cleanly from M_after / M_before perk ratio")
    {
        // Base Fireball = 60, local bandit has 40% fire resist -> magBeforeAdjust = 36.0.
        // Guest 0x14 has CustomSkills Pyromancy + 2nd Ring + Dual Cast = 2.75x -> magAfterAdjust = 99.0.
        const float ratio = ComputeCasterOutgoingPerkMultiplier(36.0f, 99.0f);
        CHECK(ratio == Approx(2.75f));

        // Negative magnitudes cancel sign cleanly too:
        CHECK(ComputeCasterOutgoingPerkMultiplier(-20.0f, -55.0f) == Approx(2.75f));

        // Zero / NaN / Inf edge cases fall back safely to 1.0x:
        CHECK(ComputeCasterOutgoingPerkMultiplier(0.0f, 50.0f) == Approx(1.0f));
        CHECK(ComputeCasterOutgoingPerkMultiplier(std::numeric_limits<float>::quiet_NaN(), 50.0f) == Approx(1.0f));
    }

    SECTION("AddTargetRequest and NotifyAddTarget preserve HasPreAdjustedPerks across wire serialization")
    {
        AddTargetRequest req{};
        req.TargetId = 0x2001;
        req.CasterId = 0x1002;
        req.SpellId = GameId(0x04, 0x1C789);
        req.EffectId = GameId(0x04, 0x1CEA1);
        req.Magnitude = 247.5f;
        req.IsDualCasting = true;
        req.ApplyHealPerkBonus = false;
        req.ApplyStaminaPerkBonus = true;
        req.HasPreAdjustedPerks = true;

        Buffer reqBuf(512);
        Buffer::Writer reqWriter(&reqBuf);
        req.Serialize(reqWriter);

        Buffer::Reader reqReader(&reqBuf);
        const ClientMessageFactory clientFactory;
        auto extractedReq = clientFactory.Extract(reqReader);
        REQUIRE(extractedReq);
        auto rxReq = CastUnique<AddTargetRequest>(std::move(extractedReq));
        CHECK(*rxReq == req);
        CHECK(rxReq->HasPreAdjustedPerks == true);
        CHECK(rxReq->Magnitude == Approx(247.5f));

        NotifyAddTarget notify{};
        notify.TargetId = rxReq->TargetId;
        notify.CasterId = rxReq->CasterId;
        notify.SpellId = rxReq->SpellId;
        notify.EffectId = rxReq->EffectId;
        notify.Magnitude = rxReq->Magnitude;
        notify.IsDualCasting = rxReq->IsDualCasting;
        notify.ApplyHealPerkBonus = rxReq->ApplyHealPerkBonus;
        notify.ApplyStaminaPerkBonus = rxReq->ApplyStaminaPerkBonus;
        notify.HasPreAdjustedPerks = rxReq->HasPreAdjustedPerks;

        Buffer notifyBuf(512);
        Buffer::Writer notifyWriter(&notifyBuf);
        notify.Serialize(notifyWriter);

        Buffer::Reader notifyReader(&notifyBuf);
        const ServerMessageFactory serverFactory;
        auto extractedNotify = serverFactory.Extract(notifyReader);
        REQUIRE(extractedNotify);
        auto rxNotify = CastUnique<NotifyAddTarget>(std::move(extractedNotify));
        CHECK(*rxNotify == notify);
        CHECK(rxNotify->HasPreAdjustedPerks == true);
    }
}

TEST_CASE("v3.1 Pillar 3 & 4: Non-Rubberbanding CurrentHP Reconciliation & Physical Hit Impact Gate", "[rfab][v310][impact]")
{
    using namespace CoopDamagePipeline;
    using namespace TiltedPhoques;

    SECTION("Periodic kHealth reconciliation prevents upward bounce right after local hit while applying extra damage immediately")
    {
        // Guest just predicted -50 HP (500 -> 450) at t = 1000ms.
        // Stale snapshot of 490 arrives at t = 1250ms (within 700ms window): suppressed!
        CHECK_FALSE(ShouldReconcileRemoteActorHealthSnapshot(
            /*aIsRemoteNonPlayer=*/true,
            /*aIsDead=*/false,
            /*aLocalHp=*/450.0f,
            /*aSnapshotHp=*/490.0f,
            /*aNowMs=*/1250ULL,
            /*aLastLocalPredictedHitMs=*/1000ULL));

        // Host ALSO hit the enemy down to 320 HP at t = 1250ms: downward drop applies immediately!
        CHECK(ShouldReconcileRemoteActorHealthSnapshot(
            true, false, 450.0f, 320.0f, 1250ULL, 1000ULL));

        // At t = 1850ms (> 700ms after local hit), troll regenerated from 320 -> 345 on Host: reconciles smoothly!
        CHECK(ShouldReconcileRemoteActorHealthSnapshot(
            true, false, 320.0f, 345.0f, 1850ULL, 1000ULL));

        // Players and dead actors never reconcile via NPC snapshot path:
        CHECK_FALSE(ShouldReconcileRemoteActorHealthSnapshot(false, false, 100.0f, 200.0f, 5000ULL, 0ULL));
        CHECK_FALSE(ShouldReconcileRemoteActorHealthSnapshot(true, true, 0.0f, 200.0f, 5000ULL, 0ULL));
    }

    SECTION("Directional stagger angle & per-target poise recovery gate prevent co-op stunlock")
    {
        constexpr float kHalfPi = 1.57079632679f;
        // Target at origin facing North (heading = 0):
        // Attacker North (0, +100) -> 0.0 (front)
        CHECK(ComputeNormalizedStaggerDirection(0.0f, 100.0f, 0.0f, 0.0f, 0.0f) == Approx(0.0f).margin(0.001f));
        // Attacker East (+100, 0) -> 0.25 (right)
        CHECK(ComputeNormalizedStaggerDirection(100.0f, 0.0f, 0.0f, 0.0f, 0.0f) == Approx(0.25f).margin(0.001f));
        // Attacker South (0, -100) -> 0.50 (behind)
        CHECK(ComputeNormalizedStaggerDirection(0.0f, -100.0f, 0.0f, 0.0f, 0.0f) == Approx(0.50f).margin(0.001f));
        // Target facing East (heading = pi/2), Attacker East (+100, 0) -> 0.0 (front of East-facing target!)
        CHECK(ComputeNormalizedStaggerDirection(100.0f, 0.0f, 0.0f, 0.0f, kHalfPi) == Approx(0.0f).margin(0.001f));

        RemoteHitImpactGate gate;
        // Initial shield bash at t = 1000ms accepted:
        CHECK(gate.TryConsumeStagger(0x3001, 0.50f, false, 1000ULL) == Approx(0.50f));
        // Follow-up dual-wield/ally bash 400ms later blocked by poise recovery window:
        CHECK(gate.TryConsumeStagger(0x3001, 0.75f, false, 1400ULL) == Approx(0.0f));
        // Independent enemy 0x3002 staggers normally at t = 1400ms:
        CHECK(gate.TryConsumeStagger(0x3002, 0.40f, false, 1400ULL) == Approx(0.40f));
        // After 1650ms window (t = 2700ms), enemy 0x3001 can be staggered again:
        CHECK(gate.TryConsumeStagger(0x3001, 0.60f, false, 2700ULL) == Approx(0.60f));
        // Ghost/ethereal boss never staggers:
        CHECK(gate.TryConsumeStagger(0x3003, 1.0f, /*aIsTargetGhostOrDead=*/true, 5000ULL) == Approx(0.0f));
    }

    SECTION("Zero-damage Shield Bash interrupt roundtrips through RequestHealthChangeBroadcast & NotifyHealthChangeBroadcast")
    {
        RequestHealthChangeBroadcast req{};
        req.Id = 0x4010;
        req.DeltaHealth = 0.0f;
        req.SourceServerId = 0x1005;
        req.StaggerMagnitude = 0.55f;
        req.StaggerDirection = 0.25f;

        Buffer buf(256);
        Buffer::Writer writer(&buf);
        req.Serialize(writer);

        Buffer::Reader reader(&buf);
        const ClientMessageFactory clientFactory;
        auto msg = clientFactory.Extract(reader);
        REQUIRE(msg);
        auto rx = CastUnique<RequestHealthChangeBroadcast>(std::move(msg));
        CHECK(*rx == req);
        CHECK(rx->SourceServerId == 0x1005);
        CHECK(rx->StaggerMagnitude == Approx(0.55f));
        CHECK(rx->StaggerDirection == Approx(0.25f));

        NotifyHealthChangeBroadcast notify{};
        notify.Id = rx->Id;
        notify.DeltaHealth = rx->DeltaHealth;
        notify.SourceServerId = rx->SourceServerId;
        notify.StaggerMagnitude = rx->StaggerMagnitude;
        notify.StaggerDirection = rx->StaggerDirection;

        Buffer notifyBuf(256);
        Buffer::Writer notifyWriter(&notifyBuf);
        notify.Serialize(notifyWriter);

        Buffer::Reader notifyReader(&notifyBuf);
        const ServerMessageFactory serverFactory;
        auto extractedNotify = serverFactory.Extract(notifyReader);
        REQUIRE(extractedNotify);
        auto rxNotify = CastUnique<NotifyHealthChangeBroadcast>(std::move(extractedNotify));
        CHECK(*rxNotify == notify);
        CHECK(rxNotify->StaggerMagnitude == Approx(0.55f));
        CHECK(rxNotify->StaggerDirection == Approx(0.25f));
    }

    SECTION("Temporary Fortify Health buffs (Transmute Muscles / Beef Stew / Arkay) above Permanent MaxHP are never chopped")
    {
        // Player has 150 Permanent MaxHP + 250 Temporary Fortify Health = 400 CurrentHP.
        const float permMaxHp = 150.0f;
        const float currentBuffedHp = 400.0f;
        const auto afterChip = ApplyAuthoritativeServerHealthDelta(
            currentBuffedHp,
            std::max(permMaxHp, currentBuffedHp),
            -25.0f);
        CHECK(afterChip.UpdatedHealth == Approx(375.0f));
        CHECK(afterChip.ClampedBroadcastDelta == Approx(-25.0f));
    }
}

TEST_CASE("v3.1 Architectural Source Contracts across Client & Server Services", "[rfab][v310][contracts]")
{
    const auto root = FindV310RepoRoot();
    REQUIRE_FALSE(root.empty());

    const auto actorCpp = LoadV310File(root, "Code/client/Games/Skyrim/Actor.cpp");
    const auto animCpp = LoadV310File(root, "Code/client/Games/Animation.cpp");
    const auto magicTargetCpp = LoadV310File(root, "Code/client/Games/Skyrim/Magic/MagicTarget.cpp");
    const auto cliMagicCpp = LoadV310File(root, "Code/client/Services/Generic/MagicService.cpp");
    const auto cliAvCpp = LoadV310File(root, "Code/client/Services/Generic/ActorValueService.cpp");
    const auto srvAvCpp = LoadV310File(root, "Code/server/Services/ActorValueService.cpp");
    const auto cliQuestCpp = LoadV310File(root, "Code/client/Services/Generic/QuestService.cpp");
    const auto srvQuestCpp = LoadV310File(root, "Code/server/Services/QuestService.cpp");
    const auto v300Manifest = LoadV310File(root, "docs/V3.0.0_RELEASE_BACKUP_MANIFEST.json");
    const auto buildPs1 = LoadV310File(root, "Tools/rfab/Build-CloudRelease.ps1");

    // 0. Immutable v3.0.0-backup manifest & v3.1.0 release pipeline:
    CHECK(v300Manifest.find("\"v3.0.0-backup\"") != std::string::npos);
    CHECK(v300Manifest.find("0b6977459bc9e6e62d8bf6f36925cba2f3a83b486c3a67f6edcad0aaeb8aa298") != std::string::npos);
    CHECK(v300Manifest.find("dbb8aa7294af29ba16cb1291f8bbb6027a3825c148e4c0ae2b748f74418c1fe9") != std::string::npos);
    CHECK(buildPs1.find("Ensure-V300BackupRelease") != std::string::npos);
    CHECK(buildPs1.find("[string]$VersionTag = \"v3.1.1\"") != std::string::npos);

    // 1. Destruction ValueModifierEffect & Concentration beam fixes + 1/M enemy heal scaling:
    CHECK(actorCpp.find("ScaleIncomingEnemyHealDelta") != std::string::npos);
    CHECK(actorCpp.find("s_inLocalCasterConcentrationEffect") != std::string::npos);
    CHECK(actorCpp.find("TryAcceptRemoteHitReactionAction") != std::string::npos);
    CHECK(animCpp.find("Actor::TryAcceptRemoteHitReactionAction") != std::string::npos);

    // 2. Outgoing 0x14 AdjustForPerks spell ratio + Frame-0 CastSpellImmediate guard + 0x14 incoming perk preservation:
    CHECK(magicTargetCpp.find("ScopedLocalCasterPerkCapture") != std::string::npos);
    CHECK(magicTargetCpp.find("ComputeCasterOutgoingPerkMultiplier") != std::string::npos);
    CHECK(magicTargetCpp.find("allowLocalPlayerPvpBeam") != std::string::npos);
    CHECK(cliMagicCpp.find("m_concentrationClaimKeepaliveMs") != std::string::npos);
    CHECK(cliMagicCpp.find("resyncEvt.HasPreAdjustedPerks = true;") != std::string::npos);

    // 3. Periodic CurrentHP reconciliation + Temporary HP preservation + RemoteHitImpactGate stagger & cast interrupt:
    CHECK(cliAvCpp.find("ShouldReconcileRemoteActorHealthSnapshot") != std::string::npos);
    CHECK(cliAvCpp.find("m_remoteHitImpactGate.TryConsumeStagger") != std::string::npos);
    CHECK(cliAvCpp.find("pActor->InterruptCast(false);") != std::string::npos);
    CHECK(cliAvCpp.find("BroadcastCombatHealthSnapshots") != std::string::npos);
    CHECK(srvAvCpp.find("std::max(maxHealth, currentHealth)") != std::string::npos);

    // 4. Host-canonical BGSScene authority, liveStageId capture & echo suppression:
    CHECK(cliQuestCpp.find("ScopedRemoteQuestUpdateGuard remoteSceneGuard") != std::string::npos);
    CHECK(cliQuestCpp.find("update.Status = RequestQuestUpdate::StageUpdateForced;") != std::string::npos);
    CHECK(srvQuestCpp.find("ignoring non-leader scene") != std::string::npos);
}
