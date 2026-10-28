#include <Games/References.h>
#include <Games/Skyrim/EquipManager.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>
#include <Havok/bhkCharacterController.h>
#include <CoopOwnership.h>
#include <CoopDamagePipeline.h>
#include <CoopWorldStabilizer.h>
#include <Misc/GameVM.h>
#include <DefaultObjectManager.h>
#include <Forms/TESNPC.h>
#include <Forms/SpellItem.h>
#include <Forms/TESShout.h>
#include <Forms/TESFaction.h>
#include <Forms/TESQuest.h>
#include <Components/TESActorBaseData.h>
#include <ExtraData/ExtraFactionChanges.h>
#include <Games/Memory.h>
#include <Forms/TESLevItem.h>
#include <Forms/TESObjectARMO.h>
#include <Combat/CombatController.h>

#include <Events/HealthChangeEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/MountEvent.h>
#include <Events/DialogueEvent.h>
#include <Events/HitEvent.h>
#include <Events/RemoveSpellEvent.h>

#include <Games/TES.h>
#include <Games/Skyrim/Interface/UI.h>
#include <World.h>
#include <Services/PapyrusService.h>
#include <Services/DownedStateService.h>
#include <Services/RfabAdapterService.h>

#include <Forms/ActorValueInfo.h>
#include <Forms/EffectSetting.h>
#include <Forms/TESRace.h>
#include <Magic/EffectItem.h>

#include <Effects/ValueModifierEffect.h>

#include <Games/Skyrim/Misc/InventoryEntry.h>
#include <Games/Skyrim/ExtraData/ExtraCount.h>
#include <Games/Misc/ActorKnowledge.h>

#include <ExtraData/ExtraDataList.h>
#include <ExtraData/ExtraLeveledCreature.h>
#include <ExtraData/ExtraCharge.h>
#include <ExtraData/ExtraCount.h>
#include <ExtraData/ExtraEnchantment.h>
#include <ExtraData/ExtraHealth.h>
#include <ExtraData/ExtraPoison.h>
#include <ExtraData/ExtraSoul.h>
#include <ExtraData/ExtraTextDisplayData.h>
#include <Forms/EnchantmentItem.h>
#include <Forms/AlchemyItem.h>
#include <Forms/TESObjectCELL.h>

#include <Structs/Skyrim/AnimationGraphDescriptor_BHR_Master.h>

#include <Games/Overrides.h>
#include <Games/Skyrim/BSAnimationGraphManager.h>
#include <Havok/hkbStateMachine.h>
#include <Havok/hkbBehaviorGraph.h>
#include <Havok/bhkCharacterController.h>
#include <Forms/BGSOutfit.h>
#include <Forms/TESObjectARMO.h>

#include <ModCompat/BehaviorVar.h>

#include <World.h>

#ifdef SAVE_STUFF

#include <Games/Skyrim/SaveLoad.h>
#include "Actor.h"

void Actor::Save_Reversed(const uint32_t aChangeFlags, Buffer::Writer& aWriter)
{
    BGSSaveFormBuffer buffer;

    Save(&buffer);

    AIProcess* pProcess = currentProcess;
    const int32_t handlerId = pProcess != nullptr ? pProcess->handlerId : -1;

    aWriter.WriteBytes((uint8_t*)&handlerId, 4); // TODO: is this needed ?
    aWriter.WriteBytes((uint8_t*)&flags1, 4);

    //     if (!handlerId
    //         && (uint8_t)AIProcess::GetBoolInSubStructure(pProcess))
    //     {
    //         Actor::SaveSkinFar(this);
    //     }

    TESObjectREFR::Save_Reversed(aChangeFlags, aWriter);

    if (pProcess)
        ; // Skyrim saves the process manager state, but we don't give a shit so skip !

    aWriter.WriteBytes((uint8_t*)&unk194, 4);
    aWriter.WriteBytes((uint8_t*)&headTrackingUpdateDelay, 4);
    aWriter.WriteBytes((uint8_t*)&unk9C, 4);
    // We skip 0x180 as it's not something we care about, some timer related data

    aWriter.WriteBytes((uint8_t*)&unk98, 4);
    // skip A8 - related to timers
    // skip AC - related to timers as well
    aWriter.WriteBytes((uint8_t*)&unkB0, 4);
    // skip E4 - never seen this used
    // skip E8 - same as E4
    aWriter.WriteBytes((uint8_t*)&unk84, 4);
    aWriter.WriteBytes((uint8_t*)&unkA4, 4);
    // skip baseForm->weight
    // skip 12C

    // Save actor state sub_6F0FB0
}

#endif

TP_THIS_FUNCTION(TRemoveSpell, bool, Actor, MagicItem*);
TP_THIS_FUNCTION(TCharacterConstructor, Actor*, Actor);
TP_THIS_FUNCTION(TCharacterConstructor2, Actor*, Actor, uint8_t aUnk);
TP_THIS_FUNCTION(TCharacterDestructor, Actor*, Actor);
TP_THIS_FUNCTION(TAddInventoryItem, void, Actor, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner);
TP_THIS_FUNCTION(TPickUpObject, void*, Actor, TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2);
TP_THIS_FUNCTION(TDropObject, void*, Actor, void* apResult, TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation);
TP_THIS_FUNCTION(TSetPosition, char, Actor, NiPoint3& acPosition);
TP_THIS_FUNCTION(TActorProcess, char, Actor, float aValue);

using TGetLocation = TESForm*(TESForm*);
static TGetLocation* FUNC_GetActorLocation;

TCharacterConstructor* RealCharacterConstructor;
TCharacterConstructor2* RealCharacterConstructor2;
TCharacterDestructor* RealCharacterDestructor;

static TRemoveSpell* RealRemoveSpell = nullptr;
static TAddInventoryItem* RealAddInventoryItem = nullptr;
static TPickUpObject* RealPickUpObject = nullptr;
static TDropObject* RealDropObject = nullptr;
static TSetPosition* RealSetPosition = nullptr;
static TActorProcess* RealActorProcess = nullptr;

float Actor::GetSpeed() noexcept
{
    static BSFixedString speedSampledStr("SpeedSampled");
    float speed = 0.f;
    animationGraphHolder.GetVariableFloat(&speedSampledStr, &speed);

    return speed;
}

void Actor::SetSpeed(float aSpeed) noexcept
{
    static BSFixedString speedSampledStr("SpeedSampled");
    animationGraphHolder.SetVariableFloat(&speedSampledStr, aSpeed);
}

uint16_t Actor::GetLevel() const noexcept
{
    TP_THIS_FUNCTION(TGetLevel, uint16_t, const Actor);
    POINTER_SKYRIMSE(TGetLevel, s_getLevel, 36344);
    return TiltedPhoques::ThisCall(s_getLevel, this);
}

void Actor::ForcePosition(const NiPoint3& acPosition) noexcept
{
    ScopedReferencesOverride recursionGuard;

    bool updateController = true;
    if (GetExtension()->IsRemote() && currentProcess)
    {
        if (auto* pController = currentProcess->GetCharController())
            updateController = pController->UpdateStepTiming();
    }

    SetPosition(acPosition, updateController);
}

GamePtr<Actor> Actor::LookupByHandle(uint32_t aHandle) noexcept
{
    GamePtr<TESObjectREFR> spRefr = TESObjectREFR::LookupByHandle(aHandle);
    if (!spRefr)
        return nullptr;

    if (Actor* pActor = Cast<Actor>(spRefr.Get()))
    {
        spRefr.Detach();
        return GamePtr<Actor>::Adopt(pActor);
    }

    return nullptr;
}

void Actor::ResetTeleportPhysics(float aNewZ) noexcept
{
    if (!currentProcess)
        return;

    if (auto* pController = currentProcess->GetCharController())
        pController->ResetTeleportPhysicsState(aNewZ);
}

void Actor::QueueUpdate() noexcept
{
    auto* pIniCollection = INISettingCollection::Get();
    auto* pSetting = pIniCollection ? pIniCollection->GetSetting("bUseFaceGenPreprocessedHeads:General") : nullptr;
    const auto originalValue = pSetting ? pSetting->data : 0u;
    if (pSetting)
        pSetting->data = 0;

    TP_THIS_FUNCTION(TQueueUpdate, void, Actor, bool);
    POINTER_SKYRIMSE(TQueueUpdate, QueueUpdate, 39181);

    TiltedPhoques::ThisCall(QueueUpdate, this, true);

    if (pSetting)
        pSetting->data = originalValue;
}

GamePtr<Actor> Actor::CreateInCell(
    TESNPC* apBaseForm,
    TESObjectCELL* apCell,
    TESWorldSpace* apWorldSpace,
    const NiPoint3& acPosition,
    const NiPoint3& acRotation) noexcept
{
    if (!apBaseForm || !apCell || !apCell->loadedCellData)
        return nullptr;

    auto pActor = New();
    if (!pActor)
        return nullptr;

    if ((apBaseForm->formID & 0xFF000000u) == 0xFF000000u)
    {
        apBaseForm->SetSkipSaveFlag(true);
        apBaseForm->UnsetChanged(0xFFFFFFFF);
    }

    pActor->SetSkipSaveFlag(true);
    pActor->GetExtension()->SetRemote(true);

    pActor->SetLevelMod(4);
    pActor->SetParentCell(apCell);
    pActor->SetBaseForm(apBaseForm);

    auto position = acPosition;
    auto rotation = acRotation;

    TESObjectCELL* pSpawnCell = (apCell->cellFlags[0] & 1) ? apCell : nullptr;
    ModManager::Get()->Spawn(position, rotation, pSpawnCell, apWorldSpace, pActor);

    pActor->ForcePosition(position);

    pActor->GetMagicCaster(MagicSystem::CastingSource::LEFT_HAND);
    pActor->GetMagicCaster(MagicSystem::CastingSource::RIGHT_HAND);
    pActor->GetMagicCaster(MagicSystem::CastingSource::OTHER);

    pActor->flags &= 0xFFDFFFFF;
    pActor->SetSkipSaveFlag(true);
    pActor->UnsetChanged(0xFFFFFFFF);

    return pActor;
}

GamePtr<Actor> Actor::Create(TESNPC* apBaseForm) noexcept
{
    if (!apBaseForm)
        return nullptr;

    const auto pPlayer = static_cast<Actor*>(GetById(0x14));
    if (!pPlayer || !pPlayer->parentCell || !pPlayer->parentCell->loadedCellData)
        return nullptr;

    return CreateInCell(apBaseForm, pPlayer->parentCell, pPlayer->GetWorldSpace(), pPlayer->position, pPlayer->rotation);
}

GamePtr<Actor> Actor::Spawn(uint32_t aBaseFormId) noexcept
{
    TESNPC* pNpc = Cast<TESNPC>(TESForm::GetById(aBaseFormId));
    if (!pNpc)
        return {};
    return Actor::Create(pNpc);
}

void Actor::SetLevelMod(uint32_t aLevel) noexcept
{
    TP_THIS_FUNCTION(TActorSetLevelMod, void, ExtraDataList, uint32_t);
    POINTER_SKYRIMSE(TActorSetLevelMod, realSetLevelMod, 11660);

    const auto pExtraDataList = &extraData;

    TiltedPhoques::ThisCall(realSetLevelMod, pExtraDataList, aLevel);
}

ActorExtension* Actor::GetExtension() noexcept
{
    if (AsExActor())
    {
        return static_cast<ActorExtension*>(AsExActor());
    }

    if (AsExPlayerCharacter())
    {
        return static_cast<ActorExtension*>(AsExPlayerCharacter());
    }

    return nullptr;
}

const ActorExtension* Actor::GetExtension() const noexcept
{
    return const_cast<Actor*>(this)->GetExtension();
}

ExActor* Actor::AsExActor() noexcept
{
    if (formType == Type && this != PlayerCharacter::Get())
        return static_cast<ExActor*>(this);

    return nullptr;
}

ExPlayerCharacter* Actor::AsExPlayerCharacter() noexcept
{
    if (this == PlayerCharacter::Get())
        return static_cast<ExPlayerCharacter*>(this);

    return nullptr;
}

