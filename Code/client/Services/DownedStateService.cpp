#include <TiltedOnlinePCH.h>

#include <Services/DownedStateService.h>

#include <World.h>
#include <Utils.h>
#include <common/RfabModpackCompat.h>
#include <Components.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/HealthChangeEvent.h>
#include <Events/UpdateEvent.h>
#include <Forms/ActorValueInfo.h>
#include <Games/ActorExtension.h>
#include <Games/Overrides.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Messages/CombatEncounterStateReport.h>
#include <Messages/NotifyPlayerDownedState.h>
#include <Messages/NotifyReviveState.h>
#include <Messages/ReviveActionRequest.h>
#include <Messages/SyncPlayerDownedState.h>
#include <Misc/ActorValueOwner.h>
#include <AI/Movement/PlayerControls.h>
#include <Camera/PlayerCamera.h>
#include <PlayerCharacter.h>
#include <Services/TransportService.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
constexpr float kDownedFloorHealth = 1.0f;
constexpr float kClientRevivePromptMeters = 3.0f;
constexpr float kHostileScanRadiusMeters = 55.0f;
constexpr float kRfabBossMaxHealthThreshold = 1200.0f;

bool IsHoldReviveKeyPressed() noexcept
{
#ifdef _WIN32
    if (auto* pUi = UI::Get())
    {
        if (pUi->GetMenuOpen(BSFixedString("Console")) ||
            pUi->GetMenuOpen(BSFixedString("Journal Menu")) ||
            pUi->GetMenuOpen(BSFixedString("InventoryMenu")) ||
            pUi->GetMenuOpen(BSFixedString("MagicMenu")) ||
            pUi->GetMenuOpen(BSFixedString("Loading Menu")))
        {
            return false;
        }
    }
    return (GetAsyncKeyState('E') & 0x8000) != 0;
#else
    return false;
#endif
}
} // namespace

DownedStateService::DownedStateService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
    , m_connectedConnection(aDispatcher.sink<ConnectedEvent>().connect<&DownedStateService::OnConnected>(this))
    , m_disconnectedConnection(aDispatcher.sink<DisconnectedEvent>().connect<&DownedStateService::OnDisconnected>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&DownedStateService::OnUpdate>(this))
    , m_notifyDownedConnection(aDispatcher.sink<NotifyPlayerDownedState>().connect<&DownedStateService::OnNotifyPlayerDownedState>(this))
    , m_notifyReviveConnection(aDispatcher.sink<NotifyReviveState>().connect<&DownedStateService::OnNotifyReviveState>(this))
{
}

bool DownedStateService::IsSystemActive() const noexcept
{
    return m_transport.IsConnected() && m_world.GetServerSettings().DownedSystemEnabled;
}

bool DownedStateService::ShouldBlockPlayerHealthMutation(float aDelta) const noexcept
{
    if (!IsSystemActive())
        return false;

    if (m_allowAuthorizedHeal)
        return false;

    if (m_isLocalDowned)
    {
        // Freeze BOTH positive regen/potions/spells AND negative poison/bleed/ground-fire ticks while on knee.
        return aDelta != 0.0f;
    }

    if (m_graceRemainingSec > 0.0 && aDelta < 0.0f)
        return true;

    return false;
}

bool DownedStateService::IsRemotePlayerDowned(uint32_t aPlayerId) const noexcept
{
    const auto it = m_remoteDownedByPlayerId.find(aPlayerId);
    return it != m_remoteDownedByPlayerId.end() && it->second;
}

bool DownedStateService::InterceptPotentialFatalPlayerDamage(
    Actor* apPlayer,
    float aCurrentHealth,
    float aIncomingDamage) noexcept
{
    if (!IsSystemActive() || !apPlayer)
        return false;

    if (m_isLocalDowned || m_graceRemainingSec > 0.0)
        return true;

    if (aIncomingDamage <= 0.0f)
        return false;

    if ((aCurrentHealth - aIncomingDamage) > kDownedFloorHealth)
        return false;

    // Clamp player health safely down to 1.0f under ScopedReviveHealAllowance so Skyrim never triggers Actor::Kill
    // or REQ_DeathHandler, then transition immediately into frozen Downed-on-Knee state.
    FinalizeLethalHitCushion(apPlayer, aCurrentHealth);
    return true;
}

