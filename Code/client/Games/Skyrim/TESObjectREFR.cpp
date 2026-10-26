#include <TiltedOnlinePCH.h>

#include <common/CoopInventoryDiff.h>
#include <Games/References.h>
#include <Games/Overrides.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/TES.h>

#include <World.h>
#include <Services/ObjectService.h>
#include <Services/PapyrusService.h>
#include <Events/ActivateEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/ScriptAnimationEvent.h>
#include <Events/LockChangeEvent.h>

#include <ExtraData/ExtraDataList.h>
#include <ExtraData/ExtraCharge.h>
#include <ExtraData/ExtraCount.h>
#include <ExtraData/ExtraEnchantment.h>
#include <ExtraData/ExtraHealth.h>
#include <ExtraData/ExtraPoison.h>
#include <ExtraData/ExtraSoul.h>
#include <ExtraData/ExtraTextDisplayData.h>
#include <ExtraData/ExtraWorn.h>
#include <ExtraData/ExtraWornLeft.h>
#include <Forms/EnchantmentItem.h>
#include <Forms/AlchemyItem.h>
#include <Forms/TESObjectARMO.h>
#include <Forms/TESObjectWEAP.h>
#include <Services/RfabAdapterService.h>
#include <EquipManager.h>
#include <DefaultObjectManager.h>
#include <BSAnimationGraphManager.h>
#include <Havok/BShkbAnimationGraph.h>
#include <Havok/hkbBehaviorGraph.h>
#include <Havok/hkbVariableValueSet.h>
#include <Havok/hkbStateMachine.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>

#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/AnimationVariables.h>
#include <common/CoopTeleportGuard.h>
#include <PlayerCharacter.h>

extern const AnimationGraphDescriptor* BehaviorVarPatch(BSAnimationGraphManager* pManager, Actor* pActor);

TP_THIS_FUNCTION(TActivate, bool, TESObjectREFR, TESObjectREFR* apActivator, uint8_t aUnk1, TESBoundObject* apObjectToGet, int32_t aCount, char aDefaultProcessing);
TP_THIS_FUNCTION(TAddInventoryItem, void, TESObjectREFR, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner);
TP_THIS_FUNCTION(
    TRemoveInventoryItem, BSPointerHandle<TESObjectREFR>*, TESObjectREFR, BSPointerHandle<TESObjectREFR>* apResult, TESBoundObject* apItem, int32_t aCount, ITEM_REMOVE_REASON aReason, ExtraDataList* apExtraList, TESObjectREFR* apMoveToRef, const NiPoint3* apDropLoc, const NiPoint3* apRotate);
TP_THIS_FUNCTION(TPlayAnimationAndWait, bool, void, uint32_t auiStackID, TESObjectREFR* apSelf, BSFixedString* apAnimation, BSFixedString* apEventName);
TP_THIS_FUNCTION(TPlayAnimation, bool, void, uint32_t auiStackID, TESObjectREFR* apSelf, BSFixedString* apEventName);
TP_THIS_FUNCTION(TRotate, void, TESObjectREFR, float aAngle);
TP_THIS_FUNCTION(TLockChange, void, TESObjectREFR);
TP_THIS_FUNCTION(TInternalMoveTo, bool, TESObjectREFR, uint32_t* apTargetHandle, TESObjectCELL* apTargetCell, TESWorldSpace* apTargetWorldSpace, const NiPoint3& acPosition, const NiPoint3& acRotation);

static TActivate* RealActivate = nullptr;
static TAddInventoryItem* RealAddInventoryItem = nullptr;
static TRemoveInventoryItem* RealRemoveInventoryItem = nullptr;
static TPlayAnimationAndWait* RealPlayAnimationAndWait = nullptr;
static TPlayAnimation* RealPlayAnimation = nullptr;
static TRotate* RealRotateX = nullptr;
static TRotate* RealRotateY = nullptr;
static TRotate* RealRotateZ = nullptr;
static TLockChange* RealLockChange = nullptr;
static TInternalMoveTo* RealInternalMoveTo = nullptr;

#ifdef SAVE_STUFF

#include <Games/Skyrim/SaveLoad.h>

void TESObjectREFR::Save_Reversed(const uint32_t aChangeFlags, Buffer::Writer& aWriter)
{
    TESForm::Save_Reversed(aChangeFlags, aWriter);

    if (aChangeFlags & CHANGE_REFR_BASEOBJECT)
    {
        // save baseForm->formID;
        // we don't because each player has it's own form id system
    }

    if (aChangeFlags & CHANGE_REFR_SCALE)
    {
        // So skyrim does some weird conversion shit here, we are going to do the same for now
        float fScale = scale;
        fScale /= 100.f;
        aWriter.WriteBytes((uint8_t*)&fScale, 4);
    }

    // So skyrim
    uint32_t extraFlags = 0xA6021C40;
    if (formType == Character)
        extraFlags = 0xA6061840;

    if (aChangeFlags & extraFlags)
    {
        // We have flags to save
    }

    if (aChangeFlags & (CHANGE_REFR_INVENTORY | CHANGE_REFR_LEVELED_INVENTORY))
    {
    }

    if (aChangeFlags & CHANGE_REFR_ANIMATION)
    {
        // do something with animations
        // get extradata 0x41
    }
}

#endif

TESObjectREFR* TESObjectREFR::GetByHandle(uint32_t aHandle) noexcept
{
    TESObjectREFR* pResult = nullptr;

    using TGetRefrByHandle = void(uint32_t & aHandle, TESObjectREFR * &apResult);

    POINTER_SKYRIMSE(TGetRefrByHandle, s_getRefrByHandle, 16828);

    s_getRefrByHandle.Get()(aHandle, pResult);

    if (pResult)
        pResult->handleRefObject.DecRefHandle();

    return pResult;
}

BSPointerHandle<TESObjectREFR> TESObjectREFR::GetHandle() const noexcept
{
    TP_THIS_FUNCTION(TGetHandle, BSPointerHandle<TESObjectREFR>, const TESObjectREFR, BSPointerHandle<TESObjectREFR>* apResult);
    POINTER_SKYRIMSE(TGetHandle, s_getHandle, 19418);

    BSPointerHandle<TESObjectREFR> result{};
    TiltedPhoques::ThisCall(s_getHandle, this, &result);

    return result;
}

uint32_t* TESObjectREFR::GetNullHandle() noexcept
{
    POINTER_SKYRIMSE(uint32_t, s_nullHandle, 514164);

    return s_nullHandle.Get();
}

void TESObjectREFR::SetRotation(float aX, float aY, float aZ) noexcept
{
    TiltedPhoques::ThisCall(RealRotateX, this, aX);
    TiltedPhoques::ThisCall(RealRotateY, this, aY);
    TiltedPhoques::ThisCall(RealRotateZ, this, aZ);
}

using TiltedPhoques::Serialization;

namespace
{
// Descriptor indices are only valid for the behavior project they came from, and an Actor can switch projects
// (e.g. creature re-roll, werewolf/vampire transform), so recompute whenever the behavior project changes.
void UpdateGraphDescriptorHash(Actor* apActor, BSAnimationGraphManager* apManager, int aForceIndex) noexcept
{
    auto* pExtension = apActor->GetExtension();
    const auto cProjectKey = apManager->GetProjectKey(aForceIndex);
    if (pExtension->GraphDescriptorHash != 0 && pExtension->GraphDescriptorProjectKey == cProjectKey)
        return;

    const auto cPreviousHash = pExtension->GraphDescriptorHash;
    pExtension->GraphDescriptorHash = apManager->GetDescriptorKey(aForceIndex);
    pExtension->GraphDescriptorProjectKey = cProjectKey;

    if (cPreviousHash != 0)
        spdlog::info("Animation graph project of actor {:X} changed, graph descriptor {} -> {}", apActor->formID, cPreviousHash, pExtension->GraphDescriptorHash);
}
} // namespace

