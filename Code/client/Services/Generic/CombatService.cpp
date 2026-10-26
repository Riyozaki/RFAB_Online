#include <Services/CombatService.h>
#include <Services/TransportService.h>

#include <Events/UpdateEvent.h>
#include <Events/ProjectileLaunchedEvent.h>
#include <Events/HitEvent.h>

#include <Messages/ProjectileLaunchRequest.h>
#include <Messages/NotifyProjectileLaunch.h>

#include <Systems/ModSystem.h>
#include <World.h>

#include <Projectiles/Projectile.h>
#include <Forms/TESObjectWEAP.h>
#include <Forms/TESAmmo.h>
#include <Games/ActorExtension.h>
#include <PlayerCharacter.h>
#include <Combat/CombatController.h>
#include <Services/DownedStateService.h>
#include <common/RfabModpackCompat.h>

CombatService::CombatService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CombatService::OnUpdate>(this);
    m_hitConnection = aDispatcher.sink<HitEvent>().connect<&CombatService::OnHitEvent>(this);
    m_localComponentRemoved = m_world.on_destroy<LocalComponent>().connect<&CombatService::OnLocalComponentRemoved>(this);
    m_combatComponentRemoved = m_world.on_destroy<CombatComponent>().connect<&CombatService::OnCombatComponentRemoved>(this);
    m_projectileLaunchedConnection = aDispatcher.sink<ProjectileLaunchedEvent>().connect<&CombatService::OnProjectileLaunchedEvent>(this);
    m_projectileLaunchConnection = aDispatcher.sink<NotifyProjectileLaunch>().connect<&CombatService::OnNotifyProjectileLaunch>(this);
}

void CombatService::OnUpdate(const UpdateEvent& acEvent) const noexcept
{
    RunTargetUpdates(static_cast<float>(acEvent.Delta));
}

void CombatService::OnLocalComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    m_world.remove<CombatComponent>(aEntity);
}

void CombatService::OnCombatComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    if (const auto* pFormComp = aRegistry.try_get<FormIdComponent>(aEntity); pFormComp && pFormComp->Id != 0)
    {
        if (Actor* pActor = Cast<Actor>(TESForm::GetById(pFormComp->Id)))
        {
            if (auto* pExt = pActor->GetExtension())
                pExt->SetCombatTargetLock(false);
        }
    }
}

void CombatService::OnProjectileLaunchedEvent(const ProjectileLaunchedEvent& acEvent) const noexcept
{
    if (!RfabCompat::AreProjectileLaunchParamsSafe(
            acEvent.Origin.x,
            acEvent.Origin.y,
            acEvent.Origin.z,
            acEvent.XAngle,
            acEvent.YAngle,
            acEvent.ZAngle,
            acEvent.Power,
            acEvent.Scale))
    {
        return;
    }

    ModSystem& modSystem = m_world.Get().GetModSystem();

    uint32_t shooterFormId = acEvent.ShooterID;
    const entt::entity shooterEntity = m_world.GetCharacterService().FindActorEntityByFormId(shooterFormId);
    if (shooterEntity == entt::null || !m_world.all_of<LocalComponent>(shooterEntity))
        return;

    LocalComponent& localComponent = m_world.get<LocalComponent>(shooterEntity);

    ProjectileLaunchRequest request{};

    request.OriginX = acEvent.Origin.x;
    request.OriginY = acEvent.Origin.y;
    request.OriginZ = acEvent.Origin.z;

    modSystem.GetServerModId(acEvent.ProjectileBaseID, request.ProjectileBaseID);
    modSystem.GetServerModId(acEvent.WeaponID, request.WeaponID);
    modSystem.GetServerModId(acEvent.AmmoID, request.AmmoID);

    request.ShooterID = localComponent.Id;

    request.ZAngle = acEvent.ZAngle;
    request.XAngle = acEvent.XAngle;
    request.YAngle = acEvent.YAngle;

    modSystem.GetServerModId(acEvent.ParentCellID, request.ParentCellID);
    modSystem.GetServerModId(acEvent.SpellID, request.SpellID);

    request.CastingSource = acEvent.CastingSource;

    request.Area = acEvent.Area;
    request.Power = acEvent.Power;
    request.Scale = acEvent.Scale;

    request.AlwaysHit = acEvent.AlwaysHit;
    request.NoDamageOutsideCombat = acEvent.NoDamageOutsideCombat;
    request.AutoAim = acEvent.AutoAim;
    request.DeferInitialization = acEvent.DeferInitialization;
    request.ForceConeOfFire = acEvent.ForceConeOfFire;

    request.UnkBool1 = acEvent.UnkBool1;
    request.UnkBool2 = acEvent.UnkBool2;

    m_transport.Send(request);
}