TESNPC* Actor::GetLeveledTemplateBase() const noexcept
{
    TESNPC* pNpc = Cast<TESNPC>(baseForm);
    if (pNpc && !pNpc->IsTemporary())
        return pNpc;

    if (const auto* pExtraLev = static_cast<const ExtraLeveledCreature*>(extraData.GetByType(ExtraDataType::LeveledCreature)))
    {
        if (pExtraLev->npc1 && !pExtraLev->npc1->IsTemporary())
            return pExtraLev->npc1;
        if (pExtraLev->npc1)
        {
            if (TESNPC* pSource = TESNPC::GetLeveledSourceNpc(pExtraLev->npc1))
                return pSource;
            if (TESNPC* pTmpl = pExtraLev->npc1->GetTemplateBase())
                return pTmpl;
        }
        if (pExtraLev->npc2 && !pExtraLev->npc2->IsTemporary())
            return pExtraLev->npc2;
        if (pExtraLev->npc2)
        {
            if (TESNPC* pSource = TESNPC::GetLeveledSourceNpc(pExtraLev->npc2))
                return pSource;
            if (TESNPC* pTmpl = pExtraLev->npc2->GetTemplateBase())
                return pTmpl;
        }
    }

    if (pNpc)
    {
        if (TESNPC* pSource = TESNPC::GetLeveledSourceNpc(pNpc))
            return pSource;
        if (TESNPC* pTmpl = pNpc->GetTemplateBase())
            return pTmpl;
    }

    return nullptr;
}

extern thread_local bool g_forceAnimation;

void Actor::SetWeaponDrawnEx(bool aDraw) noexcept
{
    spdlog::debug("Setting weapon drawn: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());

    if (actorState.IsWeaponDrawn() == aDraw)
    {
        actorState.SetWeaponDrawn(!aDraw);

        spdlog::debug("Setting weapon drawn after update: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());
    }

    const bool prevForce = g_forceAnimation;
    g_forceAnimation = true;
    SetWeaponDrawn(aDraw);
    g_forceAnimation = prevForce;
}

static thread_local bool s_execInitPackage = false;

void Actor::SetPackage(TESPackage* apPackage) noexcept
{
    s_execInitPackage = true;
    PutCreatedPackage(apPackage);
    s_execInitPackage = false;
}

void Actor::SetPlayerRespawnMode(bool aSet) noexcept
{
    SetEssentialEx(aSet);
    // Makes the player go in an unrecoverable bleedout state
    SetNoBleedoutRecovery(aSet);

    if (formID != 0x14)
    {
        if (auto* pPlayerFaction = Cast<TESFaction>(TESForm::GetById(0xDB1)))
            SetFactionRank(pPlayerFaction, 1);
    }
}

void Actor::SetEssentialEx(bool aSet) noexcept
{
    SetEssential(aSet);
    TESNPC* pBase = Cast<TESNPC>(baseForm);
    if (pBase)
        pBase->actorData.SetEssential(aSet);
}

void Actor::SetNoBleedoutRecovery(bool aSet) noexcept
{
    TP_THIS_FUNCTION(TSetNoBleedoutRecovery, void, Actor, bool);
    POINTER_SKYRIMSE(TSetNoBleedoutRecovery, s_setNoBleedoutRecovery, 37583);
    TiltedPhoques::ThisCall(s_setNoBleedoutRecovery, this, aSet);
}

void Actor::DispelAllSpells(bool aNow) noexcept
{
    magicTarget.DispelAllSpells(aNow);
}

void Actor::DispelHostileEffects() noexcept
{
    // Selectively terminate only temporary hostile/detrimental DoTs (poisons, bleeds, fire/frost/shock burns)
    // while keeping 100% of long-duration RFAB food/alcohol/elixir buffs, shrine blessings, Mage Armor, and passives!
    if (!currentProcess || !currentProcess->middleProcess || !currentProcess->middleProcess->ActiveEffects)
        return;

    constexpr uint32_t kActiveEffectDispelledFlag = (1u << 18);
    for (ActiveEffect* pEffect : *currentProcess->middleProcess->ActiveEffects)
    {
        if (!pEffect || pEffect->fDuration <= 0.0f)
            continue;

        const auto* pEffectItem = static_cast<const EffectItem*>(pEffect->pEffect);
        const EffectSetting* pSetting = pEffectItem ? pEffectItem->pEffectSetting : nullptr;
        if (!pSetting)
            continue;

        const bool isHostileDoT = (pSetting->flags & EffectSetting::kHostile) != 0;
        if (isHostileDoT)
        {
            pEffect->uiFlags |= kActiveEffectDispelledFlag;
            pEffect->fElapsedSeconds = pEffect->fDuration + 1.0f;
        }
    }
}

bool Actor::IsInCombat() const noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, IsInCombat);
    return s_pIsInCombat(this);
}

Actor* Actor::GetCombatTarget() const noexcept
{
    PAPYRUS_FUNCTION(Actor*, Actor, GetCombatTarget);
    return s_pGetCombatTarget(this);
}

// TODO: this is a really hacky solution.
// The internal targeting system should be disabled instead.
void Actor::StartCombatEx(Actor* apTarget) noexcept
{
    if (GetCombatTarget() != apTarget)
    {
        StopCombat();
        StartCombat(apTarget);
    }
}

void Actor::SetCombatTargetEx(Actor* apTarget) noexcept
{
    if (pCombatController)
        pCombatController->SetTarget(apTarget);
}

void Actor::StartCombat(Actor* apTarget) noexcept
{
    PAPYRUS_FUNCTION(void, Actor, StartCombat, Actor*);
    s_pStartCombat(this, apTarget);
}

void Actor::StopCombat() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, StopCombat);
    if (s_pStopCombat)
        s_pStopCombat(this);
}

bool Actor::IsHostileToActor(Actor* apTarget) const noexcept
{
    if (!apTarget || IsDeleted() || apTarget->IsDeleted())
        return false;

    PAPYRUS_FUNCTION(bool, Actor, IsHostileToActor, Actor*);
    if (!s_pIsHostileToActor)
        return false;

    return s_pIsHostileToActor(this, apTarget);
}

void Actor::EvaluatePackage() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, EvaluatePackage);
    if (s_pEvaluatePackage)
        s_pEvaluatePackage(this);
}

bool Actor::RemoveSpell(MagicItem* apSpell) noexcept
{
    if (!apSpell)
    {
        spdlog::error(__FUNCTION__ ": apSpell is null");
        return false;
    }

    bool removed = TiltedPhoques::ThisCall(RealRemoveSpell, this, apSpell);

    PAPYRUS_FUNCTION(bool, Actor, DispelSpell, MagicItem*);
    if (s_pDispelSpell)
        removed = s_pDispelSpell(this, apSpell) || removed;

    // Also mark any matching ActiveEffects in MiddleProcess::ActiveEffects as dispelled (kDispelled = 1 << 18)
    // so CONSTANT_EFFECT stances, toggle auras, and buffs added via MagicTarget::AddTarget on remote player clones
    // are guaranteed to terminate immediately when toggled off!
    if (currentProcess && currentProcess->middleProcess && currentProcess->middleProcess->ActiveEffects)
    {
        constexpr uint32_t kActiveEffectDispelledFlag = (1u << 18);
        for (ActiveEffect* pEffect : *currentProcess->middleProcess->ActiveEffects)
        {
            if (pEffect && pEffect->pSpell == apSpell)
            {
                pEffect->uiFlags |= kActiveEffectDispelledFlag;
                if (pEffect->fDuration > 0.0f)
                    pEffect->fElapsedSeconds = pEffect->fDuration + 1.0f;
                removed = true;
            }
        }
    }

    return removed;
}

bool Actor::HasPerk(uint32_t aPerkFormId) const noexcept
{
    return GetPerkRank(aPerkFormId) != 0;
}

uint8_t Actor::GetPerkRank(uint32_t aPerkFormId) const noexcept
{
    BGSPerk* pPerk = Cast<BGSPerk>(TESForm::GetById(aPerkFormId));
    if (!pPerk)
        return 0;

    TP_THIS_FUNCTION(TGetPerkRank, uint8_t, const Actor, BGSPerk*);
    POINTER_SKYRIMSE(TGetPerkRank, getPerkRank, 36690);

    return TiltedPhoques::ThisCall(getPerkRank, this, pPerk);
}

bool TP_MAKE_THISCALL(HookRemoveSpell, Actor, MagicItem* apSpell)
{
    if (!apThis || !apSpell || apThis->IsDeleted())
        return false;

    bool result = TiltedPhoques::ThisCall(RealRemoveSpell, apThis, apSpell);
    if (result && apThis && apSpell && World::IsCreated() && World::IsConnectedCached())
    {
        if (const auto* pExt = apThis->GetExtension(); pExt && pExt->IsLocalPlayer())
        {
            RemoveSpellEvent removalEvent;
            removalEvent.TargetId = apThis->formID;
            removalEvent.SpellId = apSpell->formID;
            World::Get().GetRunner().Trigger(removalEvent);
        }
    }

    return result;
}

Actor* TP_MAKE_THISCALL(HookCharacterConstructor, Actor)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    TiltedPhoques::ThisCall(RealCharacterConstructor, apThis);

    return apThis;
}

Actor* TP_MAKE_THISCALL(HookCharacterConstructor2, Actor, uint8_t aUnk)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    TiltedPhoques::ThisCall(RealCharacterConstructor2, apThis, aUnk);

    return apThis;
}

Actor* TP_MAKE_THISCALL(HookCharacterDestructor, Actor)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    auto pExtension = apThis->GetExtension();

    if (pExtension)
    {
        pExtension->~ActorExtension();
    }

    TiltedPhoques::ThisCall(RealCharacterDestructor, apThis);

    return apThis;
}

GamePtr<Actor> Actor::New() noexcept
{
    auto* const pActor = Memory::Allocate<Actor>();

    TiltedPhoques::ThisCall(RealCharacterConstructor, pActor);

    return {pActor};
}

void Actor::InterruptCast(bool abRefund) noexcept
{
    TP_THIS_FUNCTION(TInterruptCast, void, Actor, bool abRefund);

    POINTER_SKYRIMSE(TInterruptCast, s_interruptCast, 37808);

    TiltedPhoques::ThisCall(s_interruptCast, this, abRefund);
}

TESForm* Actor::GetEquippedWeapon(uint32_t aSlotId) const noexcept
{
    if (currentProcess && currentProcess->middleProcess)
    {
        auto pMiddleProcess = currentProcess->middleProcess;

        if (aSlotId == 0 && pMiddleProcess->leftEquippedObject)
            return pMiddleProcess->leftEquippedObject->pObject;

        else if (aSlotId == 1 && pMiddleProcess->rightEquippedObject)
            return pMiddleProcess->rightEquippedObject->pObject;
    }

    return nullptr;
}

TESForm* Actor::GetEquippedAmmo() const noexcept
{
    if (currentProcess && currentProcess->middleProcess && currentProcess->middleProcess->ammoEquippedObject)
    {
        // TODO: rtti cast to check if is ammo object? or actually, just call AIProcess::GetCurrentAmmo()
        return currentProcess->middleProcess->ammoEquippedObject->pObject;
    }

    return nullptr;
}

bool Actor::IsWearingBodyPiece() const noexcept
{
    return GetContainerChanges()->GetArmor(32) != nullptr;
}

namespace
{
void CollectArmorFromOutfitForm(
    TESForm* apForm,
    uint32_t aDeterministicSeed,
    int aDepth,
    TiltedPhoques::Vector<TESObjectARMO*>& aOutArmors) noexcept
{
    if (!apForm || aDepth > 8)
        return;

    if (apForm->formType == FormType::Armor)
    {
        if (auto* pArmor = Cast<TESObjectARMO>(apForm))
            aOutArmors.push_back(pArmor);
        return;
    }

    if (apForm->formType == FormType::LeveledItem)
    {
        auto* pLevItem = Cast<TESLevItem>(apForm);
        const uint8_t flags = pLevItem ? pLevItem->TESLeveledList::unk10[1] : 0;      // offset 0x11 in TESLeveledList
        const uint8_t entryCount = pLevItem ? pLevItem->TESLeveledList::unk10[2] : 0; // offset 0x12 in TESLeveledList
        if (!pLevItem || !pLevItem->pLeveledListA || entryCount == 0)
            return;

        // TESLeveledList::kUseAll == 1 << 2 (0x04): Requiem / RFAB full armor-set sublists use UseAll
        // to grant cuirass + boots + gauntlets + helmet together.
        const bool useAll = (flags & 0x04u) != 0;
        if (useAll)
        {
            for (uint8_t i = 0; i < entryCount; ++i)
            {
                CollectArmorFromOutfitForm(
                    pLevItem->pLeveledListA[i].pForm,
                    aDeterministicSeed + static_cast<uint32_t>(i) * 2654435761u,
                    aDepth + 1,
                    aOutArmors);
            }
            return;
        }

        // Deterministic pick seeded by actor baseForm/formID so Host and Friend materialize identical armor.
        const uint32_t count = static_cast<uint32_t>(entryCount);
        const uint32_t startIdx = aDeterministicSeed % count;
        for (uint32_t offset = 0; offset < count; ++offset)
        {
            const uint32_t idx = (startIdx + offset) % count;
            TESForm* pSubForm = pLevItem->pLeveledListA[idx].pForm;
            if (!pSubForm)
                continue;

            const size_t before = aOutArmors.size();
            CollectArmorFromOutfitForm(
                pSubForm,
                aDeterministicSeed ^ (idx * 2246822519u + 1u),
                aDepth + 1,
                aOutArmors);
            if (aOutArmors.size() > before)
                return;
        }
    }
}
} // namespace

