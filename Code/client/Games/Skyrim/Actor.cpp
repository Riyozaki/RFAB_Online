#include <Games/References.h>
#include <Games/Skyrim/EquipManager.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>
#include <Misc/GameVM.h>
#include <DefaultObjectManager.h>
#include <Forms/TESNPC.h>
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
#include <Forms/TESRace.h>

#include <Effects/ValueModifierEffect.h>

#include <Games/Skyrim/Misc/InventoryEntry.h>
#include <Games/Skyrim/ExtraData/ExtraCount.h>
#include <Games/Misc/ActorKnowledge.h>

#include <ExtraData/ExtraDataList.h>
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

void Actor::QueueUpdate() noexcept
{
    auto* pSetting = INISettingCollection::Get()->GetSetting("bUseFaceGenPreprocessedHeads:General");
    const auto originalValue = pSetting->data;
    pSetting->data = 0;

    TP_THIS_FUNCTION(TQueueUpdate, void, Actor, bool);
    POINTER_SKYRIMSE(TQueueUpdate, QueueUpdate, 39181);

    TiltedPhoques::ThisCall(QueueUpdate, this, true);

    pSetting->data = originalValue;
}

GamePtr<Actor> Actor::Create(TESNPC* apBaseForm) noexcept
{
    if (!apBaseForm)
        return nullptr;

    const auto pPlayer = static_cast<Actor*>(GetById(0x14));
    if (!pPlayer || (!pPlayer->parentCell && !pPlayer->GetWorldSpace()))
        return nullptr;

    auto pActor = New();
    if (!pActor)
        return nullptr;

    // Prevent saving
    pActor->SetSkipSaveFlag(true);
    pActor->GetExtension()->SetRemote(true);

    auto pCell = pPlayer->parentCell;
    const auto pWorldSpace = pPlayer->GetWorldSpace();

    pActor->SetLevelMod(4);
    pActor->SetParentCell(pCell);
    pActor->SetBaseForm(apBaseForm);

    auto position = pPlayer->position;
    auto rotation = pPlayer->rotation;

    if (pCell && !(pCell->cellFlags[0] & 1))
        pCell = nullptr;

    ModManager::Get()->Spawn(position, rotation, pCell, pWorldSpace, pActor);

    pActor->ForcePosition(position);

    pActor->GetMagicCaster(MagicSystem::CastingSource::LEFT_HAND);
    pActor->GetMagicCaster(MagicSystem::CastingSource::RIGHT_HAND);
    pActor->GetMagicCaster(MagicSystem::CastingSource::OTHER);

    pActor->flags &= 0xFFDFFFFF;
    pActor->SetSkipSaveFlag(true);
    pActor->UnsetChanged(0xFFFFFFFF);

    return pActor;
}

