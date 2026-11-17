#include "CombatController.h"
#include "CombatTargetSelector.h"
#include <Games/ActorExtension.h>
#include <Actor.h>

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

    // If CombatComponent is attached, don't try to fetch a new target.
    if (Actor* pAttacker = Cast<Actor>(TESObjectREFR::GetByHandle(attackerHandle)))
    {
        const auto view = World::Get().view<FormIdComponent, CombatComponent>();
        const auto it = std::find_if(view.begin(), view.end(), [view, pAttacker](auto entity) { return view.get<FormIdComponent>(entity).Id == pAttacker->formID; });
        if (it != view.end())
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
    apThis->UpdateTarget();
}

TP_THIS_FUNCTION(TSetCombatTarget, void, CombatController, Actor*);
static TSetCombatTarget* RealSetCombatTarget = nullptr;

void TP_MAKE_THISCALL(HookSetCombatTarget, CombatController, Actor* apTarget)
{
    // Called by native AI jobs: read only an actor-local atomic snapshot. Keep the original
    // selector/AI implementation; do not enable the old replacement UpdateTarget() above.
    if (apTarget && apTarget->GetExtension()->CoopDowned.load(std::memory_order_relaxed))
        apTarget = nullptr;
    TiltedPhoques::ThisCall(RealSetCombatTarget, apThis, apTarget);
}

void CombatController::SetTarget(Actor* apTarget)
{
    TiltedPhoques::ThisCall(RealSetCombatTarget, this, apTarget);
}

static TiltedPhoques::Initializer s_combatControllerHooks(
    []()
    {
        POINTER_SKYRIMSE(TSetCombatTarget, s_setCombatTarget, 32488);
        RealSetCombatTarget = s_setCombatTarget.Get();
        TP_HOOK(&RealSetCombatTarget, HookSetCombatTarget);
#if 0
        POINTER_SKYRIMSE(TUpdateTarget, s_updateTarget, 32489);

        RealUpdateTarget = s_updateTarget.Get();

        TP_HOOK(&RealUpdateTarget, HookUpdateTarget);
#endif
    });