bool Actor::ShouldWearBodyPiece() const noexcept
{
    TESNPC* pBase = Cast<TESNPC>(baseForm);
    if (!pBase)
        return false;

    BGSOutfit* pDefaultOutfit = pBase->outfits[0];
    if (!pDefaultOutfit)
        return false;

    const uint32_t deterministicSeed = pBase->formID != 0 ? pBase->formID : formID;
    TiltedPhoques::Vector<TESObjectARMO*> resolvedArmors;
    for (auto* pItem : pDefaultOutfit->outfitItems)
    {
        CollectArmorFromOutfitForm(pItem, deterministicSeed, 0, resolvedArmors);
    }

    for (TESObjectARMO* pArmor : resolvedArmors)
    {
        if (pArmor && pArmor->IsBodyPiece())
            return true;
    }

    return false;
}

void Actor::SetOutfit(BGSOutfit* apOutfit, bool aIsSleepOutfit) 
{
    PAPYRUS_FUNCTION(void, Actor, SetOutfit, const BGSOutfit*, bool);
    s_pSetOutfit(this, apOutfit, aIsSleepOutfit);
}

void Actor::EquipOutfit(bool aIsSleepOutfit) noexcept
{
    auto* pBase = Cast<TESNPC>(baseForm);
    if (!pBase)
        return;

    BGSOutfit* pDefaultOutfit = pBase->outfits[aIsSleepOutfit ? 1 : 0];
    if (!pDefaultOutfit)
        return;

    auto* pEquipManager = EquipManager::Get();
    if (!pEquipManager)
        return;

    const uint32_t deterministicSeed = pBase->formID != 0 ? pBase->formID : formID;
    TiltedPhoques::Vector<TESObjectARMO*> resolvedArmors;
    for (auto* pItem : pDefaultOutfit->outfitItems)
    {
        CollectArmorFromOutfitForm(pItem, deterministicSeed, 0, resolvedArmors);
    }

    ScopedInventoryOverride invGuard;
    ScopedEquipOverride equipGuard;

    for (TESObjectARMO* pArmor : resolvedArmors)
    {
        if (!pArmor)
            continue;

        // SetActorInventory() calls RemoveAllItems(), so always restore 1 item in ExtraContainerChanges
        // under ScopedInventoryOverride before invoking EquipManager::Equip().
        // NOTE: in real SkyrimSE.exe TESObjectARMO IS a TESBoundObject subclass (the simplified RE model here
        // only derives it from TESForm), so this reinterpret_cast is ABI-safe for the native call.
        if (RealAddInventoryItem)
            TiltedPhoques::ThisCall(RealAddInventoryItem, this, reinterpret_cast<TESBoundObject*>(pArmor), nullptr, 1, nullptr);

        spdlog::debug(__FUNCTION__ ": equipping {:X} on actor {:X} {}", pArmor->formID, formID, baseForm->GetName());
        pEquipManager->Equip(this, pArmor, nullptr, 1, nullptr, false, true, false, false);
    }
}

// Get owner of a summon or raised corpse
Actor* Actor::GetCommandingActor() const noexcept
{
    if (currentProcess && currentProcess->middleProcess && currentProcess->middleProcess->commandingActor)
    {
        auto handle = currentProcess->middleProcess->commandingActor.handle;
        auto* pOwner = Cast<Actor>(TESObjectREFR::GetByHandle(handle.iBits));
        return pOwner;
    }

    return nullptr;
}

TESObjectREFR* Actor::GetOccupiedFurniture() const noexcept
{
    if (currentProcess && currentProcess->middleProcess && currentProcess->middleProcess->occupiedFurniture)
    {
        auto handle = currentProcess->middleProcess->occupiedFurniture.handle;
        return TESObjectREFR::GetByHandle(handle.iBits);
    }

    return nullptr;
}

bool Actor::HasValidNearbyFurniture(float aMaxDistance) const noexcept
{
    if (!currentProcess || !currentProcess->middleProcess || !currentProcess->middleProcess->occupiedFurniture)
        return false;

    const GamePtr<TESObjectREFR> spFurniture = TESObjectREFR::LookupByHandle(currentProcess->middleProcess->occupiedFurniture);
    const TESObjectREFR* pFurniture = spFurniture.Get();
    if (!pFurniture || pFurniture->IsDisabled() || pFurniture->IsDeleted())
        return false;

    const float dx = pFurniture->position.x - position.x;
    const float dy = pFurniture->position.y - position.y;
    const float dz = pFurniture->position.z - position.z;
    return (dx * dx + dy * dy + dz * dz) <= (aMaxDistance * aMaxDistance);
}

// Get owner of a summon or raised corpse
void Actor::SetCommandingActor(BSPointerHandle<TESObjectREFR> aCommandingActor) noexcept
{
    if (currentProcess && currentProcess->middleProcess)
    {
        currentProcess->middleProcess->commandingActor = aCommandingActor;
        flags2 |= ActorFlags::IS_COMMANDED_ACTOR;
    }
}

bool Actor::IsPlayerSummon() const noexcept
{
    if (const auto* pExt = GetExtension())
    {
        if (pExt->IsPlayerSummonFlag())
            return true;

        // RFAB spirit/toggle summons (RFAB_Script_SummonSpirit) spawn as temporary 0xFFxxxxxx PlayerTeammates
        // with GetCommandingActor() == nullptr on the summoner's client.
        if (!pExt->IsRemote() && !pExt->IsPlayer() && (formID & 0xFF000000u) == 0xFF000000u && IsPlayerTeammate())
            return true;
    }

    const Actor* pCommandingActor = GetCommandingActor();
    if (!pCommandingActor)
        return false;

    const auto* pCmdExt = pCommandingActor->GetExtension();
    return pCommandingActor->formID == 0x14 || (pCmdExt && pCmdExt->IsRemotePlayer());
}

TESForm* Actor::GetCurrentLocation()
{
    // we use the safe function which also
    // checks the form type
    return FUNC_GetActorLocation(this);
}

Factions Actor::GetFactions() const noexcept
{
    Factions result;

    auto& modSystem = World::Get().GetModSystem();

    auto* pNpc = Cast<TESNPC>(baseForm);
    if (pNpc)
    {
        const TESNPC* pFactionSourceNpc = pNpc;
        if (pFactionSourceNpc->actorData.factions.length == 0)
        {
            if (const TESNPC* pTemplateNpc = GetLeveledTemplateBase();
                pTemplateNpc && pTemplateNpc->actorData.factions.length > 0)
            {
                pFactionSourceNpc = pTemplateNpc;
            }
        }

        const auto& factions = pFactionSourceNpc->actorData.factions;

        for (auto i = 0u; i < factions.length; ++i)
        {
            if (!factions[i].faction)
                continue;

            Faction faction;

            modSystem.GetServerModId(factions[i].faction->formID, faction.Id);
            faction.Rank = factions[i].rank;

            result.NpcFactions.push_back(faction);
        }
    }

    auto* pChanges = Cast<ExtraFactionChanges>(extraData.GetByType(ExtraDataType::Faction));
    if (pChanges)
    {
        for (auto i = 0u; i < pChanges->entries.length; ++i)
        {
            if (!pChanges->entries[i].faction)
                continue;

            Faction faction;

            modSystem.GetServerModId(pChanges->entries[i].faction->formID, faction.Id);
            faction.Rank = pChanges->entries[i].rank;

            result.ExtraFactions.push_back(faction);
        }
    }

    return result;
}

ActorValues Actor::GetEssentialActorValues() const noexcept
{
    ActorValues actorValues;

    const int essentialPools[] = {ActorValueInfo::kHealth, ActorValueInfo::kStamina, ActorValueInfo::kMagicka};
    for (int i : essentialPools)
    {
        const float value = actorValueOwner.GetValue(i);
        actorValues.ActorValuesList.insert({i, value});
        const float maxValue = actorValueOwner.GetPermanentValue(i);
        actorValues.ActorMaxValuesList.insert({i, maxValue});
    }

    // Sync armor rating (kDamageResist = 39) & magic/elemental resistances (40..44) via PERMANENT so
    // RFAB.dll ProcessHit / Shield Bash flat-damage armor mitigation and spell damage match across peers!
    const int defenseValues[] = {
        ActorValueInfo::kDamageResist,
        ActorValueInfo::kPoisonResist,
        ActorValueInfo::kFireResist,
        ActorValueInfo::kElectricResist,
        ActorValueInfo::kFrostResist,
        ActorValueInfo::kMagicResist};
    for (int av : defenseValues)
    {
        actorValues.ActorMaxValuesList.insert({av, actorValueOwner.GetValue(av)});
    }

    return actorValues;
}

float Actor::GetActorValue(uint32_t aId) const noexcept
{
    return actorValueOwner.GetValue(aId);
}

float Actor::GetActorPermanentValue(uint32_t aId) const noexcept
{
    return actorValueOwner.GetPermanentValue(aId);
}

void Actor::SetActorValue(uint32_t aId, float aValue) noexcept
{
    actorValueOwner.SetValue(aId, aValue);
}

void Actor::ForceActorValue(ActorValueOwner::ForceMode aMode, uint32_t aId, float aValue) noexcept
{
    float initialValue = aMode == ActorValueOwner::ForceMode::PERMANENT 
                         ? GetActorPermanentValue(aId)
                         : GetActorValue(aId);

    if (aValue == initialValue)
        return;

    actorValueOwner.ForceCurrent(aMode, aId, aValue - initialValue);
}

Inventory Actor::GetActorInventory() const noexcept
{
    Inventory inventory = GetInventory();

    inventory.CurrentMagicEquipment = GetMagicEquipment();

    return inventory;
}

MagicEquipment Actor::GetMagicEquipment() const noexcept
{
    MagicEquipment equipment;

    auto& modSystem = World::Get().GetModSystem();

    uint32_t mainId = magicItems[0] ? magicItems[0]->formID : 0;
    modSystem.GetServerModId(mainId, equipment.LeftHandSpell);

    uint32_t secondaryId = magicItems[1] ? magicItems[1]->formID : 0;
    modSystem.GetServerModId(secondaryId, equipment.RightHandSpell);

    uint32_t shoutId = equippedShout ? equippedShout->formID : 0;
    modSystem.GetServerModId(shoutId, equipment.Shout);

    return equipment;
}

Inventory Actor::GetEquipment() const noexcept
{
    Inventory inventory = GetInventory();
    inventory.RemoveByFilter([](const auto& entry) { return !entry.IsWorn(); });
    inventory.CurrentMagicEquipment = GetMagicEquipment();
    return inventory;
}

int32_t Actor::GetGoldAmount() const noexcept
{
    TP_THIS_FUNCTION(TGetGoldAmount, int32_t, const Actor);
    POINTER_SKYRIMSE(TGetGoldAmount, s_getGoldAmount, 36527);
    return TiltedPhoques::ThisCall(s_getGoldAmount, this);
}

void Actor::SetActorInventory(const Inventory& acInventory) noexcept
{
    Inventory currentInventory = GetActorInventory();
    if (currentInventory == acInventory)
    {
        spdlog::debug("Skipping redundant SetActorInventory for actor {:X} (inventory already matches)", formID);
        return;
    }

    spdlog::info("Setting inventory differentially for actor {:X}", formID);

    // SkyMP-style differential inventory reconciliation keeps unchanged items and already-worn
    // Requiem/RFAB Fortify-Health gear untouched in ExtraContainerChanges (no RemoveAllItems() strip
    // and no +4096 HP spike needed). Preserve relative health ratio only if MaxHealth genuinely changed.
    const bool wasAlive = !IsDead();
    const float preHealth = wasAlive ? GetActorValue(ActorValueInfo::kHealth) : 0.0f;
    const float preMaxHealth = wasAlive ? GetActorPermanentValue(ActorValueInfo::kHealth) : 0.0f;
    const float preRatio = (wasAlive && preMaxHealth > 0.0f)
        ? std::clamp(preHealth / preMaxHealth, 0.05f, 1.0f)
        : 1.0f;

    const auto* pSelfExt = this->GetExtension();
    if ((!pSelfExt || !pSelfExt->IsPlayer()) && currentInventory.ContainsQuestItems())
        SetInventoryRetainingQuestItems(currentInventory, acInventory);
    else
        SetInventory(acInventory);

    SetMagicEquipment(acInventory.CurrentMagicEquipment);

    if (wasAlive && preHealth > 0.0f)
    {
        const float postMaxHealth = GetActorPermanentValue(ActorValueInfo::kHealth);
        if (std::fabs(postMaxHealth - preMaxHealth) > 0.5f || GetActorValue(ActorValueInfo::kHealth) <= 0.0f)
        {
            ScopedActorValueOverride avOverride;
            const float restoredHealth = (postMaxHealth > 0.0f)
                ? std::max(1.0f, postMaxHealth * preRatio)
                : std::max(1.0f, preHealth);
            ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, restoredHealth);
        }
    }
}