void DownedStateService::FinalizeLethalHitCushion(Actor* apPlayer, float aHealthBefore) noexcept
{
    if (!apPlayer)
        return;

    {
        ScopedReviveHealAllowance allow(m_allowAuthorizedHeal);
        const float healthNow = apPlayer->GetActorValue(ActorValueInfo::kHealth);
        const float clampDelta = kDownedFloorHealth - healthNow;
        if (std::fabs(clampDelta) > 1e-4f)
            apPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, clampDelta);

        const float netDelta = apPlayer->GetActorValue(ActorValueInfo::kHealth) - aHealthBefore;
        if (std::fabs(netDelta) > 1e-4f)
            m_world.GetRunner().Trigger(HealthChangeEvent(apPlayer->formID, netDelta));
    }

    EnterLocalDownedState();
}

void DownedStateService::ApplyRemoteActorDownedPosture(Actor* apRemoteActor, bool aIsDowned) noexcept
{
    if (!apRemoteActor)
        return;

    apRemoteActor->InterruptCast(false);
    if (aIsDowned)
        apRemoteActor->SetWeaponDrawnEx(false);

    apRemoteActor->SetNoBleedoutRecovery(aIsDowned);
    BSFixedString animEventLower(aIsDowned ? "bleedoutStart" : "bleedoutStop");
    apRemoteActor->SendAnimationEvent(&animEventLower);
    BSFixedString animEventPascal(aIsDowned ? "BleedoutStart" : "BleedoutStop");
    apRemoteActor->SendAnimationEvent(&animEventPascal);
}

void DownedStateService::SetDownedControlLock(bool aLocked) noexcept
{
    PlayerControls* pControls = PlayerControls::GetInstance();
    if (!pControls)
        return;

    if (aLocked)
    {
        if (!m_savedControls.Valid)
        {
            m_savedControls.Movement = pControls->pMovementHandler ? pControls->pMovementHandler->isEnabled : true;
            m_savedControls.Sprint = pControls->pSprintHandler ? pControls->pSprintHandler->isEnabled : true;
            m_savedControls.ReadyWeapon = pControls->pReadyWeaponHandler ? pControls->pReadyWeaponHandler->isEnabled : true;
            m_savedControls.AutoMove = pControls->pAutoMoveHandler ? pControls->pAutoMoveHandler->isEnabled : true;
            m_savedControls.Activate = pControls->pActivateHandler ? pControls->pActivateHandler->isEnabled : true;
            m_savedControls.Jump = pControls->pJumpHandler ? pControls->pJumpHandler->isEnabled : true;
            m_savedControls.Shout = pControls->shoutHandler ? pControls->shoutHandler->isEnabled : true;
            m_savedControls.AttackBlock = pControls->attackBlockHandler ? pControls->attackBlockHandler->isEnabled : true;
            m_savedControls.Sneak = pControls->sneakHandler ? pControls->sneakHandler->isEnabled : true;
            m_savedControls.TogglePOV = pControls->togglePOVHandler ? pControls->togglePOVHandler->isEnabled : true;
            m_savedControls.Valid = true;
        }

        // Lock all locomotion, attacks, magic/shouts, jumping, sprinting, item activation, and 1st-person toggle
        // while keeping pLookHandler open so the kneeling player can look around in 3rd-person while waiting for revive.
        pControls->Data.MoveInputVec.x = 0.f;
        pControls->Data.MoveInputVec.y = 0.f;
        pControls->Data.PrevMoveVec.x = 0.f;
        pControls->Data.PrevMoveVec.y = 0.f;
        pControls->Data.bAutoMove = false;
        pControls->Data.bRunning = false;
        if (pControls->pMovementHandler)
            pControls->pMovementHandler->isEnabled = false;
        if (pControls->pSprintHandler)
            pControls->pSprintHandler->isEnabled = false;
        if (pControls->pReadyWeaponHandler)
            pControls->pReadyWeaponHandler->isEnabled = false;
        if (pControls->pAutoMoveHandler)
            pControls->pAutoMoveHandler->isEnabled = false;
        if (pControls->pActivateHandler)
            pControls->pActivateHandler->isEnabled = false;
        if (pControls->pJumpHandler)
            pControls->pJumpHandler->isEnabled = false;
        if (pControls->shoutHandler)
            pControls->shoutHandler->isEnabled = false;
        if (pControls->attackBlockHandler)
            pControls->attackBlockHandler->isEnabled = false;
        if (pControls->sneakHandler)
            pControls->sneakHandler->isEnabled = false;
        if (pControls->togglePOVHandler)
            pControls->togglePOVHandler->isEnabled = false;
    }
    else if (m_savedControls.Valid)
    {
        if (pControls->pMovementHandler)
            pControls->pMovementHandler->isEnabled = m_savedControls.Movement;
        if (pControls->pSprintHandler)
            pControls->pSprintHandler->isEnabled = m_savedControls.Sprint;
        if (pControls->pReadyWeaponHandler)
            pControls->pReadyWeaponHandler->isEnabled = m_savedControls.ReadyWeapon;
        if (pControls->pAutoMoveHandler)
            pControls->pAutoMoveHandler->isEnabled = m_savedControls.AutoMove;
        if (pControls->pActivateHandler)
            pControls->pActivateHandler->isEnabled = m_savedControls.Activate;
        if (pControls->pJumpHandler)
            pControls->pJumpHandler->isEnabled = m_savedControls.Jump;
        if (pControls->shoutHandler)
            pControls->shoutHandler->isEnabled = m_savedControls.Shout;
        if (pControls->attackBlockHandler)
            pControls->attackBlockHandler->isEnabled = m_savedControls.AttackBlock;
        if (pControls->sneakHandler)
            pControls->sneakHandler->isEnabled = m_savedControls.Sneak;
        if (pControls->togglePOVHandler)
            pControls->togglePOVHandler->isEnabled = m_savedControls.TogglePOV;
        m_savedControls = {};
    }
}

