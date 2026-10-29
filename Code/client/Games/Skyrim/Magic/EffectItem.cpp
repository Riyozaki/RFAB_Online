#include "EffectItem.h"

#include <Games/Magic/MagicSystem.h>
#include <Forms/EffectSetting.h>

bool EffectItem::IsHealingEffect() const noexcept
{
    return pEffectSetting && pEffectSetting->eArchetype == EffectArchetypes::ArchetypeID::kValueModifier && data.fMagnitude > 0.0f;
}

bool EffectItem::IsSummonEffect() const noexcept
{
    if (!pEffectSetting)
        return false;

    const auto arch = pEffectSetting->eArchetype;
    return arch == EffectArchetypes::ArchetypeID::kSummonCreature ||
           arch == EffectArchetypes::ArchetypeID::kReanimate ||
           arch == EffectArchetypes::ArchetypeID::kCommandSummoned;
}

bool EffectItem::IsSlowEffect() const noexcept
{
    return pEffectSetting && pEffectSetting->eArchetype == EffectArchetypes::ArchetypeID::kSlowTime;
}

bool EffectItem::IsInivisibilityEffect() const noexcept
{
    return pEffectSetting && pEffectSetting->eArchetype == EffectArchetypes::ArchetypeID::kInvisibility;
}

bool EffectItem::IsWerewolfEffect() const noexcept
{
    return pEffectSetting && pEffectSetting->eArchetype == EffectArchetypes::ArchetypeID::kWerewolf;
}

bool EffectItem::IsVampireLordEffect() const noexcept
{
    return pEffectSetting && pEffectSetting->eArchetype == EffectArchetypes::ArchetypeID::kVampireLord;
}

bool EffectItem::IsNightVisionEffect() const noexcept
{
    if (!pEffectSetting)
        return false;

    BGSKeyword* pMagicNightEye = Cast<BGSKeyword>(TESForm::GetById(0xad7c6));
    if (pEffectSetting->keywordForm.count == 0)
    {
        const char* effectName = pEffectSetting->fullName.value.AsAscii() ? pEffectSetting->fullName.value.AsAscii() : "";
        spdlog::debug(__FUNCTION__ ": correcting BGSKeywordForm::Contains() bug for zero-keyword {:x}, {}", pEffectSetting->formID, effectName);
    }
    return pMagicNightEye && pEffectSetting->keywordForm.count > 0 && pEffectSetting->keywordForm.Contains(pMagicNightEye);
}

