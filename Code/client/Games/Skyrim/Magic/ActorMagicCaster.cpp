#include "ActorMagicCaster.h"

#include <Events/SpellCastEvent.h>
#include <Events/InterruptCastEvent.h>

#include <Games/Skyrim/Actor.h>
#include <Games/ActorExtension.h>

#include <World.h>

TP_THIS_FUNCTION(TSpellCast, void, ActorMagicCaster, bool sbSuccess, int32_t auiTargetCount, MagicItem* apSpell);
TP_THIS_FUNCTION(TInterruptCast, void, ActorMagicCaster, bool);

static TSpellCast* RealSpellCast = nullptr;
static TInterruptCast* RealInterruptCast = nullptr;

void TP_MAKE_THISCALL(HookSpellCast, ActorMagicCaster, bool abSuccess, int32_t auiTargetCount, MagicItem* apSpell)
{
    spdlog::debug("HookSpellCast, abSuccess: {}, auiTargetCount: {}, apSpell: {:X}", abSuccess, auiTargetCount, (uint64_t)apSpell);

    // Note: these if guards is how the game does it too
    if (!apThis || !apThis->pCasterActor)
        return;
    if (!abSuccess)
        return;
    if (!apSpell && !apThis->pCurrentSpell)
        return;

    const ActorExtension* pExtended = apThis->pCasterActor->GetExtension();
    if (!pExtended || pExtended->IsRemote())
        return;

    uint32_t targetFormId = 0;

    if (apThis->hDesiredTarget)
    {
        TESObjectREFR* pDesiredTarget = TESObjectREFR::GetByHandle(apThis->hDesiredTarget.handle.iBits);
        if (pDesiredTarget)
        {
            targetFormId = pDesiredTarget->formID;
        }
    }

    if (apSpell && apThis->pCasterActor->formID != 0)
    {
        World::Get().GetRunner().Trigger(SpellCastEvent(
            apThis->pCasterActor->formID,
            apThis->GetCastingSource(),
            apThis->GetIsDualCasting(),
            apSpell->formID,
            targetFormId));
    }

    TiltedPhoques::ThisCall(RealSpellCast, apThis, abSuccess, auiTargetCount, apSpell);
}

void TP_MAKE_THISCALL(HookInterruptCast, ActorMagicCaster, bool abRefund)
{
    if (apThis && apThis->pCasterActor && apThis->pCasterActor->formID != 0 && apThis->pCasterActor->GetNiNode())
    {
        if (const ActorExtension* pExtended = apThis->pCasterActor->GetExtension(); pExtended && pExtended->IsLocal())
        {
            World::Get().GetRunner().Trigger(InterruptCastEvent(apThis->pCasterActor->formID, apThis->eCastingSource));
        }
    }

    TiltedPhoques::ThisCall(RealInterruptCast, apThis, abRefund);
}

static TiltedPhoques::Initializer s_actorMagicCasterHooks(
    []()
    {
        POINTER_SKYRIMSE(TSpellCast, s_spellCast, 33363);
        POINTER_SKYRIMSE(TInterruptCast, s_interruptCast, 33359);

        RealSpellCast = s_spellCast.Get();
        RealInterruptCast = s_interruptCast.Get();

        TP_HOOK(&RealSpellCast, HookSpellCast);
        TP_HOOK(&RealInterruptCast, HookInterruptCast);
    });
