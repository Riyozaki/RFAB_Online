#include "InvisibilityEffect.h"

#include <Forms/ActorValueInfo.h>
#include <Forms/TESObjectCELL.h>
#include <Games/ActorExtension.h>
#include <World.h>

TP_THIS_FUNCTION(TFinish, void, InvisibilityEffect);
static TFinish* RealFinish = nullptr;

// This needs to be done because the actor value does not update in time
void TP_MAKE_THISCALL(HookFinish, InvisibilityEffect)
{
    if (World::IsCreated() && World::IsConnectedCached() && apThis && apThis->pTarget)
    {
        if (Actor* pActor = apThis->pTarget->GetTargetAsActor())
        {
            const auto* pExt = pActor->GetExtension();
            TESObjectCELL* pCell = pActor->GetParentCellEx();
            if (pExt &&
                pExt->IsRemote() &&
                !pActor->IsDeleted() &&
                !pActor->IsDisabled() &&
                !pActor->IsStillLoading() &&
                pCell &&
                pCell->IsAttachedAndLoaded())
            {
                pActor->SetActorValue(ActorValueInfo::kInvisibility, 0.f);
            }
        }
    }

    TiltedPhoques::ThisCall(RealFinish, apThis);
}

static TiltedPhoques::Initializer s_invisibilityEffectsHooks(
    []()
    {
        POINTER_SKYRIMSE(TFinish, s_finish, 33594);

        RealFinish = s_finish.Get();

        TP_HOOK(&RealFinish, HookFinish);
    });