void TESObjectREFR::SaveAnimationVariables(AnimationVariables& aVariables) const noexcept
{
    const auto spManager = animationGraphHolder.GetAnimationGraphManagerPtr();
    if (spManager)
    {
        BSAnimationGraphManager* pManager = spManager.get();
        BSScopedLock<BSRecursiveLock> _{pManager->lock};

        if (pManager->animationGraphIndex < pManager->animationGraphs.size)
        {
            auto* pActor = Cast<Actor>(this);
            if (!pActor)
                return;

            const BShkbAnimationGraph* pGraph = nullptr;

            if (pActor->formID == 0x14)
                pGraph = pManager->animationGraphs.Get(0);
            else
                pGraph = pManager->animationGraphs.Get(pManager->animationGraphIndex);

            if (!pGraph)
                return;

            if (!pGraph->behaviorGraph || !pGraph->behaviorGraph->stateMachine || !pGraph->behaviorGraph->stateMachine->name)
                return;

            // Force third person graph to be used on player
            UpdateGraphDescriptorHash(pActor, pManager, pActor->formID == 0x14 ? 0 : -1);
            auto* pExtendedActor = pActor->GetExtension();

            auto pDescriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(pExtendedActor->GraphDescriptorHash);

            // Modded behavior check if descriptor wasn't found
            if (!pDescriptor)
                pDescriptor = BehaviorVarPatch(pManager, pActor);

            if (!pDescriptor)
                return;

            const auto* pVariableSet = pGraph->behaviorGraph->animationVariables;

            if (!pVariableSet)
                return;

            aVariables.Booleans.assign(pDescriptor->BooleanLookUpTable.size(), false);
            aVariables.Floats.assign(pDescriptor->FloatLookupTable.size(), 0.f);
            aVariables.Integers.assign(pDescriptor->IntegerLookupTable.size(), 0);

            for (size_t i = 0; i < pDescriptor->BooleanLookUpTable.size(); ++i)
            {
                const auto idx = pDescriptor->BooleanLookUpTable[i];

                if (pVariableSet->size > idx && pVariableSet->data[idx] != 0)
                    aVariables.Booleans[i] = true;
            }

            for (size_t i = 0; i < pDescriptor->FloatLookupTable.size(); ++i)
            {
                const auto idx = pDescriptor->FloatLookupTable[i];

                if (pVariableSet->size > idx)
                    aVariables.Floats[i] = *reinterpret_cast<float*>(&pVariableSet->data[idx]);
            }

            for (size_t i = 0; i < pDescriptor->IntegerLookupTable.size(); ++i)
            {
                const auto idx = pDescriptor->IntegerLookupTable[i];

                if (pVariableSet->size > idx)
                    aVariables.Integers[i] = *reinterpret_cast<uint32_t*>(&pVariableSet->data[idx]);
            }
        }
    }
}

void TESObjectREFR::LoadAnimationVariables(const AnimationVariables& aVariables) const noexcept
{
    const auto spManager = animationGraphHolder.GetAnimationGraphManagerPtr();
    if (spManager)
    {
        BSAnimationGraphManager* pManager = spManager.get();
        BSScopedLock<BSRecursiveLock> _{pManager->lock};

        if (pManager->animationGraphIndex < pManager->animationGraphs.size)
        {
            const auto* pGraph = pManager->animationGraphs.Get(pManager->animationGraphIndex);

            if (!pGraph)
                return;

            if (!pGraph->behaviorGraph || !pGraph->behaviorGraph->stateMachine || !pGraph->behaviorGraph->stateMachine->name)
                return;

            auto* pActor = Cast<Actor>(this);
            if (!pActor)
                return;

            UpdateGraphDescriptorHash(pActor, pManager, -1);
            auto* pExtendedActor = pActor->GetExtension();

            auto pDescriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(pExtendedActor->GraphDescriptorHash);

            // Modded behavior check if descriptor wasn't found
            if (!pDescriptor)
                pDescriptor = BehaviorVarPatch(pManager, pActor);

            if (!pDescriptor)
                return;

            const auto* pBehaviorGraph = pGraph->behaviorGraph;
            const auto* pVariableSet = pBehaviorGraph->animationVariables;

            if (!pVariableSet)
                return;

            // Never write into a variable whose word is an index: writing scalar data into a pointer/vector slot
            // corrupts Havok bound-node lookups during behavior evaluation.
            for (size_t i = 0; i < pDescriptor->BooleanLookUpTable.size(); ++i)
            {
                const auto idx = pDescriptor->BooleanLookUpTable[i];

                if (pVariableSet->size > idx && pBehaviorGraph->IsWordVariable(idx))
                {
                    pVariableSet->data[idx] = aVariables.Booleans.size() > i ? aVariables.Booleans[i] : false;
                }
            }

            for (size_t i = 0; i < pDescriptor->FloatLookupTable.size(); ++i)
            {
                const auto idx = pDescriptor->FloatLookupTable[i];

                if (pVariableSet->size > idx && pBehaviorGraph->IsWordVariable(idx))
                {
                    *reinterpret_cast<float*>(&pVariableSet->data[idx]) = aVariables.Floats.size() > i ? aVariables.Floats[i] : 0.f;
                }
            }

            for (size_t i = 0; i < pDescriptor->IntegerLookupTable.size(); ++i)
            {
                const auto idx = pDescriptor->IntegerLookupTable[i];

                if (pVariableSet->size > idx && pBehaviorGraph->IsWordVariable(idx))
                {
                    *reinterpret_cast<uint32_t*>(&pVariableSet->data[idx]) = aVariables.Integers.size() > i ? aVariables.Integers[i] : 0;
                }
            }
        }
    }
}

uint32_t TESObjectREFR::GetCellId() const noexcept
{
    if (!parentCell)
        return 0;

    const auto* pWorldSpace = parentCell->worldspace;

    return pWorldSpace != nullptr ? pWorldSpace->formID : parentCell->formID;
}

TESWorldSpace* TESObjectREFR::GetWorldSpace() const noexcept
{
    auto* pParentCell = parentCell ? parentCell : GetParentCell();
    if (pParentCell && !(pParentCell->cellFlags[0] & 1))
        return pParentCell->worldspace;

    return nullptr;
}

ExtraDataList* TESObjectREFR::GetExtraDataList() noexcept
{
    return &extraData;
}

// Delete() should only be used on temporaries
void TESObjectREFR::Delete() const noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(void, ObjectReference, Delete);

    s_pDelete(this);
}

void TESObjectREFR::Disable() const noexcept
{
    // CRITICAL SE 1.5.97 ABI FIX:
    // Papyrus ObjectReference.Disable(bool abFadeOut) is a LATENT VM native expecting `const RefrOrInventoryObj&`
    // in R8 (and attempting to suspend a live Papyrus stackID when abFadeOut == true). Calling it via non-latent
    // PAPYRUS_FUNCTION passed `this` (`TESObjectREFR*`) in R8, causing SkyrimSE.exe to dereference `Actor::vtable`
    // (.rdata) as `self.pRefr` -> instant 0xC0000005 CTD when a peer changed location!
    // Always invoke native C++ virtual TESObjectREFR::DisableImpl() (vtable[0x89]) directly without touching GameVM.
    const_cast<TESObjectREFR*>(this)->DisableImpl();
}

void TESObjectREFR::Enable() const noexcept
{
    // Same as Disable(): Papyrus ObjectReference.Enable(bool abFadeIn) is latent; invoke native C++ EnableImpl() (SE ID 19373).
    const_cast<TESObjectREFR*>(this)->EnableImpl();
}