GamePtr<Actor> Actor::Spawn(uint32_t aBaseFormId) noexcept
{
    TESNPC* pNpc = Cast<TESNPC>(TESForm::GetById(aBaseFormId));
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

extern thread_local bool g_forceAnimation;

void Actor::SetWeaponDrawnEx(bool aDraw) noexcept
{
    spdlog::debug("Setting weapon drawn: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());

    if (actorState.IsWeaponDrawn() == aDraw)
    {
        actorState.SetWeaponDrawn(!aDraw);

        spdlog::debug("Setting weapon drawn after update: {:X}:{}, current state: {}", formID, aDraw, actorState.IsWeaponDrawn());
    }

    g_forceAnimation = true;
    SetWeaponDrawn(aDraw);
    g_forceAnimation = false;
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
        auto pPlayerFaction = Cast<TESFaction>(TESForm::GetById(0xDB1));
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
    s_pStopCombat(this);
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
    bool result = TiltedPhoques::ThisCall(RealRemoveSpell, apThis, apSpell);
    if (apThis->GetExtension()->IsLocalPlayer() && result)
    {
        //spdlog::info(__FUNCTION__ ": spell: {}, ID: {} from local player", apSpell->GetName() , apSpell->formID);
       RemoveSpellEvent removalEvent;

        removalEvent.TargetId = apThis->formID;
        removalEvent.SpellId = apSpell->formID;
        World::Get().GetRunner().Trigger(removalEvent);
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
        auto& factions = pNpc->actorData.factions;

        for (auto i = 0u; i < factions.length; ++i)
        {
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
    spdlog::info("Setting inventory for actor {:X}", formID);

    // The UnEquipAll() that used to be here is redundant,
    // as RemoveAllItems() unequips every item if needed.
    // Placing this UnEquipAll() here seems to trigger a Skyrim bug/race.

    Inventory currentInventory = GetActorInventory();

    if (!this->GetExtension()->IsPlayer() && currentInventory.ContainsQuestItems())
        SetInventoryRetainingQuestItems(currentInventory, acInventory);
    else
        SetInventory(acInventory);

    SetMagicEquipment(acInventory.CurrentMagicEquipment);
}

void Actor::SetMagicEquipment(const MagicEquipment& acEquipment) noexcept
{
    auto* pEquipManager = EquipManager::Get();
    auto& modSystem = World::Get().GetModSystem();

    if (acEquipment.LeftHandSpell)
    {
        uint32_t mainHandWeaponId = modSystem.GetGameId(acEquipment.LeftHandSpell);
        spdlog::debug("Setting left hand spell: {:X}", mainHandWeaponId);
        pEquipManager->EquipSpell(this, TESForm::GetById(mainHandWeaponId), 0);
    }

    if (acEquipment.RightHandSpell)
    {
        uint32_t secondaryHandWeaponId = modSystem.GetGameId(acEquipment.RightHandSpell);
        spdlog::debug("Setting right hand spell: {:X}", secondaryHandWeaponId);
        pEquipManager->EquipSpell(this, TESForm::GetById(secondaryHandWeaponId), 1);
    }

    if (acEquipment.Shout)
    {
        uint32_t shoutId = modSystem.GetGameId(acEquipment.Shout);
        spdlog::debug("Setting shout: {:X}", shoutId);
        pEquipManager->EquipShout(this, TESForm::GetById(shoutId));
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

    for (auto& value : acActorValues.ActorMaxValuesList)
    {
        if (isLocallyDerivedSpeedAv(value.first))
            continue;

        ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, value.first, value.second);
    }

    for (auto& value : acActorValues.ActorValuesList)
    {
        if (isLocallyDerivedSpeedAv(value.first))
            continue;

        ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, value.first, value.second);
    }
}

void Actor::SetFactions(const Factions& acFactions) noexcept
{
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

    g_forceAnimation = false;
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

// TODO: this is flawed, since it does not account for invulnerable actors
bool TP_MAKE_THISCALL(HookDamageActor, Actor, float aDamage, Actor* apHitter, bool aKillMove)
{
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

    const float currentHealth = apThis->GetActorValue(ActorValueInfo::kHealth);
    const float estimatedRealDamage = GameplayFormulas::CalculateRealDamage(apThis, aDamage, aKillMove);
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

        const float healthBefore = apThis->GetActorValue(ActorValueInfo::kHealth);
        const bool result = TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, aKillMove);
        const float healthAfter = apThis->GetActorValue(ActorValueInfo::kHealth);
        const float actualDelta = healthAfter - healthBefore;

        // If the remote NPC survived this hit, immediately restore kInfamy >= 0 so if ownership later transfers
        // to this client (TakeOwnership), RFAB_XP_Alias::OnActorKilled will still award full kill XP!
        if (isRemoteNonPlayer && !apThis->IsDead() && healthAfter > 0.0f)
        {
            ScopedActorValueOverride avOverride;
            apThis->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, std::max(0.0f, prevInfamy));
        }

        // Broadcast the exact delta produced by SkyrimSE.exe + RFAB.dll so invulnerable/ethereal
        // boss phases, Requiem armor/expertise/silver modifiers, damage caps, and ScriptDamage stay 100% synced.
        if (actualDelta != 0.f)
            World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, actualDelta));

        return result;
    };

    const auto* pExHittee = apThis->GetExtension();
    if (pExHittee->IsLocalPlayer())
    {
        if (!World::Get().GetServerSettings().PvpEnabled && !coopPolicy.AllowPartySparring)
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

            // Cushion temporaryModifier across RealDamageActor so RFAB.dll / HookDamage.dll / Requiem multipliers
            // execute 100% natively without ever reaching <= 0 HP or firing Actor::Kill / REQ_DeathHandler / killmoves.
            ScopedDamageHookGuard dmgGuard;
            constexpr float kLethalGuardCushion = 65536.0f;
            const float healthBefore = apThis->GetActorValue(ActorValueInfo::kHealth);
            const float savedTempMod = apThis->healthModifiers.temporaryModifier;
            apThis->healthModifiers.temporaryModifier = savedTempMod + kLethalGuardCushion;

            const bool result = TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter, false);

            apThis->healthModifiers.temporaryModifier = savedTempMod;
            const float realHealthAfter = apThis->GetActorValue(ActorValueInfo::kHealth);

            if (realHealthAfter <= 1.0f)
            {
                downedSvc.FinalizeLethalHitCushion(apThis, healthBefore);
                return false;
            }

            const float actualDelta = realHealthAfter - healthBefore;
            if (actualDelta != 0.f)
                World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, actualDelta));

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
            return applyAuthoritativeDamage();
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
    const auto* pValueModEffect = Cast<ValueModifierEffect>(apThis);

    if (pValueModEffect)
    {
        if (pValueModEffect->actorValueIndex == ActorValueInfo::kHealth && aEffectValue > 0.0f)
        {
            if (apTarget && apTarget->GetExtension())
            {
                const auto pExTarget = apTarget->GetExtension();
                if (pExTarget->IsLocal())
                {
                    if (pExTarget->IsLocalPlayer() &&
                        World::Get().GetDownedStateService().ShouldBlockPlayerHealthMutation(aEffectValue))
                    {
                        return;
                    }

                    World::Get().GetRunner().Trigger(HealthChangeEvent(apTarget->formID, aEffectValue));
                    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, unk1);
                }
                return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, unk1);
}