void Actor::SetMagicEquipment(const MagicEquipment& acEquipment) noexcept
{
    auto* pEquipManager = EquipManager::Get();
    if (!pEquipManager || !World::IsCreated())
        return;

    ScopedEquipOverride equipOverride;
    auto& modSystem = World::Get().GetModSystem();

    if (acEquipment.LeftHandSpell)
    {
        const uint32_t leftSpellId = modSystem.GetGameId(acEquipment.LeftHandSpell);
        spdlog::debug("Setting left hand spell: {:X}", leftSpellId);
        if (SpellItem* pLeftSpell = Cast<SpellItem>(TESForm::GetById(leftSpellId)))
            pEquipManager->EquipSpell(this, pLeftSpell, 0);
    }
    else if (magicItems[0])
    {
        pEquipManager->UnEquipSpell(this, magicItems[0], 0);
    }

    if (acEquipment.RightHandSpell)
    {
        const uint32_t rightSpellId = modSystem.GetGameId(acEquipment.RightHandSpell);
        spdlog::debug("Setting right hand spell: {:X}", rightSpellId);
        if (SpellItem* pRightSpell = Cast<SpellItem>(TESForm::GetById(rightSpellId)))
            pEquipManager->EquipSpell(this, pRightSpell, 1);
    }
    else if (magicItems[1])
    {
        pEquipManager->UnEquipSpell(this, magicItems[1], 1);
    }

    if (acEquipment.Shout)
    {
        const uint32_t shoutId = modSystem.GetGameId(acEquipment.Shout);
        spdlog::debug("Setting shout: {:X}", shoutId);
        if (TESShout* pShout = Cast<TESShout>(TESForm::GetById(shoutId)))
            pEquipManager->EquipShout(this, pShout);
    }
    else if (equippedShout)
    {
        pEquipManager->UnEquipShout(this, equippedShout);
    }
}

void Actor::SetActorValues(const ActorValues& acActorValues) noexcept
{
    ScopedActorValueOverride avOverride;
    auto isLocallyDerivedSpeedAv = [](uint32_t aAvId) constexpr noexcept
    {
        return aAvId == ActorValueInfo::kSpeedMult ||
               aAvId == ActorValueInfo::kWeaponSpeedMult ||
               aAvId == ActorValueInfo::kLeftWeaponSpeedMult ||
               aAvId == ActorValueInfo::kInfamy;
    };

    const bool keepAlive = !IsDead();
    const auto maxHpIt = acActorValues.ActorMaxValuesList.find(ActorValueInfo::kHealth);
    const auto curHpIt = acActorValues.ActorValuesList.find(ActorValueInfo::kHealth);
    const bool hasHealthPair = (maxHpIt != acActorValues.ActorMaxValuesList.end() || curHpIt != acActorValues.ActorValuesList.end());

    if (hasHealthPair)
    {
        const float currentPermMax = GetActorPermanentValue(ActorValueInfo::kHealth);
        const float desiredPermMax = (maxHpIt != acActorValues.ActorMaxValuesList.end()) ? maxHpIt->second : currentPermMax;
        const float desiredCurHp = (curHpIt != acActorValues.ActorValuesList.end()) ? curHpIt->second : GetActorValue(ActorValueInfo::kHealth);
        const auto hpPlan = CoopOwnership::ComputeSafeHealthStepPlan(
            currentPermMax,
            desiredPermMax,
            desiredCurHp,
            keepAlive && desiredCurHp > 0.0f);

        if (hpPlan.AdjustDamageBeforePermanent)
        {
            if (curHpIt != acActorValues.ActorValuesList.end())
                ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, hpPlan.DamageModeTarget);
            if (maxHpIt != acActorValues.ActorMaxValuesList.end())
                ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, ActorValueInfo::kHealth, hpPlan.PermanentModeTarget);
        }
        else
        {
            if (maxHpIt != acActorValues.ActorMaxValuesList.end())
                ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, ActorValueInfo::kHealth, hpPlan.PermanentModeTarget);
            if (curHpIt != acActorValues.ActorValuesList.end())
                ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, hpPlan.DamageModeTarget);
        }
    }

    for (auto& value : acActorValues.ActorMaxValuesList)
    {
        if (isLocallyDerivedSpeedAv(value.first) || value.first == ActorValueInfo::kHealth)
            continue;

        ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, value.first, value.second);
    }

    for (auto& value : acActorValues.ActorValuesList)
    {
        if (isLocallyDerivedSpeedAv(value.first) || value.first == ActorValueInfo::kHealth)
            continue;

        ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, value.first, value.second);
    }
}

void Actor::SetFactions(const Factions& acFactions) noexcept
{
    const auto* pExt = GetExtension();
    const bool isRemotePlayer = pExt && pExt->IsRemotePlayer();
    const bool isLocalPlayer = pExt && pExt->IsLocalPlayer();

    // NEVER call `RemoveFromAllFactions()` on world NPCs (`Bandits`, `Draugr`, `Vampires`, `Wildlife`, bosses):
    // Skyrim NPCs inherit factions from `TESActorBaseData::kUseFactions` (`npcTemplate`) AND dynamic `BGSRefAlias`
    // quest aliases, neither of which appears in `pNpc->actorData.factions`. Calling `RemoveFromAllFactions()`
    // appended `-1` removal overrides into `ExtraFactionChanges`, permanently turning hostile enemies peaceful
    // toward all players until console `resurrect` cleared `ExtraFactionChanges`!
    if (!CoopWorldStabilizer::ShouldPreserveExistingNpcFactionsOnSync(isRemotePlayer, isLocalPlayer))
        RemoveFromAllFactions();

    auto& modSystem = World::Get().GetModSystem();

    for (auto& entry : acFactions.NpcFactions)
    {
        auto pForm = GetById(modSystem.GetGameId(entry.Id));
        auto pFaction = Cast<TESFaction>(pForm);
        if (pFaction)
        {
            SetFactionRank(pFaction, entry.Rank);
        }
    }

    for (auto& entry : acFactions.ExtraFactions)
    {
        auto pForm = GetById(modSystem.GetGameId(entry.Id));
        auto pFaction = Cast<TESFaction>(pForm);
        if (pFaction)
        {
            SetFactionRank(pFaction, entry.Rank);
        }
    }

    // Guarantee every Remote Player Clone carries `PlayerFaction` (`0x00000DB1`, rank `0`) so faction-hostile
    // enemies (`Enemy` to `PlayerFaction`) treat all party players symmetrically rather than ignoring one peer.
    if (isRemotePlayer)
    {
        if (const auto* pPlayerFaction = Cast<TESFaction>(GetById(0x00000DB1u)))
            SetFactionRank(pPlayerFaction, 0);
    }
}

void Actor::SetFactionRank(const TESFaction* apFaction, int8_t aRank) noexcept
{
    TP_THIS_FUNCTION(TSetFactionRankInternal, void, Actor, const TESFaction*, int8_t);

    POINTER_SKYRIMSE(TSetFactionRankInternal, s_setFactionRankInternal, 36669);

    TiltedPhoques::ThisCall(s_setFactionRankInternal, this, apFaction, aRank);
}

void Actor::SetPlayerTeammate(bool aSet) noexcept
{
    TP_THIS_FUNCTION(TSetPlayerTeammate, void, Actor, bool aSet, bool abCanDoFavor);
    POINTER_SKYRIMSE(TSetPlayerTeammate, setPlayerTeammate, 36707);
    return TiltedPhoques::ThisCall(setPlayerTeammate, this, aSet, true);
}

void Actor::UnEquipAll() noexcept
{
    EquipManager::Get()->UnequipAll(this);

    // Taken from skyrim's code shouts can be two form types apparently
    if (equippedShout && ((int)equippedShout->formType - 41) <= 1)
    {
        EquipManager::Get()->UnEquipShout(this, equippedShout);
        equippedShout = nullptr;
    }
}

void Actor::RemoveFromAllFactions() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, RemoveFromAllFactions);

    s_pRemoveFromAllFactions(this);
}

TP_THIS_FUNCTION(TInitiateMountPackage, bool, Actor, Actor* apMount);
static TInitiateMountPackage* RealInitiateMountPackage = nullptr;

bool Actor::InitiateMountPackage(Actor* apMount) noexcept
{
    return TiltedPhoques::ThisCall(RealInitiateMountPackage, this, apMount);
}

void Actor::GenerateMagicCasters() noexcept
{
    using CS = MagicSystem::CastingSource;

    for (int i = 0; i < 4; i++)
    {
        if (casters[i] == nullptr)
            casters[i] = Cast<ActorMagicCaster>(GetMagicCaster(static_cast<CS>(i)));
    }
}

bool Actor::IsDead() const noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, IsDead);

    return s_pIsDead(this);
}

bool Actor::IsDragon() const noexcept
{
    const ActorExtension* pExtension = const_cast<Actor*>(this)->GetExtension();
    return BehaviorVar::IsDragon(pExtension->GraphDescriptorHash);
}

bool Actor::IsGhost() const noexcept
{
    if (const auto* pExtraGhost = static_cast<const ExtraGhost*>(extraData.GetByType(ExtraDataType::Ghost)))
        return pExtraGhost->ghost;

    if (const auto* pActorBase = Cast<TESActorBase>(baseForm))
    {
        if (pActorBase->actorData.IsGhostBase() || pActorBase->actorData.IsInvulnerableBase())
            return true;
    }

    return false;
}

void Actor::SetGhost(bool aIsGhost) noexcept
{
    if (auto* pExtraGhost = static_cast<ExtraGhost*>(extraData.GetByType(ExtraDataType::Ghost)))
        pExtraGhost->ghost = aIsGhost;

    PAPYRUS_FUNCTION(void, Actor, SetGhost, bool);
    if (s_pSetGhost)
        s_pSetGhost(this, aIsGhost);
}

void Actor::Kill(Actor* apKiller) noexcept
{
    // Never kill players
    ActorExtension* pExtension = GetExtension();
    if (pExtension->IsPlayer())
        return;

    KillImpl(apKiller, 100.f, true, true);
}

void Actor::Reset() noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(void, ObjectReference, Reset, int, TESObjectREFR*);

    s_pReset(this, 0, nullptr);
}

bool Actor::PlayIdle(TESIdleForm* apIdle) noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, PlayIdle, TESIdleForm*);
    return s_pPlayIdle(this, apIdle);
}

void Actor::Respawn() noexcept
{
    Resurrect(false);
    Reset();
}

void Actor::ResurrectInPlace() noexcept
{
    // Resurrects the existing placed/dynamic Actor IN PLACE (`aResetInventory = false`) WITHOUT calling
    // `ObjectReference::Reset()`, avoiding editor-marker teleports, inventory wipes, and `TESLevCharacter`
    // creature template re-rolls!
    ScopedInventoryOverride invOverride;
    ScopedEquipOverride equipOverride;
    ScopedActorValueOverride avOverride;
    Resurrect(false);
}

namespace
{
CoopDamagePipeline::ScriptDamageAuthLedger<64> s_scriptDamageAuthLedger{};
thread_local bool s_inApplyActorEffect = false;
thread_local bool s_inLocalCasterConcentrationEffect = false;
thread_local uint32_t s_localConcentrationCasterFormId = 0;
thread_local Actor* s_remoteHitReactionTarget = nullptr;
thread_local float s_capturedHitStaggerMag = 0.0f;

[[nodiscard]] uint64_t NowActorSteadyMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct ScopedApplyActorEffectGuard
{
    explicit ScopedApplyActorEffectGuard(bool aLocalConcentration = false, uint32_t aCasterFormId = 0) noexcept
        : m_prev(s_inApplyActorEffect)
        , m_prevLocalConc(s_inLocalCasterConcentrationEffect)
        , m_prevCasterId(s_localConcentrationCasterFormId)
    {
        s_inApplyActorEffect = true;
        s_inLocalCasterConcentrationEffect = aLocalConcentration;
        s_localConcentrationCasterFormId = aLocalConcentration ? aCasterFormId : 0;
    }
    ~ScopedApplyActorEffectGuard() noexcept
    {
        s_inApplyActorEffect = m_prev;
        s_inLocalCasterConcentrationEffect = m_prevLocalConc;
        s_localConcentrationCasterFormId = m_prevCasterId;
    }
    bool m_prev{false};
    bool m_prevLocalConc{false};
    uint32_t m_prevCasterId{0};
};

struct ScopedRemoteHitReactionCapture
{
    explicit ScopedRemoteHitReactionCapture(Actor* apTarget) noexcept
        : m_prevTarget(s_remoteHitReactionTarget)
        , m_prevMag(s_capturedHitStaggerMag)
    {
        s_remoteHitReactionTarget = apTarget;
        s_capturedHitStaggerMag = 0.0f;
    }
    ~ScopedRemoteHitReactionCapture() noexcept
    {
        s_remoteHitReactionTarget = m_prevTarget;
        s_capturedHitStaggerMag = m_prevMag;
    }
    Actor* m_prevTarget{nullptr};
    float m_prevMag{0.0f};
};
} // namespace

