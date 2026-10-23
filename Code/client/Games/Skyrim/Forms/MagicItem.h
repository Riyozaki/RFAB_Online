#pragma once

#include <Forms/TESBoundObject.h>
#include <Forms/EffectSetting.h>
#include <Forms/BGSKeyword.h>
#include <Magic/EffectItem.h>
#include <Components/BGSKeywordForm.h>
#include <Components/TESFullName.h>

struct MagicItem : TESBoundObject
{
    bool IsWardSpell() const noexcept;
    bool IsInvisibilitySpell() const noexcept;
    bool IsHealingSpell() const noexcept;
    bool IsBuffSpell() const noexcept;
    bool IsBoundWeaponSpell() noexcept;

    EffectItem* GetEffect(const uint32_t aEffectId) noexcept;

    TESFullName fullName;
    BGSKeywordForm keyword;
    GameArray<EffectItem*> listOfEffects;
    int32_t iHostileCount;
    EffectSetting* pAVEffectSetting;
    uint32_t uiPreloadCount;
    void* pPreloadItem;
};

#ifdef _MSC_VER
static_assert(sizeof(MagicItem) == 0x90);
#else
static_assert(sizeof(MagicItem) == 0x88);
#endif