// Skyrim: MoveTo() can fail, causing the object to be deleted
void TESObjectREFR::MoveTo(TESObjectCELL* apCell, const NiPoint3& acPosition) const noexcept
{
    if (!apCell)
        return;

    ScopedReferencesOverride recursionGuard;

    Actor* pSelfActor = Cast<Actor>(const_cast<TESObjectREFR*>(this));
    if (pSelfActor)
        pSelfActor->ResetTeleportPhysics(acPosition.z);

    if (RealInternalMoveTo)
    {
        TiltedPhoques::ThisCall(RealInternalMoveTo, const_cast<TESObjectREFR*>(this), GetNullHandle(), apCell, apCell->worldspace, acPosition, rotation);
    }
    else
    {
        POINTER_SKYRIMSE(TInternalMoveTo, s_internalMoveTo, 56227);
        TiltedPhoques::ThisCall(s_internalMoveTo, const_cast<TESObjectREFR*>(this), GetNullHandle(), apCell, apCell->worldspace, acPosition, rotation);
    }

    if (pSelfActor)
        pSelfActor->ResetTeleportPhysics(acPosition.z);
}

void TESObjectREFR::PayGold(int32_t aAmount) noexcept
{
    ScopedInventoryOverride _;
    PayGoldToContainer(nullptr, aAmount);
}

void TESObjectREFR::PayGoldToContainer(TESObjectREFR* pContainer, int32_t aAmount) noexcept
{
    TP_THIS_FUNCTION(TPayGoldToContainer, void, TESObjectREFR, TESObjectREFR*, int32_t);
    POINTER_SKYRIMSE(TPayGoldToContainer, s_payGoldToContainer, 36511);
    TiltedPhoques::ThisCall(s_payGoldToContainer, this, pContainer, aAmount);
}

Lock* TESObjectREFR::GetLock() const noexcept
{
    TP_THIS_FUNCTION(TGetLock, Lock*, const TESObjectREFR);
    POINTER_SKYRIMSE(TGetLock, realGetLock, 19818);

    return TiltedPhoques::ThisCall(realGetLock, this);
}

Lock* TESObjectREFR::CreateLock() noexcept
{
    TP_THIS_FUNCTION(TCreateLock, Lock*, TESObjectREFR);
    POINTER_SKYRIMSE(TCreateLock, realCreateLock, 19816);

    return TiltedPhoques::ThisCall(realCreateLock, this);
}

void TESObjectREFR::LockChange() noexcept
{
    TiltedPhoques::ThisCall(RealLockChange, this);
}

const float TESObjectREFR::GetHeight() noexcept
{
    auto boundMax = GetBoundMax();
    return boundMax.z - GetBoundMin().z;
}

TESObjectREFR::OpenState TESObjectREFR::GetOpenState() noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(TESObjectREFR::OpenState, ObjectReference, GetOpenState);

    return s_pGetOpenState(this);
}

ExtraContainerChanges::Data* TESObjectREFR::GetContainerChanges() const noexcept
{
    TP_THIS_FUNCTION(TGetContainterChanges, ExtraContainerChanges::Data*, const TESObjectREFR);

    POINTER_SKYRIMSE(TGetContainterChanges, s_getContainerChangs, 15802);

    return TiltedPhoques::ThisCall(s_getContainerChangs, this);
}

void TESObjectREFR::RemoveAllItems() noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(void, ObjectReference, RemoveAllItems, TESObjectREFR*, bool, bool);

    ScopedEquipOverride equipOverride;

    s_pRemoveAllItems(this, nullptr, false, true);
}

TESContainer* TESObjectREFR::GetContainer() const noexcept
{
    TP_THIS_FUNCTION(TGetContainer, TESContainer*, const TESObjectREFR);

    POINTER_SKYRIMSE(TGetContainer, s_getContainer, 19276);

    return TiltedPhoques::ThisCall(s_getContainer, this);
}

int64_t TESObjectREFR::GetItemCountInInventory(TESForm* apItem) const noexcept
{
    if (!apItem)
        return 0;

    int64_t count = 0;
    if (auto* pContainer = GetContainer())
        count = pContainer->GetItemCount(apItem);

    if (auto* pChanges = GetContainerChanges(); pChanges && pChanges->entries)
    {
        for (auto pChange : *pChanges->entries)
        {
            if (pChange && pChange->form)
            {
                if (pChange->form->formID == apItem->formID)
                {
                    count += pChange->count;
                    break;
                }
            }
        }
    }

    return count;
}

TESObjectCELL* TESObjectREFR::GetParentCellEx() const noexcept
{
    return parentCell ? parentCell : GetParentCell();
}

void TESObjectREFR::GetItemFromExtraData(Inventory::Entry& arEntry, ExtraDataList* apExtraDataList) noexcept
{
    if (!apExtraDataList)
        return;

    auto& modSystem = World::Get().GetModSystem();

    if (ExtraCount* pExtraCount = Cast<ExtraCount>(apExtraDataList->GetByType(ExtraDataType::Count)))
    {
        arEntry.Count = pExtraCount->count;
    }

    if (ExtraCharge* pExtraCharge = Cast<ExtraCharge>(apExtraDataList->GetByType(ExtraDataType::Charge)))
    {
        arEntry.ExtraCharge = pExtraCharge->fCharge;
    }

    if (ExtraEnchantment* pExtraEnchantment = Cast<ExtraEnchantment>(apExtraDataList->GetByType(ExtraDataType::Enchantment)))
    {
        TP_ASSERT(pExtraEnchantment->pEnchantment, "Null enchantment in ExtraEnchantment");
        if (pExtraEnchantment->pEnchantment)
        {
            modSystem.GetServerModId(pExtraEnchantment->pEnchantment->formID, arEntry.ExtraEnchantId);

            if (pExtraEnchantment->pEnchantment->formID & 0xFF000000)
            {
                for (EffectItem* pEffectItem : pExtraEnchantment->pEnchantment->listOfEffects)
                {
                    TP_ASSERT(pEffectItem, "pEffectItem is null.");
                    if (!pEffectItem || !pEffectItem->pEffectSetting)
                        continue;

                    Inventory::EffectItem effect;
                    effect.Magnitude = pEffectItem->data.fMagnitude;
                    effect.Area = pEffectItem->data.iArea;
                    effect.Duration = pEffectItem->data.iDuration;
                    effect.RawCost = pEffectItem->fRawCost;
                    modSystem.GetServerModId(pEffectItem->pEffectSetting->formID, effect.EffectId);
                    arEntry.EnchantData.Effects.push_back(effect);
                }

                uint32_t objectId = modSystem.GetGameId(arEntry.BaseId);
                TESForm* pObject = TESForm::GetById(objectId);
                if (pObject)
                    arEntry.EnchantData.IsWeapon = pObject->formType == FormType::Weapon;
            }

            arEntry.ExtraEnchantCharge = pExtraEnchantment->usCharge;
            arEntry.ExtraEnchantRemoveUnequip = pExtraEnchantment->bRemoveOnUnequip;
        }
    }

    if (ExtraHealth* pExtraHealth = Cast<ExtraHealth>(apExtraDataList->GetByType(ExtraDataType::Health)))
    {
        arEntry.ExtraHealth = pExtraHealth->fHealth;
    }

    if (ExtraPoison* pExtraPoison = Cast<ExtraPoison>(apExtraDataList->GetByType(ExtraDataType::Poison)))
    {
        TP_ASSERT(pExtraPoison->pPoison, "Null poison in ExtraPoison");
        if (pExtraPoison && pExtraPoison->pPoison && (pExtraPoison->pPoison->formID & 0xFF000000) != 0xFF000000)
        {
            modSystem.GetServerModId(pExtraPoison->pPoison->formID, arEntry.ExtraPoisonId);
            arEntry.ExtraPoisonCount = pExtraPoison->uiCount;
        }
    }

    if (ExtraSoul* pExtraSoul = Cast<ExtraSoul>(apExtraDataList->GetByType(ExtraDataType::Soul)))
    {
        arEntry.ExtraSoulLevel = (int32_t)pExtraSoul->cSoul;
    }

    /*
    if (ExtraTextDisplayData* pExtraTextDisplayData = Cast<ExtraTextDisplayData>(apExtraDataList->GetByType(ExtraDataType::TextDisplayData)))
    {
        if (pExtraTextDisplayData->DisplayName)
            arEntry.ExtraTextDisplayName = pExtraTextDisplayData->DisplayName;
        else
            arEntry.ExtraTextDisplayName = "";
    }
    */

    arEntry.ExtraWorn = apExtraDataList->Contains(ExtraDataType::Worn);
    arEntry.ExtraWornLeft = apExtraDataList->Contains(ExtraDataType::WornLeft);

    arEntry.IsQuestItem = apExtraDataList->HasQuestObjectAlias();
}

