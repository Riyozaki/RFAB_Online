#pragma once

#include <Games/GameProfile.h>

struct CombatTargetSelector;
struct CombatGroup;
struct CombatState;
struct CombatInventory;
struct CombatAimController;
struct CombatTargetSelector;

struct CombatController
{
    void SetTarget(Actor* apTarget);
    void UpdateTarget();

    CombatGroup *pCombatGroup;
    CombatState *pState;
    CombatInventory *pInventory;
    void *pCombatBlackboard;
    void *pBehaviorController;
    uint32_t attackerHandle;
    uint32_t targetHandle;
    uint32_t previousTargetHandle;
    uint8_t unk34;
    bool startedCombat;
    uint8_t unk36;
    uint8_t unk37;
    TESCombatStyle *pCombatStyle;
    bool stoppedCombat;
    bool unk41;
    bool ignoringCombat;
    bool inactive;
    float unk44;
    float unk4C;
    GameArray<void*> aimControllers;
#if TP_PROFILE_IS_1597
    // 1.5.97..1.6.353: no lock here. CommonLibSSE-NG describes the runtime data
    // block as RUNTIME_DATA_ACCESSOR_VERSIONED(RUNTIME_DATA, RUNTIME_SSE_1_6_629,
    // 0x68, 0x70): 1.6.629 inserted an eight byte spin lock in front of it.
#else
    uint64_t aimControllerLock;
#endif
    CombatAimController *pCurrentAimController;
    CombatAimController *pPreviousAimController;
    GameArray<void*> areas;
    CombatAreaStandard *pCurrentArea;
    GameArray<CombatTargetSelector*> targetSelectors;
    CombatTargetSelector *pActiveTargetSelector;
    CombatTargetSelector *pPreviousTargetSelector;
    uint32_t handleCount;
    int32_t unkCC;
    NiPointer<Actor> pCachedAttacker;
    NiPointer<Actor> pCachedTarget;
};

static_assert(offsetof(CombatController, targetHandle) == 0x2C);
static_assert(offsetof(CombatController, startedCombat) == 0x35);
static_assert(offsetof(CombatController, aimControllers) == 0x50);
static_assert(offsetof(CombatController, targetSelectors) == TP_LAYOUT(0x98, 0xA0));
static_assert(offsetof(CombatController, pActiveTargetSelector) == TP_LAYOUT(0xB0, 0xB8));
static_assert(offsetof(CombatController, pCachedAttacker) == TP_LAYOUT(0xC8, 0xD0));
static_assert(sizeof(CombatController) == TP_LAYOUT(0xD8, 0xE0));