void Actor::ArmLocalPlayerScriptDamageWindow(uint32_t aTargetFormId, uint64_t aDurationMs) noexcept
{
    s_scriptDamageAuthLedger.ArmWindow(aTargetFormId, NowActorSteadyMs(), aDurationMs);
}

bool Actor::HasLocalPlayerScriptDamageWindow(uint32_t aTargetFormId) noexcept
{
    return s_scriptDamageAuthLedger.IsAuthorized(aTargetFormId, NowActorSteadyMs());
}

bool Actor::TryAcceptRemoteHitReactionAction(Actor* apActor, const char* acActionKeyword) noexcept
{
    if (!apActor || apActor != s_remoteHitReactionTarget || !acActionKeyword || acActionKeyword[0] == '\0')
        return false;

    if (std::strncmp(acActionKeyword, "ActionStagger", 13) == 0)
    {
        s_capturedHitStaggerMag = std::max(s_capturedHitStaggerMag, 0.35f);
        return true;
    }
    if (std::strncmp(acActionKeyword, "ActionRecoilLarge", 17) == 0)
    {
        s_capturedHitStaggerMag = std::max(s_capturedHitStaggerMag, 0.25f);
        return true;
    }
    if (std::strncmp(acActionKeyword, "ActionRecoil", 12) == 0)
    {
        s_capturedHitStaggerMag = std::max(s_capturedHitStaggerMag, 0.15f);
        return true;
    }
    return false;
}

bool Actor::IsVampireLord() const noexcept
{
    return race && race->formID == 0x200283A;
}

extern thread_local bool g_forceAnimation;

void Actor::FixVampireLordModel() noexcept
{
    TESBoundObject* pLordArmor = Cast<TESBoundObject>(TESForm::GetById(0x2011a84));
    if (!pLordArmor)
        return;

    {
        ScopedInventoryOverride _;
        AddObjectToContainer(pLordArmor, nullptr, 1, nullptr);
    }

    EquipManager::Get()->Equip(this, pLordArmor, nullptr, 1, nullptr, false, true, false, false);

    const bool prevForce = g_forceAnimation;
    g_forceAnimation = true;

    BSFixedString str("isLevitating");
    uint32_t isLevitating = GetAnimationVariableInt(&str);
    spdlog::critical("isLevitating {}", isLevitating);

    // By default, a loaded vampire lord is not levitating.
    if (isLevitating)
    {
        BSFixedString levitation("LevitationToggle");
        SendAnimationEvent(&levitation);
    }

    // TODO: weapon draw code does not seem to take care of this
    //BSFixedString weapEquip("WeapEquip");
    //SendAnimationEvent(&weapEquip);

    g_forceAnimation = prevForce;
}

char TP_MAKE_THISCALL(HookSetPosition, Actor, NiPoint3& aPosition)
{
    const auto pExtension = apThis ? apThis->GetExtension() : nullptr;
    const auto bIsRemote = pExtension && pExtension->IsRemote();

    if (bIsRemote && !ScopedReferencesOverride::IsOverriden())
        return 1;

    // Don't interfere with non actor references, or the player, or if we are calling our self
    if (apThis->formType != Actor::Type || apThis == PlayerCharacter::Get() || ScopedReferencesOverride::IsOverriden())
        return TiltedPhoques::ThisCall(RealSetPosition, apThis, aPosition);

    ScopedReferencesOverride recursionGuard;

    // It just works TM
    apThis->SetPosition(aPosition, false);

    return 1;
}

TP_THIS_FUNCTION(TForceState, void, Actor, const NiPoint3&, float, float, TESObjectCELL*, TESWorldSpace*, bool);
static TForceState* RealForceState = nullptr;

void TP_MAKE_THISCALL(HookForceState, Actor, const NiPoint3& acPosition, float aX, float aZ, TESObjectCELL* apCell, TESWorldSpace* apWorldSpace, bool aUnkBool)
{
    /*const auto pNpc = Cast<TESNPC>(apThis->baseForm);
    if (pNpc)
    {
        spdlog::info("For TESNPC: {}, spawn at {} {} {}", pNpc->fullName.value, apPosition->m_x, apPosition->m_y,
                     apPosition->m_z);
    }*/

    // if (apThis != PlayerCharacter::Get())
    //     return;

    return TiltedPhoques::ThisCall(RealForceState, apThis, acPosition, aX, aZ, apCell, apWorldSpace, aUnkBool);
}

TP_THIS_FUNCTION(TSpawnActorInWorld, bool, Actor);
static TSpawnActorInWorld* RealSpawnActorInWorld = nullptr;

// TODO: this isn't SpawnActorInWorld, this is TESObjectREFR::UpdateReference3D()
bool TP_MAKE_THISCALL(HookSpawnActorInWorld, Actor)
{
    if (!apThis || apThis->IsDeleted())
        return false;

    // Pre-3D authoritative `TESLevCharacter` reconciliation:
    // Reconcile placed `0x00..0xFE` actors BEFORE `RealSpawnActorInWorld` builds their `NiNode` and
    // `BShkbAnimationGraph` so every client constructs the exact same species/variant on pass 1
    // with zero `NiNode` teardown and zero Havok/TBB worker-thread race.
    if (World::IsCreated() &&
        World::IsConnectedCached() &&
        World::IsMainThread() &&
        apThis->formID != 0x14 &&
        (apThis->formID & 0xFF000000u) != 0xFF000000u &&
        !apThis->GetNiNode() &&
        !apThis->IsDead())
    {
        const uint32_t authBaseId =
            World::Get().GetCharacterService().LookupAuthoritativePlacedBaseFormId(apThis->formID);
        if (authBaseId != 0 && (!apThis->baseForm || apThis->baseForm->formID != authBaseId))
        {
            if (TESNPC* pAuthNpc = Cast<TESNPC>(TESForm::GetById(authBaseId));
                pAuthNpc && !pAuthNpc->IsTemporary())
            {
                apThis->SetBaseForm(pAuthNpc);
            }
        }
    }

    const auto* pNpc = Cast<TESNPC>(apThis->baseForm);
    if (pNpc)
    {
        spdlog::info("Spawn Actor: {:X}, and NPC {}", apThis->formID, pNpc->fullName.value);
    }

    return TiltedPhoques::ThisCall(RealSpawnActorInWorld, apThis);
}

TP_THIS_FUNCTION(TDamageActor, bool, Actor, float aDamage, Actor* apHitter, bool aKillMove);
static TDamageActor* RealDamageActor = nullptr;
static thread_local bool s_inAuthoritativeDamageHook = false;
static thread_local bool s_inNativeBleedoutNudge = false;

struct ScopedDamageHookGuard
{
    ScopedDamageHookGuard() noexcept
        : m_prev(s_inAuthoritativeDamageHook)
    {
        s_inAuthoritativeDamageHook = true;
    }
    ~ScopedDamageHookGuard() noexcept { s_inAuthoritativeDamageHook = m_prev; }
    bool m_prev{false};
};

struct ScopedNativeBleedoutNudgeGuard
{
    ScopedNativeBleedoutNudgeGuard() noexcept
        : m_prev(s_inNativeBleedoutNudge)
    {
        s_inNativeBleedoutNudge = true;
    }
    ~ScopedNativeBleedoutNudgeGuard() noexcept { s_inNativeBleedoutNudge = m_prev; }
    bool m_prev{false};
};

