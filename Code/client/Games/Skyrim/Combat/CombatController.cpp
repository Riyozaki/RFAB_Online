#include "CombatController.h"
#include "CombatTargetSelector.h"
#include <Games/ActorExtension.h>
#include <Games/References.h>
#include <Services/RfabAdapterService.h>
#include <World.h>

void ArrayQuickSortRecursiveCombatTargets(GameArray<CombatTargetSelector*>* apArray, uint32_t aiLowIndex,
                                          uint32_t aiHighIndex)
{
    using TArrayQuickSort = void(GameArray<CombatTargetSelector*>* apArray, void* apFunction, uint32_t aiLowIndex, uint32_t aiHighIndex);
    POINTER_SKYRIMSE(TArrayQuickSort, arrayQuickSort, 32528);

    using TSortTargetSelectors = int64_t(int64_t, int64_t);
    POINTER_SKYRIMSE(TSortTargetSelectors, sortTargetSelectors, 32525);

    arrayQuickSort(apArray, sortTargetSelectors, aiLowIndex, aiHighIndex);
}

void CombatController::UpdateTarget()
{
    for (auto* pTargetSelector : targetSelectors)
    {
        if ((pTargetSelector->flags & 2) == 0)
            pTargetSelector->Update();
    }

    if (targetSelectors.length > 1)
        ArrayQuickSortRecursiveCombatTargets(&targetSelectors, 0, targetSelectors.length - 1);

    pActiveTargetSelector = nullptr;
    BSPointerHandle<Actor> newTarget{};

    if (targetSelectors.length)
    {
        for (auto* pTargetSelector : targetSelectors)
        {
            if ((pTargetSelector->flags & 1) != 0 && (pTargetSelector->flags & 2) == 0)
            {
                newTarget = pTargetSelector->SelectTarget();
                if (newTarget)
                {
                    pActiveTargetSelector = pTargetSelector;
                    break;
                }
            }
        }
    }

    // Thread-safe AI worker guard: check lock-free atomic ActorExtension::HasCombatTargetLock() instead of touching EnTT views!
    if (Actor* pAttacker = Cast<Actor>(TESObjectREFR::GetByHandle(attackerHandle)))
    {
        if (const auto* pExt = pAttacker->GetExtension(); pExt && pExt->HasCombatTargetLock())
            return;
    }

    if (newTarget.handle.iBits == targetHandle)
        return;

    Actor* pNewTarget = Cast<Actor>(TESObjectREFR::GetByHandle(newTarget.handle.iBits));
    SetTarget(pNewTarget);
}

TP_THIS_FUNCTION(TUpdateTarget, void, CombatController);
static TUpdateTarget* RealUpdateTarget = nullptr;

void TP_MAKE_THISCALL(HookUpdateTarget, CombatController)
{
    TiltedPhoques::ThisCall(RealUpdateTarget, apThis);

    if (!apThis || apThis->targetHandle == 0)
        return;

    // Thread-safe AI worker guard: never touch EnTT views here!
    // Check atomic ActorExtension::IsDownedPlayer() on the selected target; if the AI picked a downed player
    // (local 0x14 or remote clone), seamlessly retarget onto the nearest standing player via mutex-protected
    // CoopCombatSnapshot (or clear target to nullptr if all players in range are downed).
    Actor* pCurrentTarget = Cast<Actor>(TESObjectREFR::GetByHandle(apThis->targetHandle));
    if (pCurrentTarget && pCurrentTarget->GetExtension() && pCurrentTarget->GetExtension()->IsDownedPlayer())
    {
        Actor* pAttacker = apThis->attackerHandle ? Cast<Actor>(TESObjectREFR::GetByHandle(apThis->attackerHandle)) : nullptr;
        Actor* pReplacement = World::Get().GetRfabAdapterService().FindBestStandingPlayerTargetFor(pAttacker);
        apThis->SetTarget(pReplacement);
    }
}

void CombatController::SetTarget(Actor* apTarget)
{
    TP_THIS_FUNCTION(TSetTarget, void, CombatController, Actor*);
    POINTER_SKYRIMSE(TSetTarget, setTarget, 32488);
    TiltedPhoques::ThisCall(setTarget, this, apTarget);
}

static TiltedPhoques::Initializer s_combatControllerHooks(
    []()
    {
        POINTER_SKYRIMSE(TUpdateTarget, s_updateTarget, 32489);

        RealUpdateTarget = s_updateTarget.Get();

        TP_HOOK(&RealUpdateTarget, HookUpdateTarget);
    });

