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

    // Thread-safe AI worker guard: hold owning GamePtr and check lock-free atomic ActorExtension::HasCombatTargetLock().
    if (GamePtr<Actor> spAttacker = Actor::LookupByHandle(attackerHandle))
    {
        if (const auto* pExt = spAttacker->GetExtension(); pExt && (pExt->IsRemote() || pExt->HasCombatTargetLock()))
            return;
    }

    if (newTarget.handle.iBits == targetHandle)
        return;

    GamePtr<Actor> spNewTarget = Actor::LookupByHandle(newTarget.handle.iBits);
    SetTarget(spNewTarget.Get());
}

TP_THIS_FUNCTION(TUpdateTarget, void, CombatController);
static TUpdateTarget* RealUpdateTarget = nullptr;

void TP_MAKE_THISCALL(HookUpdateTarget, CombatController)
{
    if (!apThis)
        return;

    // Hold owning handle reference across the entire AI worker-thread hook so main-thread actor
    // cleanup cannot free the Actor mid-execution.
    GamePtr<Actor> spAttacker = apThis->attackerHandle ? Actor::LookupByHandle(apThis->attackerHandle) : nullptr;
    if (spAttacker)
    {
        if (auto* pAttackerExt = spAttacker->GetExtension())
        {
            // Never let Skyrim's local AI target selector overwrite a remote-owned actor's combat target.
            if (pAttackerExt->IsRemote())
                return;

            // Enforce authoritative boss/aggro target lock directly inside the active AI hook (SE 32489),
            // releasing the lock automatically if the locked target died or entered downed state.
            if (pAttackerExt->HasCombatTargetLock())
            {
                GamePtr<Actor> spLockedTarget = apThis->targetHandle ? Actor::LookupByHandle(apThis->targetHandle) : nullptr;
                const auto* pLockedExt = spLockedTarget ? spLockedTarget->GetExtension() : nullptr;
                if (spLockedTarget &&
                    spLockedTarget->formID != 0 &&
                    !spLockedTarget->IsDead() &&
                    !(pLockedExt && pLockedExt->IsDownedPlayer()))
                {
                    return;
                }

                pAttackerExt->SetCombatTargetLock(false);
            }
        }
    }

    TiltedPhoques::ThisCall(RealUpdateTarget, apThis);

    if (apThis->targetHandle == 0)
        return;

    // Thread-safe AI worker guard: never touch EnTT views here!
    // Check atomic ActorExtension::IsDownedPlayer() on the selected target; if the AI picked a downed player
    // (local 0x14 or remote clone), seamlessly retarget onto the nearest standing player via mutex-protected
    // CoopCombatSnapshot (or clear target to nullptr if all players in range are downed).
    GamePtr<Actor> spCurrentTarget = Actor::LookupByHandle(apThis->targetHandle);
    if (spCurrentTarget && spCurrentTarget->GetExtension() && spCurrentTarget->GetExtension()->IsDownedPlayer())
    {
        Actor* pReplacement = World::IsConnectedCached()
            ? World::Get().GetRfabAdapterService().FindBestStandingPlayerTargetFor(spAttacker.Get())
            : nullptr;
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