TP_THIS_FUNCTION(TRestoreActorValue, void, Actor, ActorValueOwner::ForceMode aMode, uint32_t aId, float aValue);
static TRestoreActorValue* RealRestoreActorValue = nullptr;

// Unified hook on Actor::RestoreActorValue (SE ID 37513 @ 0x620900, exact ABI: RCX = Actor*, EDX = ForceMode, R8D = AV, XMM3 = float):
// 1. Intercepts ALL direct health damage on local player (including RFAB.dll custom combat/magic damage,
//    poisons, bleeds, fall damage, and scripts) BEFORE kHealth ever goes <= 0 (-50..-500 impossible!).
// 2. Blocks 100% of unauthorized heals/regen/potions/DoTs on local player while Downed-on-Knee or in revive grace.
// 3. Blocks passive local health regeneration on remote actors without interfering with authoritative damage/sync.
void TP_MAKE_THISCALL(HookRestoreActorValue, Actor, ActorValueOwner::ForceMode aMode, uint32_t aId, float aValue)
{
    if (aMode == ActorValueOwner::ForceMode::DAMAGE && aId == ActorValueInfo::kHealth && apThis)
    {
        if (auto* pExt = apThis->GetExtension())
        {
            if (pExt->IsRemote())
            {
                if (!ScopedActorValueOverride::IsOverriden() && !s_inAuthoritativeDamageHook)
                    return;

                TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, aValue);
                return;
            }
        }

        PlayerCharacter* pLocalPlayer = PlayerCharacter::Get();
        if (pLocalPlayer && apThis == pLocalPlayer)
        {
            auto& downed = World::Get().GetDownedStateService();
            if (downed.ShouldBlockPlayerHealthMutation(aValue))
                return;

            if (!s_inAuthoritativeDamageHook)
            {
                if (aValue < 0.0f)
                {
                    const float curHp = pLocalPlayer->GetActorValue(ActorValueInfo::kHealth);
                    if (downed.InterceptPotentialFatalPlayerDamage(pLocalPlayer, curHp, -aValue))
                        return;
                }

                if (!ScopedActorValueOverride::IsOverriden() && aValue != 0.0f)
                    World::Get().GetRunner().Trigger(HealthChangeEvent(pLocalPlayer->formID, aValue));
            }
            TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, aValue);
            return;
        }

        // RFAB.dll applies spell/poison/bleed/ScriptDamage directly via RestoreActorValue(DAMAGE, kHealth, -dmg),
        // bypassing Actor::DamageActor. Broadcast actual negative HP deltas on locally-owned NPCs immediately!
        if (auto* pExt = apThis->GetExtension();
            pExt && pExt->IsLocal() && !s_inAuthoritativeDamageHook && !ScopedActorValueOverride::IsOverriden() && aValue < -0.05f)
        {
            const float hpBefore = apThis->GetActorValue(ActorValueInfo::kHealth);
            TiltedPhoques::ThisCall(RealRestoreActorValue, apThis, aMode, aId, aValue);
            const float hpAfter = apThis->GetActorValue(ActorValueInfo::kHealth);
            const float actualDelta = hpAfter - hpBefore;
            if (actualDelta < -0.05f)
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
        (UI::Get() && UI::Get()->GetMenuOpen(BSFixedString("ContainerMenu")));
    const bool blockRemote =
        pExt &&
        (pExt->IsRemotePlayer() ||
         (pExt->IsRemote() && !isInteractivePlayerTransfer));

    if (!ScopedInventoryOverride::IsOverriden() && apItem && !blockRemote)
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
            const bool syncHomes = World::Get().GetServerSettings().SyncPlayerHomes;
            if (RfabCompat::IsSharedCoopKeyOrClaw(editorIdView, static_cast<uint8_t>(apItem->formType), apItem->formID, syncHomes))
                item.IsQuestItem = true;
        }

        World::Get().GetRunner().Trigger(InventoryChangeEvent(apThis->formID, std::move(item)));
    }

    TiltedPhoques::ThisCall(RealAddInventoryItem, apThis, apItem, apExtraData, aCount, apOldOwner);
}