ExtraDataList* TESObjectREFR::GetExtraDataFromItem(const Inventory::Entry& arEntry) noexcept
{
    auto& modSystem = World::Get().GetModSystem();

    ExtraDataList* pExtraDataList = nullptr;

    if (!arEntry.ContainsExtraData())
        return pExtraDataList;

    pExtraDataList = ExtraDataList::New();

    if (arEntry.ExtraCharge > 0.f)
    {
        pExtraDataList->SetChargeData(arEntry.ExtraCharge);
    }

    if (arEntry.ExtraEnchantId != 0)
    {
        EnchantmentItem* pEnchantment = nullptr;
        if (arEntry.ExtraEnchantId.ModId == 0xFFFFFFFF)
        {
            pEnchantment = EnchantmentItem::Create(arEntry.EnchantData);
        }
        else
        {
            uint32_t enchantId = modSystem.GetGameId(arEntry.ExtraEnchantId);
            pEnchantment = Cast<EnchantmentItem>(TESForm::GetById(enchantId));
        }

        TP_ASSERT(pEnchantment, "No Enchantment created or found.");
        if (pEnchantment)
            pExtraDataList->SetEnchantmentData(pEnchantment, arEntry.ExtraEnchantCharge, arEntry.ExtraEnchantRemoveUnequip);
    }

    if (arEntry.ExtraPoisonId != 0 && arEntry.ExtraPoisonId.ModId != 0xFFFFFFFF)
    {
        uint32_t poisonId = modSystem.GetGameId(arEntry.ExtraPoisonId);
        if (AlchemyItem* pPoison = Cast<AlchemyItem>(TESForm::GetById(poisonId)))
        {
            pExtraDataList->SetPoison(pPoison, arEntry.ExtraPoisonCount);
        }
    }

    if (arEntry.ExtraHealth > 0.f)
    {
        pExtraDataList->SetHealth(arEntry.ExtraHealth);
    }

    if (arEntry.ExtraSoulLevel > 0 && arEntry.ExtraSoulLevel <= 5)
    {
        pExtraDataList->SetSoulData(static_cast<SOUL_LEVEL>(arEntry.ExtraSoulLevel));
    }

    if (arEntry.ExtraWorn)
    {
        pExtraDataList->SetWorn(false);
    }

    if (arEntry.ExtraWornLeft)
    {
        pExtraDataList->SetWorn(true);
    }

    // TODO: this is causing crashes
    /*
    if (!arEntry.ExtraTextDisplayName.empty())
    {
        ExtraTextDisplayData* pExtraText = Memory::Allocate<ExtraTextDisplayData>();
        *((uint64_t*)pExtraText) = 0x1416244D0;
        pExtraText->next = nullptr;
        pExtraText->DisplayName = arEntry.ExtraTextDisplayName.c_str();
        pExtraText->usCustomNameLength = arEntry.ExtraTextDisplayName.length();
        pExtraText->iOwnerInstance = -2;
        pExtraText->fTemperFactor = 1.0F;
        pExtraDataList->Add(ExtraDataType::TextDisplayData, pExtraText);
    }
    */

    if (pExtraDataList->data == nullptr)
    {
        Memory::Delete(pExtraDataList->bitfield);
        Memory::Delete(pExtraDataList);
        pExtraDataList = nullptr;
    }

    return pExtraDataList;
}

Inventory TESObjectREFR::GetInventory() const noexcept
{
    return GetInventory([](TESForm& aForm) { return true; });
}

Inventory TESObjectREFR::GetInventory(std::function<bool(TESForm&)> aFilter) const noexcept
{
    auto& modSystem = World::Get().GetModSystem();
    Inventory inventory{};

    if (TESContainer* pBaseContainer = GetContainer())
    {
        for (int i = 0; i < pBaseContainer->count; i++)
        {
            TESContainer::Entry* pGameEntry = pBaseContainer->entries[i];
            if (!pGameEntry || !pGameEntry->form)
            {
                spdlog::warn("Entry or form for inventory item is null.");
                continue;
            }

            if (!aFilter(*pGameEntry->form))
                continue;

            Inventory::Entry entry;
            modSystem.GetServerModId(pGameEntry->form->formID, entry.BaseId);
            entry.Count = pGameEntry->count;

            inventory.Entries.push_back(std::move(entry));
        }
    }

    Inventory extraInventory{};

    if (auto* pContainerChanges = GetContainerChanges(); pContainerChanges && pContainerChanges->entries)
    {
        for (auto pGameEntry : *pContainerChanges->entries)
        {
            if (!pGameEntry || !pGameEntry->form)
                continue;

            if (!aFilter(*pGameEntry->form))
                continue;

            Inventory::Entry entry{};
            modSystem.GetServerModId(pGameEntry->form->formID, entry.BaseId);
            entry.Count = pGameEntry->count;

            if (pGameEntry->dataList)
            {
                for (ExtraDataList* pExtraDataList : *pGameEntry->dataList)
                {
                    if (!pExtraDataList)
                        continue;

                    Inventory::Entry innerEntry;
                    innerEntry.BaseId = entry.BaseId;
                    innerEntry.Count = 1;

                    GetItemFromExtraData(innerEntry, pExtraDataList);

                    entry.Count -= innerEntry.Count;

                    extraInventory.Entries.push_back(std::move(innerEntry));
                }
            }

            if (entry.Count != 0)
                extraInventory.Entries.push_back(std::move(entry));
        }
    }

    spdlog::debug("ExtraInventory count: {}", extraInventory.Entries.size());

    Inventory minimizedExtraInventory{};

    for (auto& entry : extraInventory.Entries)
    {
        auto duplicate = std::find_if(minimizedExtraInventory.Entries.begin(), minimizedExtraInventory.Entries.end(), [entry](const Inventory::Entry& newEntry) { return newEntry.CanBeMerged(entry); });

        if (duplicate == std::end(minimizedExtraInventory.Entries))
        {
            minimizedExtraInventory.Entries.push_back(entry);
            continue;
        }

        duplicate->Count += entry.Count;
    }

    spdlog::debug("MinExtraInventory count: {}", minimizedExtraInventory.Entries.size());

    for (auto& entry : minimizedExtraInventory.Entries)
    {
        if (entry.ContainsExtraData())
            continue;

        auto duplicate = std::find_if(inventory.Entries.begin(), inventory.Entries.end(), [entry](const Inventory::Entry& newEntry) { return newEntry.CanBeMerged(entry); });

        if (duplicate == std::end(inventory.Entries))
            continue;

        entry.Count += duplicate->Count;
        duplicate->Count = 0;
    }

    spdlog::debug("MinExtraInventory count after: {}", minimizedExtraInventory.Entries.size());

    inventory.Entries.insert(inventory.Entries.end(), minimizedExtraInventory.Entries.begin(), minimizedExtraInventory.Entries.end());

    spdlog::debug("Inventory count before: {}", inventory.Entries.size());

    inventory.RemoveByFilter([](const auto& entry) { return entry.Count == 0; });

    spdlog::debug("Inventory count after: {}", inventory.Entries.size());

    return inventory;
}

Inventory TESObjectREFR::GetArmor() const noexcept
{
    return GetInventory([](TESForm& aForm) { return aForm.formType == FormType::Armor; });
}

Inventory TESObjectREFR::GetWornArmor() const noexcept
{
    Inventory wornArmor = GetArmor();
    wornArmor.RemoveByFilter([](const auto& entry) { return !entry.IsWorn(); });
    return wornArmor;
}

bool TESObjectREFR::IsItemInInventory(uint32_t aFormID) const noexcept
{
    Inventory inventory = GetInventory([aFormID](TESForm& aForm) { return aForm.formID == aFormID; });
    return !inventory.Entries.empty();
}

