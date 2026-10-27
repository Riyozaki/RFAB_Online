#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <CoopOwnership.h>
#include <RfabModpackCompat.h>

#include <entt/entt.hpp>
#include <Server.hpp>
#include <Messages/ReviveActionRequest.h>
#include <Services/EncounterService.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
std::filesystem::path LocateRepoRootPhase2()
{
    auto cur = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (std::filesystem::exists(cur / "Code" / "client" / "Games" / "Skyrim" / "Actor.cpp") &&
            std::filesystem::exists(cur / "Code" / "client" / "Services" / "DownedStateService.cpp"))
        {
            return cur;
        }
        if (!cur.has_parent_path())
            break;
        cur = cur.parent_path();
    }
    throw std::runtime_error("Failed to locate RFAB_Online repository root for Phase 2 test suite");
}

std::string ReadRepoFilePhase2(const std::filesystem::path& acRoot, const char* apRelPath)
{
    std::ifstream ifs(acRoot / apRelPath, std::ios::binary);
    if (!ifs.is_open())
        throw std::runtime_error(std::string("Unable to open ") + apRelPath);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}
} // namespace

TEST_CASE("RFAB v3.0 Phase 2.1: Native 0.0f HP Essential + NoBleedoutRecovery replaces 1.0f HP cushion & bit-hacks", "[rfab][v300][phase2][bleedout]")
{
    const auto root = LocateRepoRootPhase2();

    SECTION("PlayerService preserves SetPlayerRespawnMode(true) while DownedSystemEnabled is active and restores mortality on disconnect")
    {
        const std::string playerSvcCpp = ReadRepoFilePhase2(root, "Code/client/Services/Generic/PlayerService.cpp");
        REQUIRE(playerSvcCpp.find("keepNativeBleedoutMode") != std::string::npos);
        REQUIRE(playerSvcCpp.find("pPlayer->SetPlayerRespawnMode(aSet || keepNativeBleedoutMode)") != std::string::npos);
        REQUIRE(playerSvcCpp.find("pPlayer->SetPlayerRespawnMode(false)") != std::string::npos);
    }

    SECTION("Actor hooks use exact 0.0f native bleedout with TriggerNativeBleedout and zero 1.0f / 65536.0f cushions")
    {
        const std::string actorH = ReadRepoFilePhase2(root, "Code/client/Games/Skyrim/Actor.h");
        const std::string actorCpp = ReadRepoFilePhase2(root, "Code/client/Games/Skyrim/Actor.cpp");

        REQUIRE(actorH.find("void TriggerNativeBleedout() noexcept;") != std::string::npos);
        REQUIRE(actorCpp.find("void Actor::TriggerNativeBleedout() noexcept") != std::string::npos);
        REQUIRE(actorCpp.find("ScopedNativeBleedoutNudgeGuard") != std::string::npos);
        REQUIRE(actorCpp.find("downedSvc.LatchNativeBleedoutEntered(apThis)") != std::string::npos);

        // Legacy artificial HP cushions must be completely gone
        REQUIRE(actorCpp.find("ScopedLethalPlayerDamageIntercept") == std::string::npos);
        REQUIRE(actorCpp.find("s_interceptLethalPlayerDamageInHook") == std::string::npos);
        REQUIRE(actorCpp.find("65536.0f") == std::string::npos);
        REQUIRE(actorCpp.find("1.0f - curHp") == std::string::npos);
    }

    SECTION("DownedStateService & AnimationSystem implement native SetNoBleedoutRecovery(false) revive and late-packet protection")
    {
        const std::string downedH = ReadRepoFilePhase2(root, "Code/client/Services/DownedStateService.h");
        const std::string downedCpp = ReadRepoFilePhase2(root, "Code/client/Services/DownedStateService.cpp");
        const std::string animSysCpp = ReadRepoFilePhase2(root, "Code/client/Systems/AnimationSystem.cpp");

        REQUIRE(downedH.find("void LatchNativeBleedoutEntered(Actor* apPlayer) noexcept;") != std::string::npos);
        REQUIRE(downedH.find("InterceptPotentialFatalPlayerDamage") == std::string::npos);
        REQUIRE(downedH.find("FinalizeLethalHitCushion") == std::string::npos);
        REQUIRE(downedCpp.find("kDownedFloorHealth") == std::string::npos);
        REQUIRE(downedCpp.find("constexpr float kNativeBleedoutFloorHealth = 0.0f;") != std::string::npos);

        // Native revive order: SetNoBleedoutRecovery(false) BEFORE restoring positive kHealth, then re-arm SetPlayerRespawnMode(true)
        const auto allowRecoveryPos = downedCpp.find("pPlayer->SetNoBleedoutRecovery(false);");
        const auto forceHealPos = downedCpp.find("pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, targetHp);");
        REQUIRE(allowRecoveryPos != std::string::npos);
        REQUIRE(forceHealPos != std::string::npos);
        REQUIRE(allowRecoveryPos < forceHealPos);
        REQUIRE(downedCpp.find("if (!pPlayer->actorState.IsBleedingOut())") != std::string::npos);

        // AnimationSystem blocks in-flight pre-revive packets from re-applying kBleedout onto a revived 0xFF clone
        REQUIRE(animSysCpp.find("else if (CoopAnimationState::IsDownedOrBleedoutLifeState(apActor->actorState.flags1))") != std::string::npos);
    }
}