void CombatService::OnNotifyProjectileLaunch(const NotifyProjectileLaunch& acMessage) const noexcept
{
    if (!RfabCompat::AreProjectileLaunchParamsSafe(
            acMessage.OriginX,
            acMessage.OriginY,
            acMessage.OriginZ,
            acMessage.XAngle,
            acMessage.YAngle,
            acMessage.ZAngle,
            acMessage.Power,
            acMessage.Scale))
    {
        spdlog::warn("Rejected projectile launch with non-finite/out-of-range coordinates or angles.");
        return;
    }

    ModSystem& modSystem = World::Get().GetModSystem();

    const entt::entity remoteEntity = m_world.GetCharacterService().FindEntityByServerId(acMessage.ShooterID);
    if (remoteEntity == entt::null || !m_world.all_of<RemoteComponent, FormIdComponent>(remoteEntity))
    {
        spdlog::warn("Shooter with remote id {:X} not found.", acMessage.ShooterID);
        return;
    }

    FormIdComponent formIdComponent = m_world.get<FormIdComponent>(remoteEntity);

    Projectile::LaunchData launchData{};

    launchData.pShooter = Cast<TESObjectREFR>(TESForm::GetById(formIdComponent.Id));
    if (!launchData.pShooter)
    {
        spdlog::warn("Cannot launch projectile, invalid shooter form id: {:X}", formIdComponent.Id);
        return;
    }

    // Projectile::Launch relies on pParentCell being valid.
    // Exterior wilderness cells have dynamic 0xFFxxxxxx FormIDs that differ across clients;
    // fall back to the remote shooter's active local parentCell (or local player's parentCell) when lookup returns null.
    if (acMessage.ParentCellID && acMessage.ParentCellID.ModId != 0xFFFFFFFF)
    {
        const uint32_t cParentCellId = modSystem.GetGameId(acMessage.ParentCellID);
        launchData.pParentCell = Cast<TESObjectCELL>(TESForm::GetById(cParentCellId));
    }
    if (!launchData.pParentCell)
    {
        launchData.pParentCell = launchData.pShooter->parentCell;
    }
    if (!launchData.pParentCell && PlayerCharacter::Get())
    {
        launchData.pParentCell = PlayerCharacter::Get()->parentCell;
    }

    if (!launchData.pParentCell)
    {
        spdlog::warn("Cannot launch projectile, invalid parent cell for shooter {:X}", formIdComponent.Id);
        return;
    }

    launchData.Origin.x = acMessage.OriginX;
    launchData.Origin.y = acMessage.OriginY;
    launchData.Origin.z = acMessage.OriginZ;

    if (!acMessage.ProjectileBaseID || acMessage.ProjectileBaseID.ModId == 0xFFFFFFFF)
        return;

    const uint32_t cProjectileBaseId = modSystem.GetGameId(acMessage.ProjectileBaseID);
    launchData.pProjectileBase = TESForm::GetById(cProjectileBaseId);
    if (!launchData.pProjectileBase)
    {
        spdlog::warn("Cannot launch projectile, invalid projectile base id: {:X}", cProjectileBaseId);
        return;
    }

    if (acMessage.WeaponID && acMessage.WeaponID.ModId != 0xFFFFFFFF)
    {
        const uint32_t cFromWeaponId = modSystem.GetGameId(acMessage.WeaponID);
        launchData.pFromWeapon = Cast<TESObjectWEAP>(TESForm::GetById(cFromWeaponId));
    }

    if (acMessage.AmmoID && acMessage.AmmoID.ModId != 0xFFFFFFFF)
    {
        const uint32_t cFromAmmoId = modSystem.GetGameId(acMessage.AmmoID);
        launchData.pFromAmmo = Cast<TESAmmo>(TESForm::GetById(cFromAmmoId));
    }

    launchData.fZAngle = acMessage.ZAngle;
    launchData.fXAngle = acMessage.XAngle;
    launchData.fYAngle = acMessage.YAngle;

    // Never resolve runtime 0xFFxxxxxx custom player enchantments on remote clients (their magic effects sync via MagicService).
    if (acMessage.SpellID && acMessage.SpellID.ModId != 0xFFFFFFFF)
    {
        const uint32_t cSpellId = modSystem.GetGameId(acMessage.SpellID);
        launchData.pSpell = Cast<MagicItem>(TESForm::GetById(cSpellId));
    }

    launchData.eCastingSource = (MagicSystem::CastingSource)acMessage.CastingSource;

    launchData.iArea = acMessage.Area;
    launchData.fPower = acMessage.Power;
    launchData.fScale = acMessage.Scale;

    launchData.bAlwaysHit = acMessage.AlwaysHit;
    launchData.bNoDamageOutsideCombat = acMessage.NoDamageOutsideCombat;
    launchData.bAutoAim = acMessage.AutoAim;

    launchData.bForceConeOfFire = acMessage.ForceConeOfFire;

    // always use origin, or it'll recalculate it and it desyncs
    launchData.bUseOrigin = true;

    launchData.bUnkBool1 = acMessage.UnkBool1;
    launchData.bUnkBool2 = acMessage.UnkBool2;

    BSPointerHandle<Projectile> result;

    Projectile::Launch(&result, launchData);
}