namespace
{
bool ShouldSkipRemoteRingEquip(Actor* apActor, TESBoundObject* apObject, uint32_t aObjectId) noexcept
{
    if (!apActor || !apObject || !apActor->GetExtension() || !apActor->GetExtension()->IsRemotePlayer())
        return false;

    if (const auto* pArmor = Cast<TESObjectARMO>(apObject);
        pArmor && (pArmor->slotType & (1u << 6u)) != 0)
    {
        return true;
    }

    const auto& rfabAdapter = World::Get().GetRfabAdapterService();
    const auto& bindings = rfabAdapter.GetBindings();
    if (TESForm* pAdvRing = rfabAdapter.ResolveFormByEditorIdOrBaseId(
            bindings.AdventurerRingEditorId, bindings.FallbackAdventurerRingBaseFormId);
        pAdvRing && pAdvRing->formID == aObjectId)
    {
        return true;
    }

    return (aObjectId & 0x00FFFFFFu) == bindings.FallbackAdventurerRingBaseFormId;
}

void ApplyDifferentialWornTransitions(
    Actor* apActor,
    const std::vector<CoopInventoryDiff::EquipTransition<Inventory::Entry>>& acTransitions) noexcept
{
    if (!apActor || acTransitions.empty())
        return;

    ScopedEquipOverride equipOverride_;
    ModSystem& modSystem = World::Get().GetModSystem();
    const auto& dom = DefaultObjectManager::Get();

    for (const auto& tr : acTransitions)
    {
        const uint32_t objectId = modSystem.GetGameId(tr.Item.BaseId);
        TESBoundObject* pObject = Cast<TESBoundObject>(TESForm::GetById(objectId));
        if (!pObject)
            continue;

        TESForm* pEquipSlot = nullptr;
        int32_t equipCount = 1;
        if (const auto* pWeapon = Cast<TESObjectWEAP>(pObject))
        {
            if (pWeapon->UsesSingleHandEquipSlot(dom))
                pEquipSlot = tr.Item.ExtraWornLeft ? dom.leftEquipSlot : dom.rightEquipSlot;
        }
        else if (pObject->formType == FormType::Ammo)
        {
            equipCount = std::max(1, tr.Item.Count);
        }

        if (tr.Action == CoopInventoryDiff::EquipActionKind::Equip)
        {
            if (!ShouldSkipRemoteRingEquip(apActor, pObject, objectId))
                EquipManager::Get()->Equip(apActor, pObject, nullptr, equipCount, pEquipSlot, false, true, false, false);
        }
        else if (tr.Action == CoopInventoryDiff::EquipActionKind::Unequip)
        {
            EquipManager::Get()->UnEquip(apActor, pObject, nullptr, 1, pEquipSlot, false, true, false, false, nullptr);
        }
    }
}
} // namespace

void TESObjectREFR::SetInventory(const Inventory& aInventory) noexcept
{
    spdlog::debug("Setting inventory differentially for {:X}", formID);

    ScopedInventoryOverride _;

    const Inventory currentInventory = GetInventory();
    const auto delta = CoopInventoryDiff::ComputeInventoryDelta<Inventory::Entry>(
        currentInventory.Entries,
        aInventory.Entries,
        false);

    if (delta.IsNoOp())
        return;

    for (const Inventory::Entry& rem : delta.ItemsToRemove)
        AddOrRemoveItem(rem, true);

    for (const Inventory::Entry& add : delta.ItemsToAdd)
        AddOrRemoveItem(add, true);

    if (Actor* pActor = Cast<Actor>(this))
        ApplyDifferentialWornTransitions(pActor, delta.WornStateChanges);
}

Vector<uint32_t> TESObjectREFR::RemoveNonQuestItems(Inventory& aCurrentInventory) noexcept
{
    ScopedEquipOverride equipOverride_;

    Vector<uint32_t> questEntries{};

    auto& modSystem = World::Get().GetModSystem();

    for (auto& entry : aCurrentInventory.Entries)
    {
        if (entry.IsQuestItem)
        {
            uint32_t gameId = modSystem.GetGameId(entry.BaseId);
            questEntries.emplace_back(gameId);
            continue;
        }

        if (entry.Count <= 0)
            continue;

        entry.Count = -entry.Count;
        AddOrRemoveItem(entry, true);
    }

    return questEntries;
}

void TESObjectREFR::SetInventoryRetainingQuestItems(Inventory& aCurrentInventory, const Inventory& acSourceInventory) noexcept
{
    spdlog::debug("Setting inventory differentially (retaining quest items) for {:X}", formID);

    ScopedInventoryOverride _;

    const auto delta = CoopInventoryDiff::ComputeInventoryDelta<Inventory::Entry>(
        aCurrentInventory.Entries,
        acSourceInventory.Entries,
        true);

    if (delta.IsNoOp())
        return;

    for (const Inventory::Entry& rem : delta.ItemsToRemove)
        AddOrRemoveItem(rem, true);

    for (const Inventory::Entry& add : delta.ItemsToAdd)
        AddOrRemoveItem(add, true);

    if (Actor* pActor = Cast<Actor>(this))
        ApplyDifferentialWornTransitions(pActor, delta.WornStateChanges);
}