// TODO: this is flawed, since it does not account for invulnerable actors
bool TP_MAKE_THISCALL(HookDamageActor, Actor, float aDamage, Actor* apHitter, bool aKillMove)
{
    if (!apThis)
        return false;

    if (!World::IsCreated() || !World::IsConnectedCached() || s_inNativeBleedoutNudge)
        return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);

    const auto coopPolicy =
        (apHitter && apThis) ? World::Get().GetRfabAdapterService().EvaluateCoopAction(apHitter, apThis, false)
                             : RfabCompat::CoopAllyActionPolicy{};

    // RFAB «Единство в битве (Кооп)»: block 100% of physical melee/ranged/bash/explosion damage and HitEvent aggro
    // from the attacker (and their summons) onto co-op party allies and party allies' summons!
    if (coopPolicy.BlockHarmfulAction)
        return false;

    // Respect synchronized boss/actor invulnerability phases (SetGhost / ExtraGhost / IS_INVULNERABLE):
    // neither owner nor non-owner peers may deal damage through an active boss shield/ethereal phase.
    if (apThis && !apThis->GetExtension()->IsPlayer() && apThis->IsGhost())
        return false;

    if (apHitter)
        World::Get().GetRunner().Trigger(HitEvent(apHitter->formID, apThis->formID));

    const auto* pTargetExtForScale = apThis->GetExtension();
    const bool scaleTargetIncomingDamage =
        pTargetExtForScale &&
        CoopDamagePipeline::ShouldScaleIncomingEnemyDamage(
            pTargetExtForScale->IsPlayer(),
            apThis->IsPlayerTeammate(),
            apThis->IsPlayerSummon(),
            apThis->IsCommandedActor(),
            apThis->IsMount());

    const float currentHealth = apThis->GetActorValue(ActorValueInfo::kHealth);
    const float rawEstimatedDamage = GameplayFormulas::CalculateRealDamage(apThis, aDamage, aKillMove);
    const float estimatedRealDamage = -CoopDamagePipeline::ScaleIncomingEnemyDamageDelta(
        -rawEstimatedDamage,
        World::GetEnemyHealthMultiplierCached(),
        scaleTargetIncomingDamage);
    const bool wouldKill = (currentHealth - estimatedRealDamage) <= 0.f;

    auto applyAuthoritativeDamage = [&]() -> bool
    {
        ScopedDamageHookGuard dmgGuard;
        // Enforce single-owner RFAB kill XP authority: if a non-owner player or local summon lands a killing blow
        // on a remotely-owned NPC, mark kInfamy = -1 locally so RFAB_XP_Alias::OnActorKilled fires ONLY on the
        // NPC's authoritative owner client (via OnHealthChangeBroadcast) and distributes exact 1x XP to the party!
        const bool isRemoteNonPlayer = apThis->GetExtension()->IsRemote() && !apThis->GetExtension()->IsPlayer();
        const float prevInfamy = isRemoteNonPlayer ? apThis->GetActorValue(ActorValueInfo::kInfamy) : 0.0f;
        if (isRemoteNonPlayer)
        {
            ScopedActorValueOverride avOverride;
            apThis->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, -1.0f);
        }

        const bool captureRemoteImpact = isRemoteNonPlayer && !apThis->IsDead() && apHitter != nullptr;
        ScopedRemoteHitReactionCapture hitCapture(captureRemoteImpact ? apThis : nullptr);
        if (captureRemoteImpact &&
            (apThis->actorState.flags2 & (1u << 13)) == 0 &&
            apThis->animationGraphHolder.IsReady())
        {
            BSFixedString staggerMagVar("staggerMagnitude");
            apThis->animationGraphHolder.SetVariableFloat(&staggerMagVar, 0.0f);
        }

        const float healthBefore = apThis->GetActorValue(ActorValueInfo::kHealth);
        const bool result = TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
        const float healthAfter = apThis->GetActorValue(ActorValueInfo::kHealth);
        float actualDelta = healthAfter - healthBefore;
        if (isRemoteNonPlayer && (apThis->IsDead() || healthAfter <= 0.0f) && actualDelta < 0.0f)
        {
            actualDelta = CoopWorldStabilizer::EnsureLethalOverkillDelta(
                actualDelta,
                -estimatedRealDamage,
                healthBefore,
                true);
        }

        float capturedStaggerMag = 0.0f;
        float capturedStaggerDir = 0.0f;
        if (captureRemoteImpact && !apThis->IsDead() && healthAfter > 0.0f)
        {
            float graphStaggerMag = 0.0f;
            if (apThis->animationGraphHolder.IsReady())
            {
                BSFixedString staggerMagVar("staggerMagnitude");
                (void)apThis->animationGraphHolder.GetVariableFloat(&staggerMagVar, &graphStaggerMag);
            }
            const float rawStagger = std::max(s_capturedHitStaggerMag, graphStaggerMag);
            if (std::isfinite(rawStagger) && rawStagger > 0.05f)
            {
                capturedStaggerMag = std::clamp(rawStagger, 0.15f, 1.0f);
                capturedStaggerDir = CoopDamagePipeline::ComputeNormalizedStaggerDirection(
                    apHitter->position.x,
                    apHitter->position.y,
                    apThis->position.x,
                    apThis->position.y,
                    apThis->rotation.z);
            }
        }

        // If the remote NPC survived this hit, immediately restore kInfamy >= 0 so if ownership later transfers
        // to this client (TakeOwnership), RFAB_XP_Alias::OnActorKilled will still award full kill XP!
        if (isRemoteNonPlayer && !apThis->IsDead() && healthAfter > 0.0f)
        {
            ScopedActorValueOverride avOverride;
            apThis->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, std::max(0.0f, prevInfamy));
        }

        // Broadcast the exact delta + native Requiem/RFAB poise/bash stagger produced by SkyrimSE.exe + RFAB.dll
        // so invulnerable/ethereal boss phases, armor/silver modifiers, interrupts, and ScriptDamage stay 100% synced.
        if (actualDelta != 0.f || capturedStaggerMag > 0.05f)
        {
            World::Get().GetRunner().Trigger(HealthChangeEvent(
                apThis->formID,
                actualDelta,
                apHitter ? apHitter->formID : 0,
                capturedStaggerMag,
                capturedStaggerDir));
        }

        return result;
    };

    const auto* pExHittee = apThis->GetExtension();
    if (pExHittee->IsLocalPlayer())
    {
        if (!World::IsPvpEnabledCached() && !coopPolicy.AllowPartySparring)
        {
            if (apHitter)
            {
                const auto* pHitterExt = apHitter->GetExtension();
                const Actor* pHitterCmd = apHitter->GetCommandingActor();
                const auto* pHitterCmdExt = pHitterCmd ? pHitterCmd->GetExtension() : nullptr;
                if ((pHitterExt && pHitterExt->IsRemotePlayer()) ||
                    (pHitterCmdExt && pHitterCmdExt->IsRemotePlayer()) ||
                    (pHitterExt && pHitterExt->IsRemote() && apHitter->IsPlayerSummon()))
                {
                    return false;
                }
            }
        }

        auto& downedSvc = World::Get().GetDownedStateService();
        if (downedSvc.IsSystemActive())
        {
            if (downedSvc.IsLocalPlayerDowned() || downedSvc.IsLocalPlayerInvulnerable())
                return false;

            // 100% Native Skyrim SE Bleedout (`Essential + NoBleedoutRecovery` at `0.0f HP`):
            // Keep Essential + NoBleedoutRecovery armed before RealDamageActor (with killmoves suppressed),
            // let HookRestoreActorValue clamp overkill cleanly at exact 0.0f (never 1.0f, never -1500.0f),
            // so SkyrimSE.exe natively transitions 0x14 into ACTOR_LIFE_STATE::kBleedout without fighting MCO/Havok.
            if (!apThis->IsEssential())
                apThis->SetPlayerRespawnMode(true);

            ScopedDamageHookGuard dmgGuard;
            const float healthBefore = apThis->GetActorValue(ActorValueInfo::kHealth);

            const bool result = TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, false);

            const float realHealthAfter = apThis->GetActorValue(ActorValueInfo::kHealth);
            const float actualDelta = realHealthAfter - healthBefore;
            if (actualDelta != 0.f)
                World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, actualDelta));

            if (apThis->actorState.IsBleedingOut() || realHealthAfter <= 0.0f)
            {
                downedSvc.LatchNativeBleedoutEntered(apThis);
                return false;
            }

            return result;
        }

        return applyAuthoritativeDamage();
    }
    else if (pExHittee->IsRemotePlayer())
    {
        return wouldKill;
    }

    if (apHitter)
    {
        const auto* pExHitter = apHitter->GetExtension();
        const Actor* pHitterCmd = apHitter->GetCommandingActor();
        const auto* pHitterCmdExt = pHitterCmd ? pHitterCmd->GetExtension() : nullptr;
        const bool isLocalPlayerSummon =
            (pHitterCmd && pHitterCmd->formID == 0x14) ||
            (pExHitter && !pExHitter->IsRemote() && apHitter->IsPlayerSummon());
        const bool isRemotePlayerSummon =
            (pHitterCmdExt && pHitterCmdExt->IsRemotePlayer()) ||
            (pExHitter && pExHitter->IsRemote() && apHitter->IsPlayerSummon());

        if ((pExHitter && pExHitter->IsLocalPlayer()) || isLocalPlayerSummon)
        {
            if (pExHittee->IsRemote() && !pExHittee->IsPlayer())
                Actor::ArmLocalPlayerScriptDamageWindow(apThis->formID, 3000ULL);
            return applyAuthoritativeDamage();
        }
        if ((pExHitter && pExHitter->IsRemotePlayer()) || isRemotePlayerSummon)
            return wouldKill;
    }

    // Covers apHitter == nullptr (RFAB_PapyrusFunctions.ScriptDamage, runes, DoTs, traps) as well as
    // summons, followers, and NPC-vs-NPC combat strictly on the single client owning apThis.
    if (pExHittee->IsLocal())
        return applyAuthoritativeDamage();

    return wouldKill;
}

TP_THIS_FUNCTION(TApplyActorEffect, void, ActiveEffect, Actor* apTarget, float aEffectValue, unsigned int unk1);
static TApplyActorEffect* RealApplyActorEffect = nullptr;

void TP_MAKE_THISCALL(HookApplyActorEffect, ActiveEffect, Actor* apTarget, float aEffectValue, unsigned int unk1)
{
    if (!World::IsCreated() || !World::IsConnectedCached())
        return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, unk1);

    bool isLocalCasterHostileConcentration = false;
    uint32_t localConcCasterFormId = 0;
    if (apThis && apThis->pSpell && apTarget)
    {
        // Unified caster-authoritative hostile magic for `0x14` & local player summons across ALL `MagicItem`
        // types (`SpellItem`, `EnchantmentItem` staves/weapons, `ScrollItem`, both `CONCENTRATION` and
        // `FIRE_AND_FORGET`): evaluates damage on the caster's real `0x14` with 100% of RFAB Magic Penetration,
        // Destruction perks, CustomSkills, Dual-Cast, and potions instead of a perkless `0xFF` puppet on Host!
        if (!apThis->pSpell->IsHealingSpell() &&
            !apThis->pSpell->IsBuffSpell() &&
            apThis->hCaster != 0)
        {
            if (GamePtr<Actor> spCaster = Actor::LookupByHandle(apThis->hCaster))
            {
                const auto* pCasterExt = spCaster->GetExtension();
                const Actor* pCasterCmd = spCaster->GetCommandingActor();
                const bool isLocalCaster =
                    (pCasterExt && pCasterExt->IsLocalPlayer()) ||
                    (pCasterCmd && pCasterCmd->formID == 0x14) ||
                    (pCasterExt && !pCasterExt->IsRemote() && spCaster->IsPlayerSummon());
                if (isLocalCaster)
                {
                    isLocalCasterHostileConcentration = true;
                    localConcCasterFormId = spCaster->formID;
                }
            }
        }
    }

    ScopedApplyActorEffectGuard effectScope(isLocalCasterHostileConcentration, localConcCasterFormId);

    const auto* pValueModEffect = Cast<ValueModifierEffect>(apThis);
    if (pValueModEffect && pValueModEffect->actorValueIndex == ActorValueInfo::kHealth && aEffectValue > 0.0f)
    {
        if (apTarget && apTarget->GetExtension())
        {
            const auto pExTarget = apTarget->GetExtension();
            if (pExTarget->IsRemote())
            {
                // Passive regeneration and ActiveEffect heals on remote actors are authoritative on their owner.
                return;
            }
            if (pExTarget->IsLocalPlayer() &&
                World::Get().GetDownedStateService().ShouldBlockPlayerHealthMutation(aEffectValue))
            {
                return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, unk1);
}

TP_THIS_FUNCTION(TRestoreActorValue, void, Actor, ActorValueOwner::ForceMode aMode, uint32_t aId, float aValue);
static TRestoreActorValue* RealRestoreActorValue = nullptr;

void Actor::TriggerNativeBleedout() noexcept
{
    SetPlayerRespawnMode(true);
    if (actorState.IsBleedingOut() || !RealDamageActor)
        return;

    ScopedDamageHookGuard dmgGuard;
    ScopedActorValueOverride avOverride;
    const float curHp = GetActorValue(ActorValueInfo::kHealth);
    if (curHp > 0.0f && RealRestoreActorValue)
        TiltedPhoques::ThisCall(RealRestoreActorValue, this, ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, -curHp);

    ScopedNativeBleedoutNudgeGuard nudgeGuard;
    (void)TiltedPhoques::ThisCall(RealDamageActor, this, 1.0f, nullptr, false);
}

// Unified hook on Actor::RestoreActorValue (SE ID 37513 @ 0x620900, exact ABI: RCX = Actor*, EDX = ForceMode, R8D = AV, XMM3 = float):
// 1. Clamps overkill subtraction on local player cleanly at exact 0.0f under Essential + NoBleedoutRecovery
//    so SkyrimSE.exe natively enters kBleedout without ever accumulating -500 HP overkill debt or clamping at 1.0f.
// 2. Blocks 100% of unauthorized heals/regen/potions/DoTs on local player while in Bleedout or post-revive grace.
// 3. Blocks passive local health regeneration on remote actors without interfering with authoritative damage/sync.
void TP_MAKE_THISCALL(HookRestoreActorValue, Actor, ActorValueOwner::ForceMode aMode, uint32_t aId, float aValue)
{
    if (!World::IsCreated() || !World::IsConnectedCached())
    {
        TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, aValue);
        return;
    }

    if (aMode == ActorValueOwner::ForceMode::DAMAGE && aId == ActorValueInfo::kHealth && apThis)
    {
        const float rawDeltaValue = aValue;
        if (!ScopedActorValueOverride::IsOverriden() && aValue != 0.0f)
        {
            if (const auto* pScaleExt = apThis->GetExtension())
            {
                const bool shouldScaleEnemyTarget = CoopDamagePipeline::ShouldScaleIncomingEnemyDamage(
                    pScaleExt->IsPlayer(),
                    apThis->IsPlayerTeammate(),
                    apThis->IsPlayerSummon(),
                    apThis->IsCommandedActor(),
                    apThis->IsMount());
                const float enemyMult = World::GetEnemyHealthMultiplierCached();
                if (aValue < 0.0f)
                {
                    aValue = CoopDamagePipeline::ScaleIncomingEnemyDamageDelta(
                        aValue,
                        enemyMult,
                        shouldScaleEnemyTarget);
                }
                else
                {
                    const float nativeMaxHp = std::max(
                        apThis->GetActorPermanentValue(ActorValueInfo::kHealth),
                        apThis->GetActorValue(ActorValueInfo::kHealth));
                    aValue = CoopDamagePipeline::ScaleIncomingEnemyHealDelta(
                        aValue,
                        nativeMaxHp,
                        enemyMult,
                        shouldScaleEnemyTarget,
                        s_inApplyActorEffect);
                }
            }
        }

        if (auto* pExt = apThis->GetExtension())
        {
            if (pExt->IsRemote())
            {
                if (!ScopedActorValueOverride::IsOverriden() && !s_inAuthoritativeDamageHook)
                {
                    // Caster-authoritative remote NPC damage paths:
                    // 1) RFAB `kScript` / `RFAB_PapyrusFunctions.ScriptDamage` / Perk `OnHit` (`!s_inApplyActorEffect`)
                    //    when the Local Player (or Summon) armed an active hit/kScript authorization window.
                    // 2) Hostile `CONCENTRATION` spells (`Flames`, `Sparks`, `Frostbite`, `Lightning Ray`) actively
                    //    channeled by `0x14` (or a local player summon), whose puppet stream on Host is visual-only.
                    const bool allowScriptDamage =
                        !s_inApplyActorEffect &&
                        rawDeltaValue < -0.05f &&
                        Actor::HasLocalPlayerScriptDamageWindow(apThis->formID);
                    const bool allowConcentrationBeam =
                        s_inLocalCasterConcentrationEffect &&
                        rawDeltaValue < -0.0005f;

                    if (!pExt->IsPlayer() &&
                        !apThis->IsGhost() &&
                        (allowScriptDamage || allowConcentrationBeam))
                    {
                        const float prevInfamy = apThis->GetActorValue(ActorValueInfo::kInfamy);
                        {
                            ScopedActorValueOverride avOverride;
                            apThis->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, -1.0f);
                        }

                        const float hpBefore = apThis->GetActorValue(ActorValueInfo::kHealth);
                        TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, aValue);
                        const float hpAfter = apThis->GetActorValue(ActorValueInfo::kHealth);
                        float actualDelta = hpAfter - hpBefore;
                        if ((apThis->IsDead() || hpAfter <= 0.0f) && actualDelta < 0.0f)
                        {
                            actualDelta = CoopWorldStabilizer::EnsureLethalOverkillDelta(
                                actualDelta,
                                aValue,
                                hpBefore,
                                true);
                        }

                        if (!apThis->IsDead() && hpAfter > 0.0f)
                        {
                            ScopedActorValueOverride avOverride;
                            apThis->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, std::max(0.0f, prevInfamy));
                        }

                        const float minEmitMag = allowConcentrationBeam ? -0.0001f : -0.005f;
                        if (actualDelta < minEmitMag)
                        {
                            const uint32_t attackerFormId = allowConcentrationBeam ? s_localConcentrationCasterFormId : 0x14u;
                            World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, actualDelta, attackerFormId));
                        }
                    }
                    return;
                }

                TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, aValue);
                return;
            }
        }

        PlayerCharacter* pLocalPlayer = PlayerCharacter::Get();
        if (pLocalPlayer && apThis == pLocalPlayer)
        {
            if (s_inNativeBleedoutNudge)
            {
                const float curHp = pLocalPlayer->GetActorValue(ActorValueInfo::kHealth);
                if (aValue < 0.0f && curHp <= 0.0f)
                    return;
                TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, (curHp > 0.0f && aValue < -curHp) ? -curHp : aValue);
                return;
            }

            auto& downed = World::Get().GetDownedStateService();
            if (downed.ShouldBlockPlayerHealthMutation(aValue))
                return;

            if (downed.IsSystemActive() && aValue < 0.0f)
            {
                if (!pLocalPlayer->IsEssential())
                    pLocalPlayer->SetPlayerRespawnMode(true);

                const float curHp = pLocalPlayer->GetActorValue(ActorValueInfo::kHealth);
                if ((curHp + aValue) <= 0.0f)
                {
                    const float exactZeroDelta = (curHp > 0.0f) ? -curHp : 0.0f;
                    if (exactZeroDelta < 0.0f)
                        TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, exactZeroDelta);

                    if (!s_inAuthoritativeDamageHook)
                    {
                        if (!ScopedActorValueOverride::IsOverriden() && exactZeroDelta != 0.0f)
                            World::Get().GetRunner().Trigger(HealthChangeEvent(pLocalPlayer->formID, exactZeroDelta));
                        downed.LatchNativeBleedoutEntered(pLocalPlayer);
                    }
                    return;
                }
            }

            const float hpBefore = pLocalPlayer->GetActorValue(ActorValueInfo::kHealth);
            TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, aValue);
            const float hpAfter = pLocalPlayer->GetActorValue(ActorValueInfo::kHealth);
            const float actualDelta = hpAfter - hpBefore;

            if (!s_inAuthoritativeDamageHook && !ScopedActorValueOverride::IsOverriden() && std::abs(actualDelta) > 0.0005f)
                World::Get().GetRunner().Trigger(HealthChangeEvent(pLocalPlayer->formID, actualDelta));
            return;
        }

        // Single authoritative owner HP delta measurement for locally-owned non-player NPCs:
        // Covers BOTH `ValueModifierEffect` (`s_inApplyActorEffect`: Destruction spells, poisons, weapon enchants,
        // `1/M`-scaled troll/spriggan/dragon-priest regen, enemy healing spells/potions) AND direct `RestoreActorValue`
        // (`RFAB.dll` ScriptDamage, bleeds, and scripted boss phase heals), coalescing small ticks cleanly via 250ms batching.
        if (auto* pExt = apThis->GetExtension();
            pExt &&
            pExt->IsLocal() &&
            !s_inAuthoritativeDamageHook &&
            !ScopedActorValueOverride::IsOverriden() &&
            aValue != 0.0f)
        {
            const float hpBefore = apThis->GetActorValue(ActorValueInfo::kHealth);
            TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, aValue);
            const float hpAfter = apThis->GetActorValue(ActorValueInfo::kHealth);
            const float actualDelta = hpAfter - hpBefore;
            if (std::abs(actualDelta) > 0.0005f)
                World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, actualDelta));
            return;
        }
    }

    TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, aValue);
}

