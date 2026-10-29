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

TP_THIS_FUNCTION(TActorMagicCasterFire, bool, MagicCaster);
static TActorMagicCasterFire* RealActorMagicCasterFire = nullptr;

extern "C" bool ShouldBlockRemoteMagicProjectile(MagicCaster* apCaster) noexcept
{
    if (!apCaster || !World::IsCreated() || !World::IsConnectedCached())
        return false;

    const auto* pActorCaster = Cast<ActorMagicCaster>(apCaster);
    if (!pActorCaster || !pActorCaster->pCasterActor)
        return false;

    const auto* pExtension = pActorCaster->pCasterActor->GetExtension();
    if (!pExtension || !pExtension->IsRemote())
        return false;

    const auto* pSpell = Cast<SpellItem>(apCaster->pCurrentSpell);
    if (pSpell && pSpell->eCastingType == MagicSystem::CastingType::CONCENTRATION)
        return false;

    return true;
}

BSPointerHandle<Projectile>* Projectile::Launch(BSPointerHandle<Projectile>* apResult, LaunchData& apLaunchData) noexcept
{
    BSPointerHandle<Projectile>* result = TiltedPhoques::ThisCall(RealLaunch, apResult, apLaunchData);

    if (!result)
    {
        spdlog::error("No projectile handle returned.");
        return nullptr;
    }

    GamePtr<TESObjectREFR> spObject = TESObjectREFR::LookupByHandle(result->handle.iBits);
    Projectile* pProjectile = Cast<Projectile>(spObject.Get());

    if (!pProjectile)
    {
        spdlog::error("No projectile found.");
        return nullptr;
    }

    pProjectile->fPower = apLaunchData.fPower;

    return result;
}

BSPointerHandle<Projectile>* TP_MAKE_THISCALL(HookLaunch, BSPointerHandle<Projectile>, Projectile::LaunchData& arData)
{
    if (!World::IsCreated() || !World::IsConnectedCached())
        return TiltedPhoques::ThisCall(RealLaunch, apThis, arData);

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
            if (pExtendedActor && pExtendedActor->IsRemote())
            {
                if (apThis)
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

    if (!result)
    {
        spdlog::error("HookLaunch: no projectile handle returned.");
        return nullptr;
    }

    GamePtr<TESObjectREFR> spObject = TESObjectREFR::LookupByHandle(result->handle.iBits);
    Projectile* pProjectile = Cast<Projectile>(spObject.Get());

    if (pProjectile)
        Event.Power = pProjectile->fPower;

    if (World::IsCreated() && World::IsConnectedCached())
        World::Get().GetRunner().Trigger(Event);

    return result;
}

static TiltedPhoques::Initializer s_projectileHooks(
    []()
    {
        // Projectile::Launch (SE 42928 @ 0x74B170) & MagicCaster::FireProjectile (SE 33672 @ 0x5506C0)
        POINTER_SKYRIMSE(TLaunch, s_launch, 42928);
        POINTER_SKYRIMSE(TActorMagicCasterFire, s_actorMagicCasterFire, 33672);

        struct FireEntryStub : TiltedPhoques::CodeGenerator
        {
            // IMPORTANT: TiltedPhoques::CodeGenerator defaults to 64 bytes, whereas this full x64
            // register/XMM save+restore wrapper emits ~120 bytes (throwing Xbyak::Error "code is too big"
            // -> 0xE06D7363 at startup if left at 64)! Allocate 512 bytes (power of 2) in .highrip.
            FireEntryStub()
                : TiltedPhoques::CodeGenerator(512)
            {
                // Entry RSP has 8-byte return address pushed (RSP % 16 == 8).
                // Sub 0x88 makes RSP 16-byte aligned and reserves 0x20 callee shadow space + register save slots
                // without touching caller's return address [S0] or stack arguments [S0 + 0x28+].
                sub(rsp, 0x88);
                mov(ptr[rsp + 0x20], rcx);
                mov(ptr[rsp + 0x28], rdx);
                mov(ptr[rsp + 0x30], r8);
                mov(ptr[rsp + 0x38], r9);
                movups(ptr[rsp + 0x40], xmm0);
                movups(ptr[rsp + 0x50], xmm1);
                movups(ptr[rsp + 0x60], xmm2);
                movups(ptr[rsp + 0x70], xmm3);

                mov(rax, reinterpret_cast<uintptr_t>(&ShouldBlockRemoteMagicProjectile));
                call(rax);

                mov(rcx, ptr[rsp + 0x20]);
                mov(rdx, ptr[rsp + 0x28]);
                mov(r8, ptr[rsp + 0x30]);
                mov(r9, ptr[rsp + 0x38]);
                movups(xmm0, ptr[rsp + 0x40]);
                movups(xmm1, ptr[rsp + 0x50]);
                movups(xmm2, ptr[rsp + 0x60]);
                movups(xmm3, ptr[rsp + 0x70]);
                add(rsp, 0x88);

                cmp(al, 0);
                jz("call_real");

                mov(al, 0);
                ret();

                L("call_real");
                mov(rax, reinterpret_cast<uintptr_t>(&RealActorMagicCasterFire));
                mov(rax, ptr[rax]);
                jmp(rax);
            }
        };
        static FireEntryStub s_fireEntryStub;
        FlushInstructionCache(GetCurrentProcess(), s_fireEntryStub.getCode<const void*>(), s_fireEntryStub.getSize());

        RealLaunch = s_launch.Get();
        RealActorMagicCasterFire = s_actorMagicCasterFire.Get();

        TP_HOOK(&RealLaunch, HookLaunch);
        TP_HOOK(&RealActorMagicCasterFire, s_fireEntryStub.getCode<TActorMagicCasterFire*>());
    });