void TESObjectREFR::AddOrRemoveItem(const Inventory::Entry& arEntry, bool aIsSettingInventory) noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();

    uint32_t objectId = modSystem.GetGameId(arEntry.BaseId);
    TESBoundObject* pObject = Cast<TESBoundObject>(TESForm::GetById(objectId));
    if (!pObject)
    {
        spdlog::warn("{}: Object to add not found, {:X}:{:X}.", __FUNCTION__, arEntry.BaseId.ModId, arEntry.BaseId.BaseId);
        return;
    }

    if (arEntry.Count > 0)
    {
        ExtraDataList* pExtraDataList = GetExtraDataFromItem(arEntry);

        bool isWorn = false;
        bool isWornLeft = false;
        if (pExtraDataList)
        {
            isWorn = pExtraDataList->Contains(ExtraDataType::Worn);
            isWornLeft = pExtraDataList->Contains(ExtraDataType::WornLeft);
        }

        spdlog::debug("Adding item {:X}, count {}", pObject->formID, arEntry.Count);
        AddObjectToContainer(pObject, pExtraDataList, arEntry.Count, nullptr);

        Actor* pActor = Cast<Actor>(this);
        if ((isWorn || isWornLeft) && pActor)
        {
            // Never equip Biped slot 36 rings (slotType & (1u << 6)) or RFAB's self-destructing Adventurer's Ring
            // on remote player clones (RFAB.dll's 2-ring patch applies strictly to 0x14, and rings have invisible 3D
            // on remote puppets anyway; equipping a ring on a remote puppet can displace RFAB_ExperienceRing and
            // fire OnUnequipped, clobbering global RFAB_XP_Bonus_IsActive = 0).
            bool skipRemoteRing = false;
            if (pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer())
            {
                if (const auto* pArmor = Cast<TESObjectARMO>(pObject);
                    pArmor && (pArmor->slotType & (1u << 6u)) != 0)
                {
                    skipRemoteRing = true;
                }
                else
                {
                    const auto& rfabAdapter = World::Get().GetRfabAdapterService();
                    const auto& bindings = rfabAdapter.GetBindings();
                    if (TESForm* pAdvRing = rfabAdapter.ResolveFormByEditorIdOrBaseId(
                            bindings.AdventurerRingEditorId, bindings.FallbackAdventurerRingBaseFormId);
                        pAdvRing && pAdvRing->formID == objectId)
                    {
                        skipRemoteRing = true;
                    }
                    else if ((objectId & 0x00FFFFFFu) == bindings.FallbackAdventurerRingBaseFormId)
                    {
                        skipRemoteRing = true;
                    }
                }
            }

            if (!skipRemoteRing)
            {
                // CRITICAL FOR SKYRIM SE 1.5.97 + RFAB COMBAT:
                // Only pass rightEquipSlot / leftEquipSlot (and count = 1) for genuine single-hand weapons/staves!
                // Passing rightEquipSlot on 2H greatswords/battleaxes/warhammers, bows, crossbows, shields, armor,
                // or arrows forces 2H weapons into 1H hand slots (swinging 2H weapons one-handed or failing to attack),
                // breaks bow+arrow / crossbow+bolt equipping, and corrupts stacked 1H dagger/axe equipping.
                const auto& dom = DefaultObjectManager::Get();
                TESForm* pEquipSlot = nullptr;
                int32_t equipCount = 1;

                if (const auto* pWeapon = Cast<TESObjectWEAP>(pObject))
                {
                    if (pWeapon->UsesSingleHandEquipSlot(dom))
                        pEquipSlot = isWornLeft ? dom.leftEquipSlot : dom.rightEquipSlot;
                }
                else if (pObject->formType == FormType::Ammo)
                {
                    equipCount = arEntry.Count;
                }

                EquipManager::Get()->Equip(pActor, pObject, nullptr, equipCount, pEquipSlot, false, true, false, false);
            }
        }
    }
    else if (arEntry.Count < 0)
    {
        // IMPORTANT: Pass nullptr for apExtraList on RemoveItem!
        // SkyrimSE.exe's TESObjectREFR::RemoveItem (SE 19263) compares apExtraList by raw pointer identity
        // against ExtraContainerChanges entries. Passing a freshly allocated GetExtraDataFromItem(arEntry)
        // pointer caused enchanted, tempered, charged, soul-filled, and worn NPC items to fail removal
        // on peer clients and leaked the allocated ExtraDataList!
        spdlog::debug("Removing item {:X}, count {}", pObject->formID, -arEntry.Count);
        RemoveItem(pObject, -arEntry.Count, ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
    }

    if (arEntry.Count > 0 && !aIsSettingInventory)
    {
        const char* pEditorId = pObject->GetFormEditorID();
        const std::string_view editorIdView = pEditorId ? std::string_view(pEditorId) : std::string_view{};
        const uint8_t rawFormType = static_cast<uint8_t>(pObject->formType);
        const bool syncHomes = World::Get().GetServerSettings().SyncPlayerHomes;

        if ((arEntry.IsQuestItem || RfabCompat::IsSharedCoopKeyOrClaw(editorIdView, rawFormType, objectId, syncHomes)) &&
            !RfabCompat::IsExcludedFromQuestItemReplication(editorIdView, rawFormType, objectId))
        {
            PlayerCharacter* pPlayer = PlayerCharacter::Get();
            if (pPlayer && !pPlayer->IsItemInInventory(objectId))
            {
                Actor* pActor = Cast<Actor>(this);
                if (pActor && pActor->GetExtension() && pActor->GetExtension()->IsRemotePlayer())
                {
                    Inventory::Entry cleanQuestEntry = arEntry;
                    cleanQuestEntry.Count = 1;
                    cleanQuestEntry.ExtraWorn = false;
                    cleanQuestEntry.ExtraWornLeft = false;
                    cleanQuestEntry.IsQuestItem = false;

                    ScopedInventoryOverride invGuard;
                    pPlayer->AddOrRemoveItem(cleanQuestEntry, true);
                }
            }
        }
    }

    UpdateItemList(nullptr);
}

void TESObjectREFR::UpdateItemList(TESForm* pUnkForm) noexcept
{
    TP_THIS_FUNCTION(TUpdateItemList, void, TESObjectREFR, TESForm*);
    POINTER_SKYRIMSE(TUpdateItemList, updateItemList, 51911);
    TiltedPhoques::ThisCall(updateItemList, this, pUnkForm);
}

namespace
{
std::mutex s_directActivationMutex{};
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_recentDirectObjectActivations{};

void RecordDirectObjectActivation(uint32_t aFormId) noexcept
{
    if (aFormId == 0)
        return;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(s_directActivationMutex);
    for (auto it = s_recentDirectObjectActivations.begin(); it != s_recentDirectObjectActivations.end();)
    {
        if (now - it->second > std::chrono::milliseconds(5000))
            it = s_recentDirectObjectActivations.erase(it);
        else
            ++it;
    }
    s_recentDirectObjectActivations[aFormId] = now;
}

bool WasObjectDirectlyActivatedRecently(uint32_t aFormId) noexcept
{
    if (aFormId == 0)
        return false;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(s_directActivationMutex);
    const auto it = s_recentDirectObjectActivations.find(aFormId);
    if (it == s_recentDirectObjectActivations.end())
        return false;

    return (now - it->second) <= std::chrono::milliseconds(2000);
}
} // namespace

bool TESObjectREFR::Activate(TESObjectREFR* apActivator, uint8_t aUnk1, TESBoundObject* aObjectToGet, int32_t aCount, char aDefaultProcessing) noexcept
{
    ScopedActivateOverride _;
    RecordDirectObjectActivation(formID);

    return TiltedPhoques::ThisCall(RealActivate, this, apActivator, aUnk1, aObjectToGet, aCount, aDefaultProcessing);
}

void TESObjectREFR::EnableImpl() noexcept
{
    TP_THIS_FUNCTION(TEnableImpl, void, TESObjectREFR, bool aResetInventory);

    POINTER_SKYRIMSE(TEnableImpl, s_enable, 19373);

    TiltedPhoques::ThisCall(s_enable, this, false);
}

uint32_t TESObjectREFR::GetAnimationVariableInt(BSFixedString* apVariableName) noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(uint32_t, ObjectReference, GetAnimationVariableInt, BSFixedString*);

    return s_pGetAnimationVariableInt(this, apVariableName);
}

static thread_local bool s_cancelAnimationWaitEvent = false;

bool TESObjectREFR::PlayAnimationAndWait(BSFixedString* apAnimation, BSFixedString* apEventName) noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(bool, ObjectReference, PlayAnimationAndWait, BSFixedString*, BSFixedString*);

    s_cancelAnimationWaitEvent = true;
    bool result = s_pPlayAnimationAndWait(this, apAnimation, apEventName);
    s_cancelAnimationWaitEvent = false;
    return result;
}

#define OBJECT_ANIM_SYNC 1

bool TP_MAKE_THISCALL(HookPlayAnimationAndWait, void, uint32_t auiStackID, TESObjectREFR* apSelf, BSFixedString* apAnimation, BSFixedString* apEventName)
{
    const char* animStr = apAnimation ? apAnimation->AsAscii() : nullptr;
    const char* evtStr = apEventName ? apEventName->AsAscii() : nullptr;
    spdlog::debug("Animation: {}, EventName: {}", animStr ? animStr : "<null>", evtStr ? evtStr : "<null>");

#if OBJECT_ANIM_SYNC
    if (!s_cancelAnimationWaitEvent &&
        apSelf &&
        !apSelf->IsTemporary() &&
        apSelf->formType != Actor::Type &&
        animStr && animStr[0] != '\0' &&
        !WasObjectDirectlyActivatedRecently(apSelf->formID))
    {
        World::Get().GetRunner().Trigger(
            ScriptAnimationEvent(apSelf->formID, animStr, evtStr ? evtStr : String{}));
    }
#endif

    return TiltedPhoques::ThisCall(RealPlayAnimationAndWait, apThis, auiStackID, apSelf, apAnimation, apEventName);
}

static thread_local bool s_cancelAnimationEvent = false;

bool TESObjectREFR::PlayAnimation(BSFixedString* apEventName) noexcept
{
    using ObjectReference = TESObjectREFR;

    PAPYRUS_FUNCTION(bool, ObjectReference, PlayAnimation, BSFixedString*);

    s_cancelAnimationEvent = true;
    bool result = s_pPlayAnimation(this, apEventName);
    s_cancelAnimationEvent = false;
    return result;
}

bool TESObjectREFR::SendAnimationEvent(BSFixedString* apEventName) noexcept
{
    return animationGraphHolder.SendAnimationEvent(apEventName);
}

