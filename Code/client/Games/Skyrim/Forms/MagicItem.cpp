#include "MagicItem.h"
#include <Forms/ActorValueInfo.h>
#include <Forms/BGSListForm.h>
#include <Games/TES.h>
#include <RfabModpackCompat.h>
#include <Services/RfabAdapterService.h>
#include <World.h>

bool MagicItem::IsWardSpell() const noexcept
{
    BGSKeyword* pMagicWard = Cast<BGSKeyword>(TESForm::GetById(0x1ea69));

    if (keyword.count > 0 && keyword.Contains(pMagicWard))
        return true;

    // Spells typically don't have keywords, so we must check their effects
    for (const EffectItem* pEffect : listOfEffects)
    {
        if (pEffect->pEffectSetting && pEffect->pEffectSetting->keywordForm.count > 0 && 
            pEffect->pEffectSetting->keywordForm.Contains(pMagicWard))
            return true;
    }

    return false;
}

bool MagicItem::IsInvisibilitySpell() const noexcept
{
    // PR #796 and the "SkyPatcher Keyword Framework" mod both hoist
    // some keywords from the effects up to the spell, because some 
    // spell records are buggy; this fixes things like syncing more ward spells.
    // But the fix can break other things. This is the first one found,
    // Bow of Shadows Invisibility (formID 0xFEXXX805), which doesn't have an 
    // Invisibility keyword. Apparently deliberately, it isn't really a spell but a 
    // condition (readying the bow) that triggers an effect. If it is treated as a 
    // spell, syncing doesn't work. There may be others, if so, there will be a 
    // more general fix but this is the pattern, just needs an array / loop.
    static bool runonce = true; 
    static uint32_t bosFormID = 0;      // Can be an array if more are found.
    if (runonce)
    {
        runonce = false;  
        Mod* pBOSMod = nullptr;
        MagicItem* pSpell  = nullptr;
        if (   (pBOSMod = ModManager::Get()->GetByName("ccbgssse038-bowofshadows.esl")) 
            && (pSpell  = Cast<MagicItem>(TESForm::GetById(pBOSMod->GetFormId(0x805)))))
            bosFormID = pSpell->formID;
    }

    if (formID == bosFormID)
        return false;

    BGSKeyword* pMagicInvisibility = Cast<BGSKeyword>(TESForm::GetById(0x1ea6f));

    if (keyword.count > 0 && keyword.Contains(pMagicInvisibility))
        return true;

    for (const EffectItem* pEffect : listOfEffects)
    {
        if (pEffect->pEffectSetting && pEffect->pEffectSetting->keywordForm.count > 0 &&
            pEffect->pEffectSetting->keywordForm.Contains(pMagicInvisibility))
            return true;
    }
    return false;
}

bool MagicItem::IsHealingSpell() const noexcept
{
    BGSKeyword* pMagicRestoreHealth = Cast<BGSKeyword>(TESForm::GetById(0x1ceb0));

    if (pMagicRestoreHealth && keyword.count > 0 && keyword.Contains(pMagicRestoreHealth))
        return true;

    if (iHostileCount > 0)
        return false;

    bool hasHealEffect = false;
    for (const EffectItem* pEffect : listOfEffects)
    {
        if (!pEffect || !pEffect->pEffectSetting)
            continue;

        const EffectSetting* pSetting = pEffect->pEffectSetting;
        if ((pSetting->flags & EffectSetting::kHostile) != 0)
            return false;

        if (pMagicRestoreHealth && pSetting->keywordForm.count > 0 && pSetting->keywordForm.Contains(pMagicRestoreHealth))
            hasHealEffect = true;

        // Detect all RFAB / Requiem / Apocalypse / Odin non-hostile Restore Health effects even when they lack 0x1CEB0!
        if ((pSetting->flags & (EffectSetting::kHostile | EffectSetting::kDetrimental | EffectSetting::kRecover)) == 0 &&
            (pSetting->eArchetype == EffectArchetypes::ArchetypeID::kValueModifier ||
             pSetting->eArchetype == EffectArchetypes::ArchetypeID::kDualValueModifier) &&
            pSetting->primaryAV == ActorValueInfo::kHealth &&
            pEffect->data.fMagnitude > 0.0f)
        {
            hasHealEffect = true;
        }
    }

    return hasHealEffect;
}