void* TP_MAKE_THISCALL(HookPickUpObject, Actor, TESObjectREFR* apObject, int32_t aCount, bool aUnk1, float aUnk2)
{
    if (!ScopedInventoryOverride::IsOverriden() && apObject && apObject->baseForm && !apThis->GetExtension()->IsRemote())
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
            const bool syncHomes = World::Get().GetServerSettings().SyncPlayerHomes;
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
    if (apObject && !apThis->GetExtension()->IsRemote())
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
            pBestMatch->Disable();
            pBestMatch->Delete();
        }

        AddOrRemoveItem(arEntry);
    }
}

void Actor::DropObject(TESBoundObject* apObject, ExtraDataList* apExtraData, int32_t aCount, NiPoint3* apLocation, NiPoint3* apRotation) noexcept
{
    spdlog::debug("Dropping object, form id: {:X}, count: {}, actor: {:X}", apObject->formID, aCount, formID);
    BSPointerHandle<TESObjectREFR> result{};
    TiltedPhoques::ThisCall(RealDropObject, this, &result, apObject, apExtraData, aCount, apLocation, apRotation);
    if (TESObjectREFR* pDropped = TESObjectREFR::GetByHandle(result))
    {
        pDropped->SetSkipSaveFlag(true);
        pDropped->UnsetChanged(0xFFFFFFFF);
    }
}

TP_THIS_FUNCTION(TUpdateDetectionState, void, ActorKnowledge, void*);
static TUpdateDetectionState* RealUpdateDetectionState = nullptr;