bool TP_MAKE_THISCALL(HookPlayAnimation, void, uint32_t auiStackID, TESObjectREFR* apSelf, BSFixedString* apEventName)
{
    const char* animStr = apEventName ? apEventName->AsAscii() : nullptr;
    spdlog::debug("EventName: {}", animStr ? animStr : "<null>");

#if OBJECT_ANIM_SYNC
    if (!s_cancelAnimationEvent &&
        apSelf &&
        !apSelf->IsTemporary() &&
        apSelf->formType != Actor::Type &&
        animStr && animStr[0] != '\0' &&
        !WasObjectDirectlyActivatedRecently(apSelf->formID))
    {
        World::Get().GetRunner().Trigger(
            ScriptAnimationEvent(apSelf->formID, animStr, String{}));
    }
#endif

    return TiltedPhoques::ThisCall(RealPlayAnimation, apThis, auiStackID, apSelf, apEventName);
}

namespace
{
bool ShouldAutoUnlockCoopArenaBarrier(TESObjectREFR* apDoor, Actor* apActivator) noexcept
{
    if (!apDoor || !apActivator || apActivator != PlayerCharacter::Get() || !apDoor->parentCell)
        return false;

    auto* pLock = apDoor->GetLock();
    if (!pLock || !pLock->IsLocked() || pLock->lockLevel < 100)
        return false;

    auto& world = World::Get();
    TiltedPhoques::Vector<uint32_t> peerFormIds;
    world.GetRfabAdapterService().CollectPartyPeerActorFormIds(peerFormIds);
    if (peerFormIds.empty())
        return false;

    bool hasPeerInSameCell = false;
    for (const uint32_t formId : peerFormIds)
    {
        Actor* pPeer = Cast<Actor>(TESForm::GetById(formId));
        if (pPeer && pPeer->parentCell == apDoor->parentCell)
        {
            hasPeerInSameCell = true;
            break;
        }
    }

    if (!hasPeerInSameCell)
        return false;

    ProcessLists* const pProcessLists = ProcessLists::Get();
    if (!pProcessLists)
        return false;

    for (uint32_t i = 0; i < pProcessLists->highActorHandleArray.length; ++i)
    {
        Actor* pNpc = Cast<Actor>(TESObjectREFR::GetByHandle(pProcessLists->highActorHandleArray[i]));
        if (!pNpc || pNpc->formID == 0 || pNpc->formID == 0x14 || pNpc->IsDead() || pNpc->parentCell != apDoor->parentCell)
            continue;
        if (const auto* pExt = pNpc->GetExtension(); pExt && pExt->IsPlayer())
            continue;

        if (pNpc->IsInCombat() || world.GetRfabAdapterService().IsBossActor(pNpc))
            return true;
    }

    return false;
}
} // namespace

bool TP_MAKE_THISCALL(HookActivate, TESObjectREFR, TESObjectREFR* apActivator, uint8_t aUnk1, TESBoundObject* apObjectToGet, int32_t aCount, char aDefaultProcessing)
{
    Actor* pActivator = Cast<Actor>(apActivator);

    // Never echo remote/scripted Activate overrides or activations performed by remote puppet actors.
    const bool shouldSyncActivate =
        !ScopedActivateOverride::IsOverriden() &&
        pActivator &&
        !pActivator->GetExtension()->IsRemote() &&
        apThis->baseForm &&
        apThis->baseForm->formType != FormType::Book;

    auto openState = TESObjectREFR::kNone;
    bool wasLockedBefore = false;

    if (shouldSyncActivate)
    {
        if (apThis->baseForm->formType == FormType::Door && ShouldAutoUnlockCoopArenaBarrier(apThis, pActivator))
        {
            if (auto* pLock = apThis->GetLock())
            {
                pLock->SetLock(false);
                apThis->LockChange();

                GameId netDoorId{};
                if (World::Get().GetModSystem().GetServerModId(apThis->formID, netDoorId))
                    World::Get().GetRfabAdapterService().BroadcastArenaBarrierUnlock(netDoorId);
            }
        }

        if (apThis->baseForm->formType == FormType::Door)
            openState = apThis->GetOpenState();

        if (apThis->baseForm->formType == FormType::Door ||
            apThis->baseForm->formType == FormType::Container)
        {
            if (const auto* pLockBefore = apThis->GetLock())
                wasLockedBefore = pLockBefore->IsLocked();
        }

        RecordDirectObjectActivation(apThis->formID);
    }

    const bool result = TiltedPhoques::ThisCall(RealActivate, apThis, apActivator, aUnk1, apObjectToGet, aCount, aDefaultProcessing);

    if (shouldSyncActivate)
    {
        // If RealActivate unlocked the door/chest using a Key in the player's inventory,
        // Skyrim's key-unlock path clears Lock::kLocked without invoking TESObjectREFR::LockChange.
        // Emit LockChangeEvent first so the server and all peers unlock the object before ActivateEvent!
        if (wasLockedBefore)
        {
            const auto* pLockAfter = apThis->GetLock();
            if (!pLockAfter || !pLockAfter->IsLocked())
            {
                World::Get().GetRunner().Trigger(
                    LockChangeEvent(apThis->formID, false, pLockAfter ? pLockAfter->lockLevel : 0));
            }
        }

        World::Get().GetRunner().Trigger(
            ActivateEvent(apThis, pActivator, apObjectToGet, aCount, aDefaultProcessing, aUnk1, openState)
        );
    }

    return result;
}

namespace
{
bool ShouldBlockRemoteActorInventoryHook(TESObjectREFR* apTarget, TESObjectREFR* apTransferPartner = nullptr) noexcept
{
    if (!apTarget)
        return true;

    // Never emit InventoryChangeEvent for faction evidence / prisoner belongings chests during PayFine / ServeSentence!
    if (apTarget->baseForm &&
        apTarget->baseForm->formType == FormType::Container &&
        ObjectService::IsFactionCrimeContainer(apTarget->formID))
    {
        return true;
    }

    Actor* pActor = Cast<Actor>(apTarget);
    if (!pActor)
        return false;

    const auto* pExt = pActor->GetExtension();
    if (!pExt)
        return false;

    // Never allow Papyrus scripts on a remote player puppet (e.g. RFAB_ExperienceRing::Break -> RemoveItem)
    // to emit InventoryChangeEvent and delete items from the real peer player's inventory!
    if (pExt->IsRemotePlayer())
        return true;

    // For remote NPCs (living OR dead), only allow interactive player transfers (ContainerMenu / QuickLoot where
    // apTransferPartner == PlayerCharacter::Get()), blocking autonomous non-owner OnDeath / OnDying / OnLoad
    // Papyrus AddItem/RemoveItem scripts from duplicating boss death-loot across clients!
    if (pExt->IsRemote())
    {
        if (apTransferPartner && apTransferPartner == PlayerCharacter::Get())
            return false;

        auto* pUi = UI::Get();
        if (!pUi || !pUi->GetMenuOpen(BSFixedString("ContainerMenu")))
            return true;
    }

    return false;
}

void MarkSharedCoopKeyOrClawAsQuestItem(TESObjectREFR* apOwner, TESForm* apItem, Inventory::Entry& arEntry) noexcept
{
    if (!apOwner || !apItem || arEntry.IsQuestItem || arEntry.Count <= 0)
        return;

    if (apOwner != PlayerCharacter::Get())
        return;

    const char* pEditorId = apItem->GetFormEditorID();
    const std::string_view editorIdView = pEditorId ? std::string_view(pEditorId) : std::string_view{};
    const bool syncHomes = World::Get().GetServerSettings().SyncPlayerHomes;
    if (RfabCompat::IsSharedCoopKeyOrClaw(editorIdView, static_cast<uint8_t>(apItem->formType), apItem->formID, syncHomes))
        arEntry.IsQuestItem = true;
}
} // namespace