void DownedStateService::EnterLocalDownedState() noexcept
{
    if (m_isLocalDowned)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    m_isLocalDowned = true;
    m_isChannelingRevive = false;
    m_channelTargetPlayerId = 0;
    m_bleedoutReassertTimerSec = 0.0;

    // 1st-person skeleton in Skyrim SE has no bleedout pose; switch cleanly to 3rd-person so the player visibly
    // sees their character drop to one knee, and restore 1st-person automatically upon revive.
    m_wasFirstPersonBeforeDowned = false;
    if (auto* pCam = PlayerCamera::Get(); pCam && pCam->IsFirstPerson())
    {
        m_wasFirstPersonBeforeDowned = true;
        pCam->ForceThirdPerson();
    }

    pPlayer->InterruptCast(false);
    pPlayer->SetWeaponDrawnEx(false);
    pPlayer->SetNoBleedoutRecovery(true);
    pPlayer->StopCombat();
    SetDownedControlLock(true);

    BSFixedString bleedoutLower("bleedoutStart");
    pPlayer->SendAnimationEvent(&bleedoutLower);
    BSFixedString bleedoutStart("BleedoutStart");
    pPlayer->SendAnimationEvent(&bleedoutStart);

    RedirectHostileAggroAwayFromPlayer(pPlayer);

    uint32_t nearbyHostiles = 0;
    const float maxUnits = RfabCompat::MetersToSkyrimUnits(kHostileScanRadiusMeters);
    const float maxUnitsSq = maxUnits * maxUnits;

    auto view = m_world.view<FormIdComponent>();
    for (auto entity : view)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pActor || pActor == pPlayer || pActor->IsDead() || pActor->IsPlayerTeammate())
            continue;
        if (auto* pExt = pActor->GetExtension(); pExt && pExt->IsPlayer())
            continue;
        if (!pActor->IsInCombat())
            continue;

        const float dx = pActor->position.x - pPlayer->position.x;
        const float dy = pActor->position.y - pPlayer->position.y;
        const float dz = pActor->position.z - pPlayer->position.z;
        if ((dx * dx + dy * dy + dz * dz) <= maxUnitsSq)
            ++nearbyHostiles;
    }

    SyncPlayerDownedState pkt{};
    pkt.IsDowned = true;
    pkt.PositionX = pPlayer->position.x;
    pkt.PositionY = pPlayer->position.y;
    pkt.PositionZ = pPlayer->position.z;
    pkt.NearbyHostileCount = nearbyHostiles;
    m_transport.Send(pkt);

    Utils::ShowHudMessage(String("Вы тяжело ранены (0 HP) — ждите помощи союзника [E] или конца боя"));
}