TEST_CASE("RFAB v3.0 Phase 2.2: Server-authoritative ally Restoration healing-spell revive", "[rfab][v300][phase2][spell_revive]")
{
    SECTION("Standing healer casting onto downed ally within 45m revives immediately and clears active E-channel")
    {
        EncounterCoordinator coord{};
        coord.UpdatePlayerCombatReport(1u, true, 2u, 8.0f, false, 0.0f, 0.0f, 0.0f, 900u);
        coord.UpdatePlayerCombatReport(2u, true, 2u, 5.0f, false, RfabCompat::MetersToSkyrimUnits(18.0f), 0.0f, 0.0f, 900u);

        coord.SetPlayerDowned(2u, true, RfabCompat::MetersToSkyrimUnits(18.0f), 0.0f, 0.0f, 2u, 1000u);
        REQUIRE(coord.IsPlayerDowned(2u));

        // Even if a melee ally started a 4s E-channel at close range, instant spell revive completes and clears the channel
        coord.UpdatePlayerCombatReport(3u, true, 2u, 4.0f, false, RfabCompat::MetersToSkyrimUnits(19.5f), 0.0f, 0.0f, 1050u);
        REQUIRE(coord.TryStartAllyRevive(3u, 2u, 1100u));

        uint32_t actualReviver = 0u;
        REQUIRE(coord.TryInstantSpellRevive(1u, 2u, actualReviver));
        REQUIRE(actualReviver == 1u);
        REQUIRE_FALSE(coord.IsPlayerDowned(2u));

        const auto tick = coord.Tick(6000u);
        REQUIRE(tick.CompletedAllyRevives.empty());
        REQUIRE(tick.CancelledAllyRevives.empty());
    }

    SECTION("Downed player hit by AoE/projectile heal resolves nearest standing ally within 45m and rejects out-of-range / all-downed")
    {
        EncounterCoordinator coord{};
        coord.UpdatePlayerCombatReport(10u, true, 1u, 20.0f, false, RfabCompat::MetersToSkyrimUnits(60.0f), 0.0f, 0.0f, 900u); // too far (>45m)
        coord.UpdatePlayerCombatReport(20u, true, 1u, 3.0f, false, 0.0f, 0.0f, 0.0f, 900u);
        coord.SetPlayerDowned(20u, true, 0.0f, 0.0f, 0.0f, 1u, 1000u);

        uint32_t reviver = 0u;
        REQUIRE_FALSE(coord.TryInstantSpellRevive(20u, 20u, reviver));
        REQUIRE_FALSE(coord.TryInstantSpellRevive(10u, 20u, reviver));
        REQUIRE(coord.IsPlayerDowned(20u));

        // Move healer within 30m: downed recipient self-report now resolves standing healer 10u
        coord.UpdatePlayerCombatReport(10u, true, 1u, 12.0f, false, RfabCompat::MetersToSkyrimUnits(30.0f), 0.0f, 0.0f, 1100u);
        REQUIRE(coord.TryInstantSpellRevive(20u, 20u, reviver));
        REQUIRE(reviver == 10u);
        REQUIRE_FALSE(coord.IsPlayerDowned(20u));
    }

    SECTION("ReviveActionRequest, MagicService, and EncounterService wire HealingSpellRevive end-to-end")
    {
        const auto root = LocateRepoRootPhase2();
        const std::string reqH = ReadRepoFilePhase2(root, "Code/encoding/Messages/ReviveActionRequest.h");
        const std::string magicCpp = ReadRepoFilePhase2(root, "Code/client/Services/Generic/MagicService.cpp");
        const std::string encCpp = ReadRepoFilePhase2(root, "Code/server/Services/EncounterService.cpp");

        REQUIRE(reqH.find("HealingSpellRevive = 2") != std::string::npos);
        REQUIRE(magicCpp.find("RequestAllySpellReviveOnActor(acEvent.TargetID, acEvent.Magnitude)") != std::string::npos);
        REQUIRE(magicCpp.find("NotifyAllyHealingSpellReceivedFromRemote(pCaster, acMessage.Magnitude)") != std::string::npos);
        REQUIRE(encCpp.find("ReviveActionRequest::ActionType::HealingSpellRevive") != std::string::npos);
        REQUIRE(encCpp.find("m_coordinator.TryInstantSpellRevive(") != std::string::npos);

        ReviveActionRequest out{};
        out.Action = ReviveActionRequest::ActionType::HealingSpellRevive;
        out.TargetPlayerId = 42u;
        TiltedPhoques::Buffer buf(256);
        TiltedPhoques::Buffer::Writer writer(&buf);
        out.SerializeRaw(writer);
        TiltedPhoques::Buffer::Reader reader(&buf);
        ReviveActionRequest in{};
        in.DeserializeRaw(reader);
        REQUIRE(in.Action == ReviveActionRequest::ActionType::HealingSpellRevive);
        REQUIRE(in.TargetPlayerId == 42u);
    }
}

