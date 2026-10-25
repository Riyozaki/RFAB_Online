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
#include <Combat/CombatController.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <PlayerCharacter.h>
#include <Services/RfabAdapterService.h>
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

    if (m_allowAuthorizedHeal.load(std::memory_order_acquire))
        return false;

    if (m_isLocalDowned.load(std::memory_order_acquire))
    {
        // Freeze BOTH positive regen/potions/spells AND negative poison/bleed/ground-fire ticks while on knee.
        return aDelta != 0.0f;
    }

    if (m_isGraceActive.load(std::memory_order_acquire) && aDelta < 0.0f)
        return true;

    return false;
}

bool DownedStateService::IsRemotePlayerDowned(uint32_t aPlayerId) const noexcept
{
    const auto it = m_remoteDownedByPlayerId.find(aPlayerId);
    return it != m_remoteDownedByPlayerId.end() && it->second;
}

bool DownedStateService::IsActorDowned(uint32_t aActorFormId) const noexcept
{
    if (aActorFormId == 0)
        return false;
    if (aActorFormId == 0x14)
        return IsLocalPlayerDowned();

    if (Actor* pActor = Cast<Actor>(TESForm::GetById(aActorFormId)))
    {
        if (auto* pExt = pActor->GetExtension(); pExt && pExt->IsDownedPlayer())
            return true;
    }

    auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : playerView)
    {
        if (playerView.get<FormIdComponent>(entity).Id == aActorFormId)
            return IsRemotePlayerDowned(playerView.get<PlayerComponent>(entity).Id);
    }

    return false;
}

float DownedStateService::GetReviveChannelProgress() const noexcept
{
    if (!m_isChannelingRevive || m_channelDurationSec <= 0.05)
        return 0.0f;
    return static_cast<float>(std::clamp(m_channelElapsedSec / m_channelDurationSec, 0.0, 1.0));
}

bool DownedStateService::IsPlayerBeingRevived(uint32_t aPlayerId) const noexcept
{
    if (aPlayerId == 0)
        return false;
    if (m_isChannelingRevive && m_channelTargetPlayerId == aPlayerId)
        return true;
    return m_playersBeingRevived.contains(aPlayerId);
}

bool DownedStateService::InterceptPotentialFatalPlayerDamage(
    Actor* apPlayer,
    float aCurrentHealth,
    float aIncomingDamage) noexcept
{
    if (!IsSystemActive() || !apPlayer)
        return false;

    if (m_isLocalDowned.load(std::memory_order_acquire) || m_isGraceActive.load(std::memory_order_acquire))
        return true;

    if (aIncomingDamage <= 0.0f)
        return false;

    if ((aCurrentHealth - aIncomingDamage) > kDownedFloorHealth)
        return false;

    // Clamp player health safely down to 1.0f under ScopedReviveHealAllowance so Skyrim never triggers Actor::Kill
    // or REQ_DeathHandler, and arm atomic downed state; heavy camera/Papyrus/aggro/network side effects execute on OnUpdate.
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
        // Note: Actor::ForceActorValue takes the TARGET actor value (computing delta = target - current internally),
        // NOT a delta! Passing kDownedFloorHealth clamps HP directly to 1.0f without frame-to-frame sign oscillation.
        if (std::fabs(kDownedFloorHealth - healthNow) > 1e-4f)
            apPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, kDownedFloorHealth);

        const float netDelta = apPlayer->GetActorValue(ActorValueInfo::kHealth) - aHealthBefore;
        if (std::fabs(netDelta) > 1e-4f)
            m_world.GetRunner().Trigger(HealthChangeEvent(apPlayer->formID, netDelta));
    }

    // Do NOT invoke ForceThirdPerson, Papyrus StopCombat/StartCombat, entt views, or TransportService::Send inside
    // HookDamageActor / HookRestoreActorValue (which run mid-combat and on worker threads). Arm atomic downed state
    // immediately so all subsequent hits/regen AND AI worker detection/target hooks see IsDownedPlayer() right away!
    if (auto* pExt = apPlayer->GetExtension())
        pExt->SetDownedPlayer(true);

    bool expected = false;
    if (m_isLocalDowned.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        m_pendingEnterDownedTransition.store(true, std::memory_order_release);
}

