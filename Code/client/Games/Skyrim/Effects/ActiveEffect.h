#pragma once

#include <Games/Magic/MagicSystem.h>

struct MagicTarget;
struct EffectItem;

struct ActiveEffect
{
    // this ctor is used by seemingly all ActiveEffect child classes' Instantiate() methods
    // can be used to find individual Instantiate() methods
    // address: 0x140C4E350
    ActiveEffect(Actor* apCaster, MagicItem* apSpell, EffectItem* apEffect);

    virtual void sub_0();
    virtual void sub_1();
    virtual void sub_2();
    virtual void sub_3();
    virtual void sub_4();
    virtual void sub_5();
    virtual void sub_6();
    virtual void sub_7();
    virtual void sub_8();
    virtual void sub_9();
    virtual void sub_A();
    virtual void sub_B();
    virtual void sub_C();
    virtual void sub_D();
    virtual void sub_E();
    virtual void sub_F();
    virtual void sub_10();
    virtual void sub_11();
    virtual void sub_12();
    virtual void sub_13();
    virtual void Start();
    virtual void sub_15();
    virtual void sub_16();
    virtual void sub_17();
    virtual void sub_18();

    static ActiveEffect* Instantiate(Actor* apCaster, MagicItem* apSpell, EffectItem* apEffect);

    // Skyrim SE 1.5.97 layout.
    //
    // Anchors taken from the upstream pre-AE header (commit 5c5837f6, written
    // against the real 1.5.97 binary): pTarget is at 0x50, the magnitude float
    // ("effectValue" there) at 0x78, and sizeof(ActiveEffect) == 0x80.
    //
    // AE (1.6.x) appended eConditionStatus / usUniqueID / eCastingSource, which
    // grew the object to 0x90 and shifted every ActiveEffect array stride. Those
    // tail fields do not exist on 1.5.97 and must not be read here.
    // The individual field names between 0x58 and 0x74 follow the AE order; they
    // are only used by admin debug views, the proven anchors are pTarget and
    // fMagnitude.
    uint8_t pad8[0x34 - 0x8];
    uint32_t hCaster;               // 0x34
    NiNode* pSourceNode;            // 0x38
    MagicItem* pSpell;              // 0x40
    void* pEffect;                  // 0x48
    MagicTarget* pTarget;           // 0x50 (verified)
    TESBoundObject* pSource;        // 0x58
    void* pHitEffects;              // 0x60
    MagicItem* pDisplacementSpell;  // 0x68
    float fElapsedSeconds;          // 0x70 (inferred, debug view only)
    float fDuration;                // 0x74 (inferred, debug view only)
    float fMagnitude;               // 0x78 (verified: pre-AE "effectValue")
    uint32_t unk7C;                 // 0x7C (pre-AE: pad_007C)
};
static_assert(sizeof(ActiveEffect) == 0x80);
static_assert(offsetof(ActiveEffect, pTarget) == 0x50);
static_assert(offsetof(ActiveEffect, fMagnitude) == 0x78);

namespace ActiveEffectFactory
{
ActiveEffect* Activate(Actor* apCaster, MagicSystem::CastingSource aeCastingSource, MagicItem* apSpell, EffectItem* apEffectItem, TESBoundObject* apSource, bool abWornEnchantment);
};