TEST_CASE("RFAB v3.0 Phase 2.3: Sticky Host Cell Authority & deterministic Auto-Party convergence", "[rfab][v300][phase2][authority]")
{
    SECTION("Calm PartyLeader bypasses MinVoluntaryHoldMs on cell entry while active combat & non-leaders stay protected")
    {
        CoopOwnership::PolicyConfig cfg{};
        cfg.MinVoluntaryHoldMs = 1500;

        // Faster-loading Guest (player 2) claimed calm dungeon NPC at t=1000ms; Host (player 1) enters cell at t=1200ms:
        // Host immediately claims calm NPC without getting stuck behind 1500ms hold!
        REQUIRE(CoopOwnership::EvaluateVoluntaryClaim(
                    2u, 1u, 1000u, 1200u,
                    /*isPlayer=*/false,
                    /*isSummon=*/false,
                    /*ownerInRange=*/true,
                    /*ownerDowned=*/false,
                    /*challengerDowned=*/false,
                    cfg,
                    /*challengerIsCalmPartyLeader=*/true) == CoopOwnership::ClaimDecision::kAccept);

        // Non-leader (or combat NPC where challengerIsCalmPartyLeader == false) is still blocked by 1500ms hold window
        REQUIRE(CoopOwnership::EvaluateVoluntaryClaim(
                    1u, 2u, 1200u, 1400u,
                    /*isPlayer=*/false,
                    /*isSummon=*/false,
                    /*ownerInRange=*/true,
                    /*ownerDowned=*/false,
                    /*challengerDowned=*/false,
                    cfg,
                    /*challengerIsCalmPartyLeader=*/false) == CoopOwnership::ClaimDecision::kRejectHoldWindowActive);

        // Downed PartyLeader must NEVER steal simulation authority from an in-range standing owner
        REQUIRE(CoopOwnership::EvaluateVoluntaryClaim(
                    2u, 1u, 1000u, 1200u,
                    /*isPlayer=*/false,
                    /*isSummon=*/false,
                    /*ownerInRange=*/true,
                    /*ownerDowned=*/false,
                    /*challengerDowned=*/true,
                    cfg,
                    /*challengerIsCalmPartyLeader=*/true) == CoopOwnership::ClaimDecision::kRejectChallengerDowned);

        // Player avatars and player summons remain pinned even against calm PartyLeader
        REQUIRE(CoopOwnership::EvaluateVoluntaryClaim(
                    2u, 1u, 1000u, 5000u, true, false, true, false, false, cfg, true) ==
                CoopOwnership::ClaimDecision::kRejectPlayerAvatar);
        REQUIRE(CoopOwnership::EvaluateVoluntaryClaim(
                    2u, 1u, 1000u, 5000u, false, true, true, false, false, cfg, true) ==
                CoopOwnership::ClaimDecision::kRejectSummonPinned);
    }

    SECTION("Server CharacterService & PartyService enforce Sticky Host Cell Authority and primary party auto-join")
    {
        const auto root = LocateRepoRootPhase2();
        const std::string srvCharCpp = ReadRepoFilePhase2(root, "Code/server/Services/CharacterService.cpp");
        const std::string partyCpp = ReadRepoFilePhase2(root, "Code/server/Services/PartyService.cpp");

        // Auto-join converges into primary active party even if m_parties.size() > 1
        REQUIRE(partyCpp.find("else if (!m_parties.empty() && !acEvent.pPlayer->GetParty().JoinedPartyId.has_value())") != std::string::npos);
        REQUIRE(partyCpp.find("primaryPartyId") != std::string::npos);
        REQUIRE(partyCpp.find("m_parties.size() == 1") == std::string::npos);

        // ReconcileAllEntitiesForObserver transfers calm placed NPCs to entering standing PartyLeader
        REQUIRE(srvCharCpp.find("Sticky Host Cell Authority") != std::string::npos);
        REQUIRE(srvCharCpp.find("isCalmPartyLeader") != std::string::npos);
        REQUIRE(srvCharCpp.find("challengerIsCalmPartyLeader") != std::string::npos);
    }
}