void TP_MAKE_THISCALL(HookAddInventoryItem, TESObjectREFR, TESBoundObject* apItem, ExtraDataList* apExtraData, int32_t aCount, TESObjectREFR* apOldOwner)
{
    if (!ScopedInventoryOverride::IsOverriden() && apItem && !ShouldBlockRemoteActorInventoryHook(apThis, apOldOwner))
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apItem->formID, item.BaseId);
        item.Count = aCount;

        if (apExtraData)
            apThis->GetItemFromExtraData(item, apExtraData);

        MarkSharedCoopKeyOrClawAsQuestItem(apThis, apItem, item);

        World::Get().GetRunner().Trigger(InventoryChangeEvent(apThis->formID, std::move(item)));
    }

    spdlog::debug("Adding inventory item {:X} to {:X}", apItem ? apItem->formID : 0, apThis->formID);

    TiltedPhoques::ThisCall(RealAddInventoryItem, apThis, apItem, apExtraData, aCount, apOldOwner);
}

BSPointerHandle<TESObjectREFR>*
TP_MAKE_THISCALL(HookRemoveInventoryItem, TESObjectREFR, BSPointerHandle<TESObjectREFR>* apResult, TESBoundObject* apItem, int32_t aCount, ITEM_REMOVE_REASON aReason, ExtraDataList* apExtraList, TESObjectREFR* apMoveToRef, const NiPoint3* apDropLoc, const NiPoint3* apRotate)
{
    if (!ScopedInventoryOverride::IsOverriden() && apItem && !ShouldBlockRemoteActorInventoryHook(apThis, apMoveToRef))
    {
        auto& modSystem = World::Get().GetModSystem();

        Inventory::Entry item{};
        modSystem.GetServerModId(apItem->formID, item.BaseId);

        if (apExtraList)
        {
            ScopedExtraDataOverride _;
            apThis->GetItemFromExtraData(item, apExtraList);
        }

        item.Count = -aCount;

        World::Get().GetRunner().Trigger(InventoryChangeEvent(apThis->formID, std::move(item)));
    }

    spdlog::debug("Removing inventory item {:X} from {:X}", apItem ? apItem->formID : 0, apThis->formID);

    ScopedEquipOverride _;

    return TiltedPhoques::ThisCall(RealRemoveInventoryItem, apThis, apResult, apItem, aCount, aReason, apExtraList, apMoveToRef, apDropLoc, apRotate);
}

void TP_MAKE_THISCALL(HookRotateX, TESObjectREFR, float aAngle)
{
    if (!apThis)
        return;

    if (apThis->formType == Actor::Type)
    {
        const auto pActor = static_cast<Actor*>(apThis);
        // We don't allow remotes to move
        if (auto* pExt = pActor->GetExtension(); pExt && pExt->IsRemote())
            return;
    }

    return TiltedPhoques::ThisCall(RealRotateX, apThis, aAngle);
}

void TP_MAKE_THISCALL(HookRotateY, TESObjectREFR, float aAngle)
{
    if (!apThis)
        return;

    if (apThis->formType == Actor::Type)
    {
        const auto pActor = static_cast<Actor*>(apThis);
        // We don't allow remotes to move
        if (auto* pExt = pActor->GetExtension(); pExt && pExt->IsRemote())
            return;
    }

    return TiltedPhoques::ThisCall(RealRotateY, apThis, aAngle);
}

void TP_MAKE_THISCALL(HookRotateZ, TESObjectREFR, float aAngle)
{
    if (!apThis)
        return;

    if (apThis->formType == Actor::Type)
    {
        const auto pActor = static_cast<Actor*>(apThis);
        // We don't allow remotes to move
        if (auto* pExt = pActor->GetExtension(); pExt && pExt->IsRemote())
            return;
    }

    return TiltedPhoques::ThisCall(RealRotateZ, apThis, aAngle);
}

bool TP_MAKE_THISCALL(HookInternalMoveTo, TESObjectREFR, uint32_t* apTargetHandle, TESObjectCELL* apTargetCell, TESWorldSpace* apTargetWorldSpace, const NiPoint3& acPosition, const NiPoint3& acRotation)
{
    if (!apThis)
        return false;

    if (!ScopedReferencesOverride::IsOverriden())
    {
        if (apThis->formID == 0x14 &&
            CoopTeleportGuard::RemoteQuestMoveToGuard::ShouldBlockUnauthorizedPlayerMoveTo())
        {
            spdlog::warn(
                "[RFAB TeleportGuard] Blocked unauthorized remote-quest PlayerCharacter::MoveTo (targetCell={:X}, worldSpace={:X}, pos=({:.1f}, {:.1f}, {:.1f}))",
                apTargetCell ? apTargetCell->formID : 0u,
                apTargetWorldSpace ? apTargetWorldSpace->formID : 0u,
                acPosition.x,
                acPosition.y,
                acPosition.z);
            return false;
        }

        if (apThis->formType == Actor::Type)
        {
            auto* pActor = static_cast<Actor*>(apThis);
            if (auto* pExt = pActor->GetExtension(); pExt && pExt->IsRemotePlayer())
            {
                spdlog::debug(
                    "[RFAB TeleportGuard] Blocked unauthorized engine/script MoveTo on remote player {:X}",
                    apThis->formID);
                return false;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealInternalMoveTo, apThis, apTargetHandle, apTargetCell, apTargetWorldSpace, acPosition, acRotation);
}

void TP_MAKE_THISCALL(HookLockChange, TESObjectREFR)
{
    if (!apThis)
        return;

    TiltedPhoques::ThisCall(RealLockChange, apThis);
    const auto* pLock = apThis->GetLock();
    if(pLock)
        World::Get().GetRunner().Trigger(LockChangeEvent(apThis->formID, pLock->IsLocked(), pLock->lockLevel));
    else
        World::Get().GetRunner().Trigger(LockChangeEvent(apThis->formID, false, 0));
}

static TiltedPhoques::Initializer s_objectReferencesHooks(
    []()
    {
        POINTER_SKYRIMSE(TLockChange, s_lockChange, 19110);
        POINTER_SKYRIMSE(TRotate, s_rotateX, 19360);
        POINTER_SKYRIMSE(TRotate, s_rotateY, 19361);
        POINTER_SKYRIMSE(TRotate, s_rotateZ, 19362);
        POINTER_SKYRIMSE(TActivate, s_activate, 19369);
        POINTER_SKYRIMSE(TAddInventoryItem, s_addInventoryItem, 19282);
        POINTER_SKYRIMSE(TRemoveInventoryItem, s_removeInventoryItem, 19263);
        POINTER_SKYRIMSE(TPlayAnimationAndWait, s_playAnimationAndWait, 55675);
        POINTER_SKYRIMSE(TPlayAnimation, s_playAnimation, 55674);
        POINTER_SKYRIMSE(TInternalMoveTo, s_internalMoveTo, 56227);

        RealLockChange = s_lockChange.Get();
        RealRotateX = s_rotateX.Get();
        RealRotateY = s_rotateY.Get();
        RealRotateZ = s_rotateZ.Get();
        RealActivate = s_activate.Get();
        RealAddInventoryItem = s_addInventoryItem.Get();
        RealRemoveInventoryItem = s_removeInventoryItem.Get();
        RealPlayAnimationAndWait = s_playAnimationAndWait.Get();
        RealPlayAnimation = s_playAnimation.Get();
        RealInternalMoveTo = s_internalMoveTo.Get();

        TP_HOOK(&RealLockChange, HookLockChange);
        TP_HOOK(&RealRotateX, HookRotateX);
        TP_HOOK(&RealRotateY, HookRotateY);
        TP_HOOK(&RealRotateZ, HookRotateZ);
        TP_HOOK(&RealActivate, HookActivate);
        TP_HOOK(&RealAddInventoryItem, HookAddInventoryItem);
        TP_HOOK(&RealRemoveInventoryItem, HookRemoveInventoryItem);
        TP_HOOK(&RealPlayAnimationAndWait, HookPlayAnimationAndWait);
        TP_HOOK(&RealPlayAnimation, HookPlayAnimation);
        TP_HOOK(&RealInternalMoveTo, HookInternalMoveTo);
    });