bool MagicItem::IsBuffSpell() const noexcept
{
    switch (formID)
    {
    case 0x4dee8: // Courage / Lionheart
    case 0x4deec: // Rally
    case 0x7e8dd: // Call to Arms
        return true;
    default:
        break;
    }

    // Honor RFAB's explicit FLST RFAB_List_NoAllyDamage_AllowedSpells (0x06005CAA: NecrohealOnTarget, ArcaneMending,
    // ProtectionFromPoisonDiseaseOnTarget, HealingHands, LionheartCloakProc, AuraOfWarlock, AuraSummoning*, etc.).
    if (BGSListForm* pAllowedList = World::Get().GetRfabAdapterService().GetNoAllyDamageAllowedSpellsList())
    {
        if (pAllowedList->ContainsFormId(formID))
            return true;
    }

    if (iHostileCount > 0)
        return false;

    bool hasFriendlyTargetedBuff = false;
    for (const EffectItem* pEffect : listOfEffects)
    {
        if (!pEffect || !pEffect->pEffectSetting)
            continue;

        const EffectSetting* pSetting = pEffect->pEffectSetting;
        if ((pSetting->flags & (EffectSetting::kHostile | EffectSetting::kDetrimental)) != 0)
            return false;

        if (pEffect->IsSummonEffect())
            continue;

        // Exclude self-only non-AoE non-cloak toggles/abilities; player-to-player buffs have AIMED / TARGET_ACTOR /
        // TARGET_LOCATION / CONTACT delivery, OR SELF delivery with AoE radius (iArea > 0) or kCloak aura archetype.
        if (pSetting->deliveryType == static_cast<uint32_t>(MagicSystem::Delivery::SELF) &&
            pEffect->data.iArea <= 0 &&
            pSetting->eArchetype != EffectArchetypes::ArchetypeID::kCloak)
        {
            continue;
        }

        switch (pSetting->eArchetype)
        {
        case EffectArchetypes::ArchetypeID::kValueModifier:
        case EffectArchetypes::ArchetypeID::kPeakValueModifier:
        case EffectArchetypes::ArchetypeID::kDualValueModifier:
        case EffectArchetypes::ArchetypeID::kAccumulateMagnitude:
        case EffectArchetypes::ArchetypeID::kValueAndParts:
        case EffectArchetypes::ArchetypeID::kRally:
        case EffectArchetypes::ArchetypeID::kCureDisease:
        case EffectArchetypes::ArchetypeID::kCurePoison:
        case EffectArchetypes::ArchetypeID::kCureParalysis:
        case EffectArchetypes::ArchetypeID::kDispel:
        case EffectArchetypes::ArchetypeID::kInvisibility:
        case EffectArchetypes::ArchetypeID::kCloak:
        case EffectArchetypes::ArchetypeID::kEnhanceWeapon:
        case EffectArchetypes::ArchetypeID::kLight:
            hasFriendlyTargetedBuff = true;
            break;
        case EffectArchetypes::ArchetypeID::kScript:
            if ((pSetting->flags & EffectSetting::kRecover) != 0)
            {
                hasFriendlyTargetedBuff = true;
            }
            else
            {
                const char* effEdid = pSetting->GetFormEditorID();
                const char* spellEdid = GetFormEditorID();
                if ((effEdid && RfabCompat::IsFriendlyScriptBuffEffectEditorId(effEdid)) ||
                    (spellEdid && RfabCompat::IsFriendlyScriptBuffEffectEditorId(spellEdid)))
                {
                    hasFriendlyTargetedBuff = true;
                }
            }
            break;
        default:
            break;
        }
    }

    return hasFriendlyTargetedBuff;
}

bool MagicItem::IsBoundWeaponSpell() noexcept
{
    for (EffectItem* pEffect : listOfEffects)
    {
        if (pEffect->pEffectSetting && pEffect->pEffectSetting->eArchetype == EffectArchetypes::ArchetypeID::kBoundWeapon)
            return true;
    }

    return false;
}

EffectItem* MagicItem::GetEffect(const uint32_t aEffectId) noexcept
{
    for (EffectItem* pEffect : listOfEffects)
    {
        if (pEffect->pEffectSetting->formID == aEffectId)
            return pEffect;
    }

    return nullptr;
}