void DownedStateService::ApplyRemoteActorDownedPosture(Actor* apRemoteActor, bool aIsDowned) noexcept
{
    if (!apRemoteActor)
        return;

    if (auto* pExt = apRemoteActor->GetExtension())
    {
        pExt->SetDownedPlayer(aIsDowned);
        if (!pExt->IsLocalPlayer())
        {
            constexpr uint32_t kLifeStateMask = 0xFu << 21;
            constexpr uint32_t kLifeStateBleedout = 8u << 21;
            if (aIsDowned)
            {
                apRemoteActor->actorState.flags1 = (apRemoteActor->actorState.flags1 & ~kLifeStateMask) | kLifeStateBleedout;
            }
            else if (((apRemoteActor->actorState.flags1 & kLifeStateMask) >> 21) == 8u)
            {
                apRemoteActor->actorState.flags1 &= ~kLifeStateMask;
            }
        }
    }

    apRemoteActor->InterruptCast(false);
    if (aIsDowned)
        apRemoteActor->SetWeaponDrawnEx(false);

    apRemoteActor->SetNoBleedoutRecovery(aIsDowned);
    BSFixedString animEventLower(aIsDowned ? "bleedoutStart" : "bleedoutStop");
    apRemoteActor->SendAnimationEvent(&animEventLower);
    BSFixedString animEventPascal(aIsDowned ? "BleedoutStart" : "BleedoutStop");
    apRemoteActor->SendAnimationEvent(&animEventPascal);

    m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
    if (aIsDowned)
        RedirectHostileAggroAwayFromActor(apRemoteActor);
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
    bool expected = false;
    if (!m_isLocalDowned.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    m_pendingEnterDownedTransition.store(false, std::memory_order_release);
    CompleteEnterLocalDownedState();
}

void DownedStateService::CompleteEnterLocalDownedState() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    m_isChannelingRevive = false;
    m_channelTargetPlayerId = 0;
    m_bleedoutReassertTimerSec = 0.0;

    if (auto* pExt = pPlayer->GetExtension())
        pExt->SetDownedPlayer(true);

    m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();

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

    RedirectHostileAggroAwayFromActor(pPlayer);

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

    m_pendingEnterDownedTransition.store(false, std::memory_order_release);
    m_isLocalDowned.store(false, std::memory_order_release);
    if (auto* pExt = pPlayer->GetExtension())
        pExt->SetDownedPlayer(false);
    m_graceRemainingSec = std::clamp(static_cast<double>(aGraceSec), 0.0, 15.0);
    m_isGraceActive.store(m_graceRemainingSec > 0.0, std::memory_order_release);
    m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();

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
            pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, targetHp);
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
    const bool wasLocalDowned = m_isLocalDowned.load(std::memory_order_acquire);
    const bool restoreFirstPerson = m_wasFirstPersonBeforeDowned;

    m_pendingEnterDownedTransition.store(false, std::memory_order_release);
    m_isLocalDowned.store(false, std::memory_order_release);
    m_allowAuthorizedHeal.store(false, std::memory_order_release);
    m_wasFirstPersonBeforeDowned = false;
    m_graceRemainingSec = 0.0;
    m_isGraceActive.store(false, std::memory_order_release);
    m_bleedoutReassertTimerSec = 0.0;
    m_revivePromptCooldownSec = 0.0;
    m_isChannelingRevive = false;
    m_channelTargetPlayerId = 0;
    m_channelElapsedSec = 0.0;
    m_bossDefeatedPendingReport = false;
    m_encounterHostileFormIds.clear();
    m_liveBossesSeenInCombat.clear();
    m_remoteDownedByPlayerId.clear();
    m_playersBeingRevived.clear();

    auto remotePlayerView = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : remotePlayerView)
    {
        const uint32_t fid = remotePlayerView.get<FormIdComponent>(entity).Id;
        if (fid == 0 || fid == 0x14)
            continue;
        if (Actor* pRemote = Cast<Actor>(TESForm::GetById(fid)))
        {
            if (auto* pExt = pRemote->GetExtension())
                pExt->SetDownedPlayer(false);
            constexpr uint32_t kLifeStateMask = 0xFu << 21;
            if (((pRemote->actorState.flags1 & kLifeStateMask) >> 21) == 8u)
                pRemote->actorState.flags1 &= ~kLifeStateMask;
        }
    }

    SetDownedControlLock(false);

    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
    {
        if (auto* pExt = pPlayer->GetExtension())
            pExt->SetDownedPlayer(false);
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

    m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();
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

void DownedStateService::RedirectHostileAggroAwayFromActor(Actor* apDownedActor) noexcept
{
    if (!apDownedActor)
        return;

    const uint32_t downedWorldId = apDownedActor->GetWorldSpace() ? apDownedActor->GetWorldSpace()->formID : 0u;
    const uint32_t downedCellId = apDownedActor->parentCell ? apDownedActor->parentCell->formID : 0u;

    auto isSameWorldOrCell = [&](Actor* apCandidate) noexcept
    {
        if (!apCandidate)
            return false;
        const uint32_t candWorldId = apCandidate->GetWorldSpace() ? apCandidate->GetWorldSpace()->formID : 0u;
        const uint32_t candCellId = apCandidate->parentCell ? apCandidate->parentCell->formID : 0u;
        if (downedWorldId != 0)
            return candWorldId == downedWorldId;
        return downedCellId == 0 || candCellId == downedCellId;
    };

    Actor* pNearestLivingAlly = nullptr;
    float bestAllyDistSq = std::numeric_limits<float>::max();

    auto considerStandingAlly = [&](Actor* apAlly) noexcept
    {
        if (!apAlly || apAlly == apDownedActor || apAlly->IsDead() || !isSameWorldOrCell(apAlly))
            return;
        if (auto* pExt = apAlly->GetExtension(); pExt && pExt->IsDownedPlayer())
            return;

        const float dx = apAlly->position.x - apDownedActor->position.x;
        const float dy = apAlly->position.y - apDownedActor->position.y;
        const float dz = apAlly->position.z - apDownedActor->position.z;
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < bestAllyDistSq)
        {
            bestAllyDistSq = distSq;
            pNearestLivingAlly = apAlly;
        }
    };

    if (!IsLocalPlayerDowned())
    {
        if (PlayerCharacter* pLocal = PlayerCharacter::Get())
            considerStandingAlly(pLocal);
    }

    auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : playerView)
    {
        const auto& playerComp = playerView.get<PlayerComponent>(entity);
        const uint32_t fid = playerView.get<FormIdComponent>(entity).Id;
        if (fid == 0x14)
        {
            if (IsLocalPlayerDowned())
                continue;
        }
        else if (IsRemotePlayerDowned(playerComp.Id))
        {
            continue;
        }

        Actor* pAlly = Cast<Actor>(TESForm::GetById(fid));
        considerStandingAlly(pAlly);
    }

    auto actorView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (auto entity : actorView)
    {
        Actor* pNpc = Cast<Actor>(TESForm::GetById(actorView.get<FormIdComponent>(entity).Id));
        if (!pNpc || pNpc == apDownedActor || pNpc->IsDead())
            continue;
        if (auto* pExt = pNpc->GetExtension(); pExt && pExt->IsPlayer())
            continue;

        Actor* pCurrentTarget = pNpc->GetCombatTarget();
        if (!pCurrentTarget && pNpc->pCombatController && pNpc->pCombatController->targetHandle != 0)
            pCurrentTarget = Cast<Actor>(TESObjectREFR::GetByHandle(pNpc->pCombatController->targetHandle));

        if (pCurrentTarget == apDownedActor)
        {
            // NEVER call StopCombat() + StartCombatEx() on a timer: that forced enemies out of combat and back in
            // every 0.5s (infinite taunt/sheath/draw loop!). Instead, switch CombatController::SetTarget directly
            // onto the nearest standing player (backed by HookUpdateDetectionState + HookUpdateTarget on AI workers).
            if (pNearestLivingAlly)
            {
                pNpc->SetCombatTargetEx(pNearestLivingAlly);
                if (!pNpc->IsInCombat())
                    pNpc->StartCombat(pNearestLivingAlly);
            }
            else
            {
                pNpc->SetCombatTargetEx(nullptr);
                if (pNpc->IsInCombat())
                    pNpc->StopCombat();
            }
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

        const bool isBossTier = m_world.GetRfabAdapterService().IsBossActor(pNpc) ||
            pNpc->IsDragon() ||
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
    {
        m_graceRemainingSec = std::max(0.0, m_graceRemainingSec - acEvent.Delta);
        m_isGraceActive.store(m_graceRemainingSec > 0.0, std::memory_order_release);
    }

    PlayerCharacter* pPlayer = PlayerCharacter::Get();

    // Belt-and-suspenders per-frame health guard: even if an external SKSE plugin mutates healthModifiers
    // directly in memory without calling DamageActor / RestoreActorValue, clamp HP to 1.0f immediately
    // and transition into frozen Downed-on-Knee posture before negative HP (-50..-500) can ever stick.
    if (pPlayer)
    {
        const float curHp = pPlayer->GetActorValue(ActorValueInfo::kHealth);
        if (!m_isLocalDowned.load(std::memory_order_acquire) && m_graceRemainingSec <= 0.0 && curHp <= kDownedFloorHealth)
        {
            FinalizeLethalHitCushion(pPlayer, curHp);
        }
        else if (m_isLocalDowned.load(std::memory_order_acquire) && std::fabs(curHp - kDownedFloorHealth) > 0.05f)
        {
            ScopedReviveHealAllowance allow(m_allowAuthorizedHeal);
            pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, kDownedFloorHealth);
        }
    }

    if (m_pendingEnterDownedTransition.exchange(false, std::memory_order_acq_rel))
    {
        if (m_isLocalDowned.load(std::memory_order_acquire))
            CompleteEnterLocalDownedState();
    }

    if (m_isLocalDowned.load(std::memory_order_acquire))
    {
        SetDownedControlLock(true);
        if (pPlayer)
        {
            if (auto* pCam = PlayerCamera::Get(); pCam && pCam->IsFirstPerson())
                pCam->ForceThirdPerson();

            m_bleedoutReassertTimerSec += acEvent.Delta;
            if (m_bleedoutReassertTimerSec >= 0.5)
            {
                m_bleedoutReassertTimerSec = 0.0;
                RedirectHostileAggroAwayFromActor(pPlayer);

                if (pPlayer->actorState.IsWeaponFullyDrawn())
                    pPlayer->SetWeaponDrawnEx(false);

                BSFixedString inBleedoutVar("bInBleedout");
                if (pPlayer->GetAnimationVariableInt(&inBleedoutVar) == 0)
                {
                    BSFixedString bleedoutLower("bleedoutStart");
                    pPlayer->SendAnimationEvent(&bleedoutLower);
                }
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
    if (!acMessage.IsDowned)
        m_playersBeingRevived.erase(acMessage.PlayerId);

    Actor* pDownedRemoteActor = nullptr;
    auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : playerView)
    {
        if (playerView.get<PlayerComponent>(entity).Id != acMessage.PlayerId)
            continue;

        if (Actor* pRemoteActor = Cast<Actor>(TESForm::GetById(playerView.get<FormIdComponent>(entity).Id)))
        {
            pDownedRemoteActor = pRemoteActor;
            ApplyRemoteActorDownedPosture(pRemoteActor, acMessage.IsDowned);
        }
        break;
    }

    m_world.GetRfabAdapterService().RefreshCoopCombatSnapshot();

    if (acMessage.IsDowned)
    {
        if (pDownedRemoteActor)
            RedirectHostileAggroAwayFromActor(pDownedRemoteActor);

        // Immediately claim AI ownership of active bosses/combatants in the room if the downed peer was their owner,
        // so boss combat packages, phase transitions, and animations never stall while a standing teammate is fighting!
        m_world.GetCharacterService().ClaimCombatActorsFromDownedPeer(acMessage.PlayerId);

        Utils::ShowHudMessage(String("Союзник тяжело ранен! Удерживайте [E] рядом, чтобы поднять"));
    }
}

void DownedStateService::OnNotifyReviveState(const NotifyReviveState& acMessage) noexcept
{
    const uint32_t localId = GetLocalPlayerId();
    const bool targetsLocalPlayer = (localId == 0 || acMessage.TargetPlayerId == localId);

    switch (acMessage.Status)
    {
    case NotifyReviveState::ReviveStatus::ChannelStarted:
        if (acMessage.TargetPlayerId != 0)
            m_playersBeingRevived.insert(acMessage.TargetPlayerId);
        if (m_isLocalDowned && targetsLocalPlayer)
            Utils::ShowHudMessage(String("Союзник поднимает вас..."));
        break;

    case NotifyReviveState::ReviveStatus::ChannelCancelled:
        if (acMessage.TargetPlayerId != 0)
            m_playersBeingRevived.erase(acMessage.TargetPlayerId);
        if (m_isChannelingRevive && m_channelTargetPlayerId == acMessage.TargetPlayerId)
        {
            m_isChannelingRevive = false;
            m_channelTargetPlayerId = 0;
        }
        break;

    case NotifyReviveState::ReviveStatus::RevivedByAlly:
        m_remoteDownedByPlayerId[acMessage.TargetPlayerId] = false;
        m_playersBeingRevived.erase(acMessage.TargetPlayerId);
        {
            auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
            for (auto entity : playerView)
            {
                if (playerView.get<PlayerComponent>(entity).Id != acMessage.TargetPlayerId)
                    continue;
                const uint32_t fid = playerView.get<FormIdComponent>(entity).Id;
                if (fid != 0 && fid != 0x14)
                {
                    if (Actor* pRemoteActor = Cast<Actor>(TESForm::GetById(fid)))
                        ApplyRemoteActorDownedPosture(pRemoteActor, false);
                }
                break;
            }
        }
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
        m_playersBeingRevived.erase(acMessage.TargetPlayerId);
        {
            auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
            for (auto entity : playerView)
            {
                if (playerView.get<PlayerComponent>(entity).Id != acMessage.TargetPlayerId)
                    continue;
                const uint32_t fid = playerView.get<FormIdComponent>(entity).Id;
                if (fid != 0 && fid != 0x14)
                {
                    if (Actor* pRemoteActor = Cast<Actor>(TESForm::GetById(fid)))
                        ApplyRemoteActorDownedPosture(pRemoteActor, false);
                }
                break;
            }
        }
        if (m_isLocalDowned && targetsLocalPlayer)
            ApplyAuthoritativeRevive(acMessage.HealthFraction, acMessage.GraceSec, true);
        break;

    case NotifyReviveState::ReviveStatus::PartyWipeImminent:
        Utils::ShowHudMessage(String("Весь отряд пал — перезагрузка последнего чекпоинта..."));
        break;
    }
}