void DownedStateService::ApplyAuthoritativeRevive(
    float aHealthFraction,
    float aGraceSec,
    bool aShowPostCombatNotice) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    m_isLocalDowned = false;
    m_graceRemainingSec = std::clamp(static_cast<double>(aGraceSec), 0.0, 15.0);

    {
        ScopedReviveHealAllowance allow(m_allowAuthorizedHeal);
        const float maxHp = std::max(
            10.0f,
            pPlayer->GetActorPermanentValue(ActorValueInfo::kHealth) + pPlayer->healthModifiers.temporaryModifier);
        const float curHp = pPlayer->GetActorValue(ActorValueInfo::kHealth);
        const float targetHp = std::max(kDownedFloorHealth + 1.0f, maxHp * std::clamp(aHealthFraction, 0.05f, 1.0f));
        const float healDelta = std::max(0.0f, targetHp - curHp);

        if (healDelta > 0.0f)
        {
            pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, healDelta);
            m_world.GetRunner().Trigger(HealthChangeEvent(pPlayer->formID, healDelta));
        }
    }

    if (aShowPostCombatNotice)
        m_encounterHostileFormIds.clear();

    pPlayer->SetNoBleedoutRecovery(false);
    SetDownedControlLock(false);

    BSFixedString bleedoutStopLower("bleedoutStop");
    pPlayer->SendAnimationEvent(&bleedoutStopLower);
    BSFixedString bleedoutStop("BleedoutStop");
    pPlayer->SendAnimationEvent(&bleedoutStop);

    if (m_wasFirstPersonBeforeDowned)
    {
        m_wasFirstPersonBeforeDowned = false;
        if (auto* pCam = PlayerCamera::Get())
            pCam->ForceFirstPerson();
    }

    SyncPlayerDownedState syncUp{};
    syncUp.IsDowned = false;
    syncUp.PositionX = pPlayer->position.x;
    syncUp.PositionY = pPlayer->position.y;
    syncUp.PositionZ = pPlayer->position.z;
    m_transport.Send(syncUp);

    Utils::ShowHudMessage(
        aShowPostCombatNotice
            ? String("Победа в бою — второе дыхание (30% HP)")
            : String("Союзник поднял вас на ноги (30% HP)"));
}

void DownedStateService::ResetAllStates() noexcept
{
    const bool wasLocalDowned = m_isLocalDowned;
    const bool restoreFirstPerson = m_wasFirstPersonBeforeDowned;

    m_isLocalDowned = false;
    m_allowAuthorizedHeal = false;
    m_wasFirstPersonBeforeDowned = false;
    m_graceRemainingSec = 0.0;
    m_bleedoutReassertTimerSec = 0.0;
    m_revivePromptCooldownSec = 0.0;
    m_isChannelingRevive = false;
    m_channelTargetPlayerId = 0;
    m_channelElapsedSec = 0.0;
    m_bossDefeatedPendingReport = false;
    m_encounterHostileFormIds.clear();
    m_liveBossesSeenInCombat.clear();
    m_remoteDownedByPlayerId.clear();

    SetDownedControlLock(false);

    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
    {
        pPlayer->SetNoBleedoutRecovery(false);
        if (wasLocalDowned)
        {
            BSFixedString bleedoutStopLower("bleedoutStop");
            pPlayer->SendAnimationEvent(&bleedoutStopLower);
            BSFixedString bleedoutStop("BleedoutStop");
            pPlayer->SendAnimationEvent(&bleedoutStop);
            if (restoreFirstPerson)
            {
                if (auto* pCam = PlayerCamera::Get())
                    pCam->ForceFirstPerson();
            }
        }
    }
}

void DownedStateService::OnConnected(const ConnectedEvent&) noexcept
{
    ResetAllStates();
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
        pPlayer->SetPlayerRespawnMode(true);
}

void DownedStateService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    ResetAllStates();
}