void TP_MAKE_THISCALL(HookUpdateDetectionState, ActorKnowledge, void* apState)
{
    auto pOwner = TESObjectREFR::GetByHandle(apThis->hOwner);
    auto pTarget = TESObjectREFR::GetByHandle(apThis->hTarget);

    if (pOwner && pTarget)
    {
        auto pOwnerActor = Cast<Actor>(pOwner);
        auto pTargetActor = Cast<Actor>(pTarget);
        if (pOwnerActor && pTargetActor)
        {
            if (pOwnerActor->GetExtension()->IsRemotePlayer() && pTargetActor->GetExtension()->IsLocalPlayer())
            {
                spdlog::debug("Cancelling detection from remote player to local player, owner: {:X}, target: {:X}", pOwner->formID, pTarget->formID);
                return;
            }

            // Suppress AI detection against downed players (both local 0x14 and downed remote clones) directly on
            // Skyrim's AI worker threads via lock-free atomic ActorExtension::IsDownedPlayer(), preventing enemies
            // from re-detecting / re-aggroing a downed player or ignoring standing teammates.
            if (pTargetActor->GetExtension() && pTargetActor->GetExtension()->IsDownedPlayer() &&
                !(pOwnerActor->GetExtension() && pOwnerActor->GetExtension()->IsPlayer()))
            {
                if (ActorKnowledgeDetectionState* pDet = apThis->spDetection.object)
                {
                    pDet->level = -1000;
                    pDet->inLos = 0;
                    pDet->hasLos = 0;
                    pDet->unk16 = 0;
                }
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
    if (apTalkingActor)
    {
        if (apTalkingActor->GetExtension()->IsRemotePlayer())
            return 0;  
    }
    return TiltedPhoques::ThisCall(RealProcessResponse, apThis, apVoice, apTalkingActor, apTalkedToActor);
}

bool TP_MAKE_THISCALL(HookInitiateMountPackage, Actor, Actor* apMount)
{
    if (apMount && apThis->GetExtension()->IsLocal())
        World::Get().GetRunner().Trigger(MountEvent(apThis->formID, apMount->formID));

    return TiltedPhoques::ThisCall(RealInitiateMountPackage, apThis, apMount);
}

TP_THIS_FUNCTION(TUnequipObject, void, Actor, void* apUnk1, TESBoundObject* apObject, int32_t aUnk2, void* apUnk3);
static TUnequipObject* RealUnequipObject = nullptr;

void TP_MAKE_THISCALL(HookUnequipObject, Actor, void* apUnk1, TESBoundObject* apObject, int32_t aUnk2, void* apUnk3)
{
    TiltedPhoques::ThisCall(RealUnequipObject, apThis, apUnk1, apObject, aUnk2, apUnk3);
}

TP_THIS_FUNCTION(TSpeakSoundFunction, float, Actor, const char* apName, uint32_t* a3, uint32_t a4, uint32_t a5, uint32_t a6, uint64_t a7, uint64_t a8, uint64_t a9, bool a10, uint64_t a11, bool a12, bool a13, bool a14);
static TSpeakSoundFunction* RealSpeakSoundFunction = nullptr;

float TP_MAKE_THISCALL(HookSpeakSoundFunction, Actor, const char* apName, uint32_t* a3, uint32_t a4, uint32_t a5, uint32_t a6, uint64_t a7, uint64_t a8, uint64_t a9, bool a10, uint64_t a11, bool a12, bool a13, bool a14)
{
    // Note most dialogues invoke SpeakSoundFunction twice, an initial call that queues it to an update thread, then a reinvocation.

    spdlog::debug("a3: {:X}, a4: {}, a5: {}, a6: {}, a7: {}, a8: {:X}, a9: {:X}, a10: {}, a11: {:X}, a12: {}, a13: {}, a14: {}", (uint64_t)a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);

    World::Get().GetRunner().Trigger(DialogueEvent(apThis->formID, apName));
    
    return TiltedPhoques::ThisCall(RealSpeakSoundFunction, apThis, apName, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

float Actor::SpeakSound(const char* pFile)
{
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