void TP_MAKE_THISCALL(HookAddInventoryItem, Actor, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner)
{
    const auto* pExt = apThis ? apThis->GetExtension() : nullptr;
    const bool isInteractivePlayerTransfer =
        (apOldOwner && apOldOwner == PlayerCharacter::Get()) ||
        World::IsContainerMenuOpenCached();
    const bool blockRemote =
        pExt &&
        (pExt->IsRemotePlayer() ||
         (pExt->IsRemote() && pExt->IsPlayerSummonFlag()) ||
         (pExt->IsRemote() && !isInteractivePlayerTransfer));

    if (World::IsCreated() &&
        World::IsConnectedCached() &&
        !ScopedInventoryOverride::IsOverriden() &&
        apThis &&
        apItem &&
        !blockRemote)
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apItem->formID, item.BaseId);
        item.Count = aCount;

        if (apExtraData)
            apThis->GetItemFromExtraData(item, apExtraData);

        if (!item.IsQuestItem && item.Count > 0 && apThis == PlayerCharacter::Get())
        {
            const char* pEditorId = apItem->GetFormEditorID();
            const std::string_view editorIdView = pEditorId ? std::string_view(pEditorId) : std::string_view{};
            const bool syncHomes = World::IsSyncPlayerHomesCached();
            if (RfabCompat::IsSharedCoopKeyOrClaw(editorIdView, static_cast<uint8_t>(apItem->formType), apItem->formID, syncHomes))
                item.IsQuestItem = true;
        }

        World::Get().GetRunner().Trigger(InventoryChangeEvent(apThis->formID, std::move(item)));
    }

    TiltedPhoques::ThisCall(RealAddInventoryItem, apThis, apItem, apExtraData, aCount, apOldOwner);
}

void* TP_MAKE_THISCALL(HookPickUpObject, Actor, TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2)
{
    if (World::IsCreated() &&
        World::IsConnectedCached() &&
        !ScopedInventoryOverride::IsOverriden() &&
        apThis &&
        apObject &&
        apObject->baseForm &&
        apThis->GetExtension() &&
        !apThis->GetExtension()->IsRemote())
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apObject->baseForm->formID, item.BaseId);
        item.Count = aCount;

        if (apObject->GetExtraDataList())
            apThis->GetItemFromExtraData(item, apObject->GetExtraDataList());

        if (!item.IsQuestItem && item.Count > 0 && apThis == PlayerCharacter::Get())
        {
            const char* pEditorId = apObject->baseForm->GetFormEditorID();
            const std::string_view editorIdView = pEditorId ? std::string_view(pEditorId) : std::string_view{};
            const bool syncHomes = World::IsSyncPlayerHomesCached();
            if (RfabCompat::IsSharedCoopKeyOrClaw(editorIdView, static_cast<uint8_t>(apObject->baseForm->formType), apObject->baseForm->formID, syncHomes))
                item.IsQuestItem = true;
        }

        const bool isTempDropPickup = apObject->IsTemporary() && !ScopedActivateOverride::IsOverriden();

        World::Get().GetRunner().Trigger(InventoryChangeEvent(apThis->formID, std::move(item), isTempDropPickup, true));
    }

    return TiltedPhoques::ThisCall(RealPickUpObject, apThis, apObject, aCount, aUnk1, aUnk2);
}

void Actor::PickUpObject(TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2) noexcept
{
    TiltedPhoques::ThisCall(RealPickUpObject, this, apObject, aCount, aUnk1, aUnk2);
}

void* TP_MAKE_THISCALL(HookDropObject, Actor, void* apResult, TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation)
{
    if (World::IsConnectedCached() &&
        apThis &&
        apObject &&
        apThis->GetExtension() &&
        !apThis->GetExtension()->IsRemote())
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apObject->formID, item.BaseId);
        item.Count = -aCount;

        if (apExtraData)
            apThis->GetItemFromExtraData(item, apExtraData);

        World::Get().GetRunner().Trigger(InventoryChangeEvent(apThis->formID, std::move(item), true));
    }

    ScopedInventoryOverride _;

    return TiltedPhoques::ThisCall(RealDropObject, apThis, apResult, apObject, apExtraData, aCount, apLocation, apRotation);
}

void Actor::DropOrPickUpObject(const Inventory::Entry& arEntry, NiPoint3* apLocation, NiPoint3* apRotation) noexcept
{
    ExtraDataList* pExtraData = GetExtraDataFromItem(arEntry);

    ModSystem& modSystem = World::Get().GetModSystem();

    uint32_t objectId = modSystem.GetGameId(arEntry.BaseId);
    TESBoundObject* pObject = Cast<TESBoundObject>(TESForm::GetById(objectId));
    if (!pObject)
    {
        spdlog::warn("Object to drop/pick up not found, {:X}:{:X}.", arEntry.BaseId.ModId, arEntry.BaseId.BaseId);
        ExtraDataList::Delete(pExtraData);
        return;
    }

    if (arEntry.Count < 0)
    {
        DropObject(pObject, pExtraData, -arEntry.Count, apLocation, apRotation);
        return;
    }

    ExtraDataList::Delete(pExtraData);

    if (arEntry.Count > 0)
    {
        // Despawn the matching temporary 0xFFxxxxxx dropped world reference near this actor so peers
        // cannot pick up a second copy after someone already picked it up!
        TESObjectREFR* pBestMatch = nullptr;
        float bestDistSq = 2048.0f * 2048.0f;

        auto scanCell = [&](TESObjectCELL* apCell)
        {
            if (!apCell)
                return;

            BSScopedLock<BSRecursiveLock> cellLock(apCell->lock);
            if (!apCell->refData.refArray)
                return;

            for (uint32_t i = 0; i < apCell->refData.capacity; ++i)
            {
                TESObjectREFR* pRef = apCell->refData.refArray[i].Get();
                if (!pRef || !pRef->baseForm || pRef == this || pRef->IsDisabled() || pRef->baseForm != pObject)
                    continue;
                if ((pRef->formID & 0xFF000000) != 0xFF000000)
                    continue;

                const float dx = pRef->position.x - position.x;
                const float dy = pRef->position.y - position.y;
                const float dz = pRef->position.z - position.z;
                const float distSq = dx * dx + dy * dy + dz * dz;
                if (distSq < bestDistSq)
                {
                    bestDistSq = distSq;
                    pBestMatch = pRef;
                }
            }
        };

        scanCell(parentCell);
        if (PlayerCharacter* pLocalPlayer = PlayerCharacter::Get(); pLocalPlayer && pLocalPlayer->parentCell != parentCell)
            scanCell(pLocalPlayer->parentCell);

        if (pBestMatch)
        {
            pBestMatch->UnsetChanged(0xFFFFFFFF);
            pBestMatch->SetSkipSaveFlag(true);
            if (World::IsMainThread())
            {
                pBestMatch->Disable();
                pBestMatch->Delete();
            }
            else
            {
                const uint32_t matchFormId = pBestMatch->formID;
                World::Get().GetRunner().Queue(
                    [matchFormId]()
                    {
                        if (TESObjectREFR* pDeferred = Cast<TESObjectREFR>(TESForm::GetById(matchFormId)))
                        {
                            pDeferred->Disable();
                            pDeferred->Delete();
                        }
                    });
            }
        }

        AddOrRemoveItem(arEntry);
    }
}

void Actor::DropObject(TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation) noexcept
{
    spdlog::debug("Dropping object, form id: {:X}, count: {}, actor: {:X}", apObject->formID, aCount, formID);
    BSPointerHandle<TESObjectREFR> result{};
    TiltedPhoques::ThisCall(RealDropObject, this, &result, apObject, apExtraData, aCount, apLocation, apRotation);
    if (GamePtr<TESObjectREFR> spDropped = TESObjectREFR::LookupByHandle(result))
    {
        spDropped->SetSkipSaveFlag(true);
        spDropped->UnsetChanged(0xFFFFFFFF);
    }
}

TP_THIS_FUNCTION(TUpdateDetectionState, void, ActorKnowledge, void*);
static TUpdateDetectionState* RealUpdateDetectionState = nullptr;