void DownedStateService::RedirectHostileAggroAwayFromPlayer(Actor* apLocalPlayer) noexcept
{
    if (!apLocalPlayer)
        return;

    Actor* pNearestLivingAlly = nullptr;
    float bestAllyDistSq = std::numeric_limits<float>::max();

    auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : playerView)
    {
        const auto& playerComp = playerView.get<PlayerComponent>(entity);
        if (IsRemotePlayerDowned(playerComp.Id))
            continue;

        Actor* pAlly = Cast<Actor>(TESForm::GetById(playerView.get<FormIdComponent>(entity).Id));
        if (!pAlly || pAlly == apLocalPlayer || pAlly->IsDead())
            continue;

        const float dx = pAlly->position.x - apLocalPlayer->position.x;
        const float dy = pAlly->position.y - apLocalPlayer->position.y;
        const float dz = pAlly->position.z - apLocalPlayer->position.z;
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < bestAllyDistSq)
        {
            bestAllyDistSq = distSq;
            pNearestLivingAlly = pAlly;
        }
    }

    auto actorView = m_world.view<FormIdComponent>();
    for (auto entity : actorView)
    {
        Actor* pNpc = Cast<Actor>(TESForm::GetById(actorView.get<FormIdComponent>(entity).Id));
        if (!pNpc || pNpc == apLocalPlayer || pNpc->IsDead())
            continue;
        if (auto* pExt = pNpc->GetExtension(); pExt && pExt->IsPlayer())
            continue;

        if (pNpc->GetCombatTarget() == apLocalPlayer)
        {
            pNpc->StopCombat();
            if (pNearestLivingAlly)
                pNpc->StartCombatEx(pNearestLivingAlly);
        }
    }
}

uint32_t DownedStateService::GetLocalPlayerId() const noexcept
{
    const PlayerCharacter* pLocal = PlayerCharacter::Get();
    if (!pLocal)
        return 0;

    auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : playerView)
    {
        if (playerView.get<FormIdComponent>(entity).Id == pLocal->formID)
            return playerView.get<PlayerComponent>(entity).Id;
    }
    return 0;
}

void DownedStateService::UpdateInteractiveReviveInput(double aDeltaSec) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || m_isLocalDowned)
        return;

    const float maxUnits = RfabCompat::MetersToSkyrimUnits(kClientRevivePromptMeters);
    const float maxUnitsSq = maxUnits * maxUnits;

    uint32_t nearestDownedPlayerId = 0;
    float bestDistSq = maxUnitsSq;

    auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : playerView)
    {
        Actor* pAlly = Cast<Actor>(TESForm::GetById(playerView.get<FormIdComponent>(entity).Id));
        if (!pAlly || pAlly == pPlayer)
            continue;

        const uint32_t pid = playerView.get<PlayerComponent>(entity).Id;
        if (!IsRemotePlayerDowned(pid))
            continue;

        const float dx = pAlly->position.x - pPlayer->position.x;
        const float dy = pAlly->position.y - pPlayer->position.y;
        const float dz = pAlly->position.z - pPlayer->position.z;
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq <= bestDistSq)
        {
            bestDistSq = distSq;
            nearestDownedPlayerId = pid;
        }
    }

    if (m_revivePromptCooldownSec > 0.0)
        m_revivePromptCooldownSec = std::max(0.0, m_revivePromptCooldownSec - aDeltaSec);

    const bool keyHeld = IsHoldReviveKeyPressed();
    if (nearestDownedPlayerId != 0 && !keyHeld && m_revivePromptCooldownSec <= 0.0)
    {
        m_revivePromptCooldownSec = 6.0;
        Utils::ShowHudMessage(String("Рядом раненый союзник — зажмите [E], чтобы поднять на ноги"));
    }

    if (nearestDownedPlayerId != 0 && keyHeld)
    {
        if (!m_isChannelingRevive || m_channelTargetPlayerId != nearestDownedPlayerId)
        {
            m_isChannelingRevive = true;
            m_channelTargetPlayerId = nearestDownedPlayerId;
            m_channelElapsedSec = 0.0;
            m_channelDurationSec = m_world.GetServerSettings().ReviveDurationSec;

            ReviveActionRequest req{};
            req.Action = ReviveActionRequest::ActionType::StartChannel;
            req.TargetPlayerId = nearestDownedPlayerId;
            m_transport.Send(req);

            Utils::ShowHudMessage(String("Реанимация союзника... удерживайте [E]"));
        }
        else
        {
            m_channelElapsedSec += aDeltaSec;
        }
    }
    else if (m_isChannelingRevive)
    {
        ReviveActionRequest cancelReq{};
        cancelReq.Action = ReviveActionRequest::ActionType::CancelChannel;
        cancelReq.TargetPlayerId = m_channelTargetPlayerId;
        m_transport.Send(cancelReq);

        m_isChannelingRevive = false;
        m_channelTargetPlayerId = 0;
        m_channelElapsedSec = 0.0;
    }
}

