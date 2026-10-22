#include "Projectile.h"
#include <Games/Skyrim/Forms/TESObjectWEAP.h>
#include <Games/Skyrim/Forms/MagicItem.h>
#include <Games/Skyrim/Forms/TESAmmo.h>
#include <Magic/ActorMagicCaster.h>
#include <Actor.h>
#include <Games/ActorExtension.h>
#include <World.h>
#include <Events/ProjectileLaunchedEvent.h>
#include <Games/Skyrim/Forms/TESObjectCELL.h>
#include <Forms/SpellItem.h>

TP_THIS_FUNCTION(TLaunch, BSPointerHandle<Projectile>*, BSPointerHandle<Projectile>, Projectile::LaunchData& arData);
static TLaunch* RealLaunch = nullptr;

TP_THIS_FUNCTION(TActorMagicCasterFire, bool, ActorMagicCaster, void* apArg1, void* apArg2, void* apArg3);
static TActorMagicCasterFire* RealActorMagicCasterFire = nullptr;

BSPointerHandle<Projectile>* Projectile::Launch(BSPointerHandle<Projectile>* apResult, LaunchData& apLaunchData) noexcept
{
    BSPointerHandle<Projectile>* result = TiltedPhoques::ThisCall(RealLaunch, apResult, apLaunchData);

    TP_ASSERT(result, "No projectile handle returned.");
    if (!result)
    {
        spdlog::error("No projectile handle returned.");
        return nullptr;
    }

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(result->handle.iBits);
    Projectile* pProjectile = Cast<Projectile>(pObject);

    TP_ASSERT(pProjectile, "No projectile found.");
    if (!pProjectile)
    {
        spdlog::error("No projectile found.");
        return nullptr;
    }

    pProjectile->fPower = apLaunchData.fPower;

    return result;
}

bool TP_MAKE_THISCALL(HookActorMagicCasterFire, ActorMagicCaster, void* apArg1, void* apArg2, void* apArg3)
{
    // Non-concentration spell projectiles fired by remote actors are spawned strictly via
    // NotifyProjectileLaunch on receiving clients. Returning false at ActorMagicCaster::Fire
    // function entry prevents the caller from dereferencing a null projectile handle without
    // needing any fragile AE stack-frame unwind Xbyak trampoline.
    if (apThis && apThis->pCasterActor && apThis->pCasterActor->GetExtension()->IsRemote())
    {
        const auto* pSpell = Cast<SpellItem>(apThis->pCurrentSpell);
        if (!pSpell || pSpell->eCastingType != MagicSystem::CastingType::CONCENTRATION)
        {
            return false;
        }
    }

    return TiltedPhoques::ThisCall(RealActorMagicCasterFire, apThis, apArg1, apArg2, apArg3);
}

BSPointerHandle<Projectile>* TP_MAKE_THISCALL(HookLaunch, BSPointerHandle<Projectile>, Projectile::LaunchData& arData)
{
    // sync concentration spells through spell cast sync, the rest through projectile sync
    if (arData.pSpell)
    {
        if (auto* pSpell = Cast<SpellItem>(arData.pSpell))
        {
            if (pSpell->eCastingType == MagicSystem::CastingType::CONCENTRATION)
            {
                return TiltedPhoques::ThisCall(RealLaunch, apThis, arData);
            }
        }
    }

    if (arData.pShooter)
    {
        Actor* pActor = Cast<Actor>(arData.pShooter);
        if (pActor)
        {
            ActorExtension* pExtendedActor = pActor->GetExtension();
            if (pExtendedActor->IsRemote())
            {
                apThis->handle.iBits = 0;
                return apThis;
            }
        }
    }

    ProjectileLaunchedEvent Event{};
    Event.Origin = arData.Origin;
    if (arData.pProjectileBase)
        Event.ProjectileBaseID = arData.pProjectileBase->formID;
    if (arData.pShooter)
        Event.ShooterID = arData.pShooter->formID;
    if (arData.pFromWeapon)
        Event.WeaponID = arData.pFromWeapon->formID;
    if (arData.pFromAmmo)
        Event.AmmoID = arData.pFromAmmo->formID;
    Event.ZAngle = arData.fZAngle;
    Event.XAngle = arData.fXAngle;
    Event.YAngle = arData.fYAngle;
    if (arData.pParentCell)
        Event.ParentCellID = arData.pParentCell->formID;
    if (arData.pSpell)
        Event.SpellID = arData.pSpell->formID;
    Event.CastingSource = arData.eCastingSource;
    Event.UnkBool1 = arData.bUnkBool1;
    Event.Area = arData.iArea;
    Event.Power = arData.fPower;
    Event.Scale = arData.fScale;
    Event.AlwaysHit = arData.bAlwaysHit;
    Event.NoDamageOutsideCombat = arData.bNoDamageOutsideCombat;
    Event.AutoAim = arData.bAutoAim;
    Event.UnkBool2 = arData.bUnkBool2;
    Event.DeferInitialization = arData.bDeferInitialization;
    Event.ForceConeOfFire = arData.bForceConeOfFire;

    auto result = TiltedPhoques::ThisCall(RealLaunch, apThis, arData);

    TP_ASSERT(result, "No projectile handle returned.");
    if (!result)
    {
        spdlog::error("HookLaunch: no projectile handle returned.");
        return nullptr;
    }

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(result->handle.iBits);
    Projectile* pProjectile = Cast<Projectile>(pObject);

    TP_ASSERT(pProjectile, "No projectile found.");
    if (pProjectile)
        Event.Power = pProjectile->fPower;

    World::Get().GetRunner().Trigger(Event);

    return result;
}

static TiltedPhoques::Initializer s_projectileHooks(
    []()
    {
        // Projectile::Launch (SE 42928 @ 0x74B170) & ActorMagicCaster::Fire (SE 33672 @ 0x5506C0)
        POINTER_SKYRIMSE(TLaunch, s_launch, 42928);
        POINTER_SKYRIMSE(TActorMagicCasterFire, s_actorMagicCasterFire, 33672);

        RealLaunch = s_launch.Get();
        RealActorMagicCasterFire = s_actorMagicCasterFire.Get();

        TP_HOOK(&RealLaunch, HookLaunch);
        TP_HOOK(&RealActorMagicCasterFire, HookActorMagicCasterFire);
    });