void TP_MAKE_THISCALL(HookUpdateDetectionState, ActorKnowledge, void* apState)
{
    GamePtr<TESObjectREFR> spOwner = TESObjectREFR::LookupByHandle(apThis->hOwner);
    GamePtr<TESObjectREFR> spTarget = TESObjectREFR::LookupByHandle(apThis->hTarget);
    TESObjectREFR* const pOwner = spOwner.Get();
    TESObjectREFR* const pTarget = spTarget.Get();

    if (pOwner && pTarget)
    {
        auto pOwnerActor = Cast<Actor>(pOwner);
        auto pTargetActor = Cast<Actor>(pTarget);
        if (pOwnerActor && pTargetActor)
        {
            const auto* pOwnerExt = pOwnerActor->GetExtension();
            const auto* pTargetExt = pTargetActor->GetExtension();

            if (pOwnerExt && pOwnerExt->IsRemotePlayer() && pTargetExt && pTargetExt->IsLocalPlayer())
            {
                spdlog::debug("Cancelling detection from remote player to local player, owner: {:X}, target: {:X}", pOwner->formID, pTarget->formID);
                return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealUpdateDetectionState, apThis, apState);
}

struct DialogueItem;

// TODO: This is an AIProcess function
TP_THIS_FUNCTION(TProcessResponse, uint64_t, void, DialogueItem* apVoice, Actor* apTalkingActor, Actor* apTalkedToActor);
static TProcessResponse* RealProcessResponse = nullptr;

uint64_t TP_MAKE_THISCALL(HookProcessResponse, void, DialogueItem* apVoice, Actor* apTalkingActor, Actor* apTalkedToActor)
{
    if (apTalkingActor && apTalkingActor->GetExtension() && apTalkingActor->GetExtension()->IsRemotePlayer())
        return 0;

    // Never allow guards or NPCs to initiate ForceGreet / arrest / greeting dialogue targeted at a remote player clone!
    if (apTalkedToActor && apTalkedToActor->GetExtension() && apTalkedToActor->GetExtension()->IsRemotePlayer())
        return 0;

    return TiltedPhoques::ThisCall(RealProcessResponse, apThis, apVoice, apTalkingActor, apTalkedToActor);
}

bool TP_MAKE_THISCALL(HookInitiateMountPackage, Actor, Actor* apMount)
{
    if (apThis && apMount && apThis->GetExtension() && apThis->GetExtension()->IsLocal())
        World::Get().GetRunner().Trigger(MountEvent(apThis->formID, apMount->formID));

    return TiltedPhoques::ThisCall(RealInitiateMountPackage, apThis, apMount);
}

TP_THIS_FUNCTION(TUnequipObject, void, Actor, void* apUnk1, TESBoundObject* apObject, int32_t aUnk2, void* apUnk3);
static TUnequipObject* RealUnequipObject = nullptr;

void TP_MAKE_THISCALL(HookUnequipObject, Actor, void* apUnk1, TESBoundObject* apObject, int32_t aUnk2, void* apUnk3)
{
    if (!apThis || !apObject || apThis->IsDeleted() || !apThis->currentProcess || !apThis->currentProcess->middleProcess)
        return;

    TiltedPhoques::ThisCall(RealUnequipObject, apThis, apUnk1, apObject, aUnk2, apUnk3);
}

TP_THIS_FUNCTION(TSpeakSoundFunction, float, Actor, const char* apName, uint32_t* a3, uint32_t a4, uint32_t a5, uint32_t a6, uint64_t a7, uint64_t a8, uint64_t a9, bool a10, uint64_t a11, bool a12, bool a13, bool a14);
static TSpeakSoundFunction* RealSpeakSoundFunction = nullptr;

float TP_MAKE_THISCALL(HookSpeakSoundFunction, Actor, const char* apName, uint32_t* a3, uint32_t a4, uint32_t a5, uint32_t a6, uint64_t a7, uint64_t a8, uint64_t a9, bool a10, uint64_t a11, bool a12, bool a13, bool a14)
{
    // Guard silent/lip-only guard/NPC topics where apName is null or empty (prevents String(nullptr) strlen crash!).
    if (World::IsCreated() && World::IsConnectedCached() && apThis && apThis->formID != 0 && apName && apName[0] != '\0')
    {
        if (const auto* pExt = apThis->GetExtension(); !pExt || !pExt->IsRemotePlayer())
        {
            String safeVoicePath(apName);
            World::Get().GetRunner().Trigger(DialogueEvent(apThis->formID, std::move(safeVoicePath)));
        }
    }

    return TiltedPhoques::ThisCall(RealSpeakSoundFunction, apThis, apName, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

float Actor::SpeakSound(const char* pFile)
{
    if (!pFile || pFile[0] == '\0' || !RealSpeakSoundFunction)
        return 0.0f;

    uint32_t handle[3]{};
    handle[0] = -1;

    return TiltedPhoques::ThisCall(RealSpeakSoundFunction, this, pFile, handle, 0, 0x32, 0, 0, 0, 0, 0, 0, 0, 1, 1);
}

bool Actor::IsTalking() noexcept
{
    TP_THIS_FUNCTION(TIsTalking, bool, Actor);
    POINTER_SKYRIMSE(TIsTalking, s_IsTalking, 36277);
    return TiltedPhoques::ThisCall(s_IsTalking, this);
}

bool Actor::IsInScene() noexcept
{
    // We don't have a solution for Condition Functions
    //PAPYRUS_FUNCTION(bool, Actor, IsInScene);
    //bool papyrusValue = s_pIsInScene(this);
    bool flagsValue= (flags1 & ActorBoolBits::kHasSceneExtra) != 0;

    return flagsValue;
}

bool Actor::IsInDialogueWithPlayer() noexcept
{
    using ObjectReference = TESObjectREFR;
    PAPYRUS_FUNCTION(bool, ObjectReference, IsInDialogueWithPlayer);
    return s_pIsInDialogueWithPlayer(this);
}

float Actor::GetVoiceRecoveryTime() noexcept
{
    // Doesn't work. And Wiki only describes w.r.t shouts
    // PAPYRUS_FUNCTION(float, Actor, GetVoiceRecoveryTime);
    // float fVRT = s_pGetVoiceRecoveryTime(this);
    // if (fVRT != fVoiceTimer)
    //    spdlog::warn(__FUNCTION__ ": fVRT {}, fVoiceTimer {}", fVRT, fVoiceTimer);

    return fVoiceTimer;
}

bool Actor::IsSpeakingInScene() 
{
    auto pScene = GetCurrentScene();
    bool isSpeakingInScene = IsInScene(); 
    isSpeakingInScene = isSpeakingInScene && GetVoiceRecoveryTime() > 0.0f;
    const bool isTalking = IsTalking(); 
    const bool isLeader = World::Get().GetPartyService().IsLeader(); // Helps distinguish logs in 2-party

    spdlog::debug(__FUNCTION__ ": isSpeakingInScene {}, isTalking {}, voiceRecoveryTime {}, isLeader {}, formId {:X}, name {}", isSpeakingInScene, isTalking, GetVoiceRecoveryTime(), isLeader, formID, baseForm ? baseForm->GetName() : "<null>");

    return isSpeakingInScene;
}

char TP_MAKE_THISCALL(HookActorProcess, Actor, float aDeltaTime)
{
    // Suppress local AI movement for remote actors while keeping Havok stepInfo valid
    // so SetPositionImpl never divides by 0 / stale deltaTime.
    if (apThis->GetExtension()->IsRemote())
    {
        if (apThis->currentProcess)
        {
            if (auto* pController = apThis->currentProcess->GetCharController())
                pController->UpdateStepTiming(aDeltaTime);
        }
        return 0;
    }

    return TiltedPhoques::ThisCall(RealActorProcess, apThis, aDeltaTime);
}

TP_THIS_FUNCTION(TAddDeathItems, void, Actor);
static TAddDeathItems* RealAddDeathItems = nullptr;

void TP_MAKE_THISCALL(HookAddDeathItems, Actor)
{
    if (apThis->GetExtension()->IsRemote())
        return;

    TiltedPhoques::ThisCall(RealAddDeathItems, apThis);
}

TP_THIS_FUNCTION(TIsFleeing, bool, Actor);
static TIsFleeing* RealIsFleeing = nullptr;

bool TP_MAKE_THISCALL(HookIsFleeing, Actor)
{
    // TODO: Player or RemotePlayer? Can players be in fleeing mode in skyrim?
    // TODO: investigate why the flee flag is set at all on remote players sometimes.
    if (apThis->GetExtension()->IsPlayer())
        return false;
    
    return TiltedPhoques::ThisCall(RealIsFleeing, apThis);
}

static TiltedPhoques::Initializer s_actorHooks(
    []()
    {
        POINTER_SKYRIMSE(TActorProcess, s_actorProcess, 36365);
        POINTER_SKYRIMSE(TSetPosition, s_setPosition, 19363);
        POINTER_SKYRIMSE(TRemoveSpell, s_removeSpell, 37772);
        POINTER_SKYRIMSE(TCharacterConstructor, s_characterCtor, 39171);
        POINTER_SKYRIMSE(TCharacterConstructor2, s_characterCtor2, 39172);
        POINTER_SKYRIMSE(TCharacterDestructor, s_characterDtor, 36196);
        POINTER_SKYRIMSE(TGetLocation, s_GetActorLocation, 19385);
        POINTER_SKYRIMSE(TForceState, s_ForceState, 36323);
        POINTER_SKYRIMSE(TSpawnActorInWorld, s_SpawnActorInWorld, 19315);
        POINTER_SKYRIMSE(TDamageActor, s_damageActor, 36345);
        POINTER_SKYRIMSE(TApplyActorEffect, s_applyActorEffect, 34286);
        POINTER_SKYRIMSE(TAddInventoryItem, s_addInventoryItem, 36525);
        POINTER_SKYRIMSE(TPickUpObject, s_pickUpObject, 36521);
        POINTER_SKYRIMSE(TDropObject, s_dropObject, 39382);
        POINTER_SKYRIMSE(TUpdateDetectionState, s_updateDetectionState, 41626);
        POINTER_SKYRIMSE(TProcessResponse, s_processResponse, 38612);
        POINTER_SKYRIMSE(TInitiateMountPackage, s_initiateMountPackage, 36881);
        POINTER_SKYRIMSE(TUnequipObject, s_unequipObject, 36950);
        POINTER_SKYRIMSE(TSpeakSoundFunction, s_speakSoundFunction, 36541);
        POINTER_SKYRIMSE(TAddDeathItems, addDeathItems, 36218);
        POINTER_SKYRIMSE(TIsFleeing, isFleeing, 36575);
        POINTER_SKYRIMSE(TRestoreActorValue, s_restoreActorValue, 37513);

        RealActorProcess = s_actorProcess.Get();
        RealSetPosition = s_setPosition.Get();
        RealRemoveSpell = s_removeSpell.Get();
        FUNC_GetActorLocation = s_GetActorLocation.Get();
        RealCharacterConstructor = s_characterCtor.Get();
        RealCharacterConstructor2 = s_characterCtor2.Get();
        RealForceState = s_ForceState.Get();
        RealSpawnActorInWorld = s_SpawnActorInWorld.Get();
        RealDamageActor = s_damageActor.Get();
        RealApplyActorEffect = s_applyActorEffect.Get();
        RealAddInventoryItem = s_addInventoryItem.Get();
        RealPickUpObject = s_pickUpObject.Get();
        RealDropObject = s_dropObject.Get();
        RealUpdateDetectionState = s_updateDetectionState.Get();
        RealProcessResponse = s_processResponse.Get();
        RealInitiateMountPackage = s_initiateMountPackage.Get();
        RealUnequipObject = s_unequipObject.Get();
        RealSpeakSoundFunction = s_speakSoundFunction.Get();
        RealAddDeathItems = addDeathItems.Get();
        RealIsFleeing = isFleeing.Get();
        RealRestoreActorValue = s_restoreActorValue.Get();

        TP_HOOK(&RealActorProcess, HookActorProcess);
        TP_HOOK(&RealSetPosition, HookSetPosition);
        TP_HOOK(&RealRemoveSpell, HookRemoveSpell);
        TP_HOOK(&RealCharacterConstructor, HookCharacterConstructor);
        TP_HOOK(&RealCharacterConstructor2, HookCharacterConstructor2);
        TP_HOOK(&RealForceState, HookForceState);
        TP_HOOK(&RealSpawnActorInWorld, HookSpawnActorInWorld);
        TP_HOOK(&RealDamageActor, HookDamageActor);
        TP_HOOK(&RealApplyActorEffect, HookApplyActorEffect);
        TP_HOOK(&RealAddInventoryItem, HookAddInventoryItem);
        TP_HOOK(&RealPickUpObject, HookPickUpObject);
        TP_HOOK(&RealDropObject, HookDropObject);
        TP_HOOK(&RealUpdateDetectionState, HookUpdateDetectionState);
        TP_HOOK(&RealProcessResponse, HookProcessResponse);
        TP_HOOK(&RealInitiateMountPackage, HookInitiateMountPackage);
        TP_HOOK(&RealUnequipObject, HookUnequipObject);
        TP_HOOK(&RealSpeakSoundFunction, HookSpeakSoundFunction);
        TP_HOOK(&RealAddDeathItems, HookAddDeathItems);
        TP_HOOK(&RealIsFleeing, HookIsFleeing);
        TP_HOOK(&RealRestoreActorValue, HookRestoreActorValue);
    });