void DownedStateService::SendCombatEncounterReport() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    uint32_t nearbyHostiles = 0;
    float nearestUnitsSq = std::numeric_limits<float>::max();
    const float maxUnits = RfabCompat::MetersToSkyrimUnits(kHostileScanRadiusMeters);
    const float maxUnitsSq = maxUnits * maxUnits;
    const float leashUnits = RfabCompat::MetersToSkyrimUnits(m_world.GetServerSettings().CombatLeashRadiusMeters);
    const float leashUnitsSq = leashUnits * leashUnits;

    auto view = m_world.view<FormIdComponent>();
    for (auto entity : view)
    {
        Actor* pNpc = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!pNpc || pNpc == pPlayer || pNpc->IsPlayerTeammate())
            continue;
        if (auto* pExt = pNpc->GetExtension(); pExt && pExt->IsPlayer())
            continue;

        const bool isBossTier = pNpc->IsDragon() ||
            (pNpc->GetActorPermanentValue(ActorValueInfo::kHealth) >= kRfabBossMaxHealthThreshold);

        if (pNpc->IsDead())
        {
            m_encounterHostileFormIds.erase(pNpc->formID);
            if (isBossTier && m_liveBossesSeenInCombat.erase(pNpc->formID) > 0)
                m_bossDefeatedPendingReport = true;
            continue;
        }

        const float dx = pNpc->position.x - pPlayer->position.x;
        const float dy = pNpc->position.y - pPlayer->position.y;
        const float dz = pNpc->position.z - pPlayer->position.z;
        const float distSq = dx * dx + dy * dy + dz * dz;

        if (pNpc->IsInCombat())
        {
            m_encounterHostileFormIds.insert(pNpc->formID);
            if (isBossTier)
                m_liveBossesSeenInCombat.insert(pNpc->formID);

            if (distSq <= maxUnitsSq)
            {
                ++nearbyHostiles;
                nearestUnitsSq = std::min(nearestUnitsSq, distSq);
            }
            continue;
        }

        // Prevent RFAB invisibility potion / stealth / boss ethereal-phase or StopCombat() aggro-drop exploits:
        // any hostile who engaged in this encounter stays unresolved until actually dead (pNpc->IsDead()) or beyond 120m leash!
        if (m_encounterHostileFormIds.contains(pNpc->formID))
        {
            if (distSq <= leashUnitsSq)
            {
                ++nearbyHostiles;
                nearestUnitsSq = std::min(nearestUnitsSq, distSq);
            }
            else
            {
                m_encounterHostileFormIds.erase(pNpc->formID);
            }
        }
    }

    CombatEncounterStateReport report{};
    report.InCombat = (!m_isLocalDowned && pPlayer->IsInCombat()) || nearbyHostiles > 0;
    report.NearbyHostileCount = nearbyHostiles;
    report.NearestHostileDistanceMeters = (nearbyHostiles > 0)
        ? RfabCompat::SkyrimUnitsToMeters(std::sqrt(nearestUnitsSq))
        : 9999.0f;
    report.BossDefeatedInEncounter = m_bossDefeatedPendingReport;
    report.PositionX = pPlayer->position.x;
    report.PositionY = pPlayer->position.y;
    report.PositionZ = pPlayer->position.z;

    m_bossDefeatedPendingReport = false;
    m_transport.Send(report);
}