void CombatService::OnHitEvent(const HitEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto* pHittee = Cast<Actor>(TESForm::GetById(acEvent.HitteeId));
    if (!pHittee || !pHittee->GetExtension())
        return;

    auto* pHitter = Cast<Actor>(TESForm::GetById(acEvent.HitterId));
    if (!pHitter || !pHitter->GetExtension())
        return;

    const entt::entity hitteeEntity = m_world.GetCharacterService().FindActorEntityByFormId(acEvent.HitteeId);
    if (hitteeEntity == entt::null || !m_world.all_of<LocalComponent>(hitteeEntity))
        return;

    const auto& downedService = m_world.GetDownedStateService();

    Actor* pCurrentTarget = pHittee->GetCombatTarget();
    if (!pCurrentTarget && pHittee->pCombatController && pHittee->pCombatController->targetHandle != 0)
        pCurrentTarget = Cast<Actor>(TESObjectREFR::GetByHandle(pHittee->pCombatController->targetHandle));

    if (pCurrentTarget == pHitter && pHittee->IsInCombat())
        return;

    auto calcDist = [](const Actor* a, const Actor* b) noexcept -> float
    {
        if (!a || !b)
            return 0.0f;
        const float dx = a->position.x - b->position.x;
        const float dy = a->position.y - b->position.y;
        const float dz = a->position.z - b->position.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    RfabCompat::CoopAggroDecisionInput decision{};
    decision.IsHitteeValidNpc = !pHittee->GetExtension()->IsPlayer();
    decision.IsHitteeRemote = pHittee->GetExtension()->IsRemote();
    decision.IsHitteeDead = pHittee->IsDead();
    decision.IsHitterPlayer = pHitter->GetExtension()->IsPlayer();
    decision.IsHitterDowned = pHitter->IsDead() || downedService.IsActorDowned(pHitter->formID);
    decision.HasActiveTargetLock = m_world.any_of<CombatComponent>(hitteeEntity);
    decision.IsHitteeInCombat = pHittee->IsInCombat();
    decision.HasValidCurrentTarget = (pCurrentTarget != nullptr);
    decision.IsCurrentTargetDeadOrDowned =
        !pCurrentTarget || pCurrentTarget->IsDead() || downedService.IsActorDowned(pCurrentTarget->formID);
    decision.DistToCurrentTarget = calcDist(pHittee, pCurrentTarget);
    decision.DistToAttacker = calcDist(pHittee, pHitter);

    if (!RfabCompat::ShouldSwitchCoopCombatTarget(decision))
        return;

    auto& combatComp = m_world.emplace_or_replace<CombatComponent>(hitteeEntity, acEvent.HitterId);
    combatComp.Timer = 4.0f;
    if (auto* pExt = pHittee->GetExtension())
        pExt->SetCombatTargetLock(true);

    pHittee->SetCombatTargetEx(pHitter);
    if (!pHittee->IsInCombat())
        pHittee->StartCombat(pHitter);
}

void CombatService::RunTargetUpdates(const float acDelta) const noexcept
{
    if (!m_transport.IsConnected() || acDelta <= 0.0f)
        return;

    const auto view = m_world.view<FormIdComponent, CombatComponent>();
    const auto& downedService = m_world.GetDownedStateService();

    Vector<entt::entity> toRemove{};

    for (const auto entity : view)
    {
        auto& combatComponent = view.get<CombatComponent>(entity);
        combatComponent.Timer -= acDelta;

        if (combatComponent.Timer <= 0.f)
        {
            toRemove.push_back(entity);
            continue;
        }

        auto* pTarget = Cast<Actor>(TESForm::GetById(combatComponent.TargetFormId));
        if (!pTarget || pTarget->IsDead() || downedService.IsActorDowned(combatComponent.TargetFormId))
        {
            toRemove.push_back(entity);
            continue;
        }

        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor || pActor->IsDead() || !pActor->IsInCombat())
        {
            toRemove.push_back(entity);
            continue;
        }
    }

    for (const auto entity : toRemove)
        m_world.remove<CombatComponent>(entity);
}