void DownedStateService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    if (!IsSystemActive())
        return;

    if (m_graceRemainingSec > 0.0)
        m_graceRemainingSec = std::max(0.0, m_graceRemainingSec - acEvent.Delta);

    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    // Belt-and-suspenders per-frame health guard: even if an external SKSE plugin mutates healthModifiers
    // directly in memory without calling DamageActor / RestoreActorValue, clamp HP to 1.0f immediately
    // and transition into frozen Downed-on-Knee posture before negative HP (-50..-500) can ever stick.
    if (pPlayer)
    {
        const float curHp = pPlayer->GetActorValue(ActorValueInfo::kHealth);
        if (!m_isLocalDowned && m_graceRemainingSec <= 0.0 && curHp <= kDownedFloorHealth)
        {
            FinalizeLethalHitCushion(pPlayer, curHp);
        }
        else if (m_isLocalDowned && std::fabs(curHp - kDownedFloorHealth) > 0.05f)
        {
            ScopedReviveHealAllowance allow(m_allowAuthorizedHeal);
            pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, kDownedFloorHealth - curHp);
        }
    }

    if (m_isLocalDowned)
    {
        SetDownedControlLock(true);
        if (pPlayer)
        {
            if (auto* pCam = PlayerCamera::Get(); pCam && pCam->IsFirstPerson())
                pCam->ForceThirdPerson();

            RedirectHostileAggroAwayFromPlayer(pPlayer);
            m_bleedoutReassertTimerSec += acEvent.Delta;
            if (m_bleedoutReassertTimerSec >= 0.85)
            {
                m_bleedoutReassertTimerSec = 0.0;
                pPlayer->SetWeaponDrawnEx(false);
                BSFixedString bleedoutLower("bleedoutStart");
                pPlayer->SendAnimationEvent(&bleedoutLower);
                BSFixedString bleedoutStart("BleedoutStart");
                pPlayer->SendAnimationEvent(&bleedoutStart);
            }
        }
    }
    else
    {
        UpdateInteractiveReviveInput(acEvent.Delta);
    }

    m_reportTimerSec += acEvent.Delta;
    if (m_reportTimerSec >= 0.25)
    {
        m_reportTimerSec = 0.0;
        SendCombatEncounterReport();
    }
}

void DownedStateService::OnNotifyPlayerDownedState(const NotifyPlayerDownedState& acMessage) noexcept
{
    m_remoteDownedByPlayerId[acMessage.PlayerId] = acMessage.IsDowned;

    auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : playerView)
    {
        if (playerView.get<PlayerComponent>(entity).Id != acMessage.PlayerId)
            continue;

        if (Actor* pRemoteActor = Cast<Actor>(TESForm::GetById(playerView.get<FormIdComponent>(entity).Id)))
            ApplyRemoteActorDownedPosture(pRemoteActor, acMessage.IsDowned);
        break;
    }

    if (acMessage.IsDowned)
        Utils::ShowHudMessage(String("Союзник тяжело ранен! Удерживайте [E] рядом, чтобы поднять"));
}

void DownedStateService::OnNotifyReviveState(const NotifyReviveState& acMessage) noexcept
{
    const uint32_t localId = GetLocalPlayerId();
    const bool targetsLocalPlayer = (localId == 0 || acMessage.TargetPlayerId == localId);

    switch (acMessage.Status)
    {
    case NotifyReviveState::ReviveStatus::ChannelStarted:
        if (m_isLocalDowned && targetsLocalPlayer)
            Utils::ShowHudMessage(String("Союзник поднимает вас..."));
        break;

    case NotifyReviveState::ReviveStatus::ChannelCancelled:
        if (m_isChannelingRevive && m_channelTargetPlayerId == acMessage.TargetPlayerId)
        {
            m_isChannelingRevive = false;
            m_channelTargetPlayerId = 0;
        }
        break;

    case NotifyReviveState::ReviveStatus::RevivedByAlly:
        m_remoteDownedByPlayerId[acMessage.TargetPlayerId] = false;
        if (m_isChannelingRevive && m_channelTargetPlayerId == acMessage.TargetPlayerId)
        {
            m_isChannelingRevive = false;
            m_channelTargetPlayerId = 0;
            Utils::ShowHudMessage(String("Вы подняли союзника!"));
        }
        if (m_isLocalDowned && targetsLocalPlayer)
            ApplyAuthoritativeRevive(acMessage.HealthFraction, acMessage.GraceSec, false);
        break;

    case NotifyReviveState::ReviveStatus::RevivedPostCombat:
        m_remoteDownedByPlayerId[acMessage.TargetPlayerId] = false;
        if (m_isLocalDowned && targetsLocalPlayer)
            ApplyAuthoritativeRevive(acMessage.HealthFraction, acMessage.GraceSec, true);
        break;

    case NotifyReviveState::ReviveStatus::PartyWipeImminent:
        Utils::ShowHudMessage(String("Весь отряд пал — перезагрузка последнего чекпоинта..."));
        break;
    }
}
