#pragma once

#include <Forms/TESBoundAnimObject.h>

#include <Components/BGSDestructibleObjectForm.h>
#include <Components/BGSSkinForm.h>
#include <Components/BGSKeywordForm.h>
#include <Components/BGSAttackDataForm.h>
#include <Components/BGSPerkRankArray.h>
#include <Components/TESActorBaseData.h>
#include <Components/TESContainer.h>
#include <Components/TESSpellList.h>
#include <Components/TESAIForm.h>
#include <Components/TESFullName.h>
#include <Misc/ActorValueOwner.h>

struct TESActorBase : TESBoundAnimObject
{
    TESActorBaseData actorData;
    TESContainer container;
    TESSpellList spellList;
    TESAIForm aiForm;
    TESFullName fullName;
    ActorValueOwner actorValueOwner;
    BGSDestructibleObjectForm destructibleObjectForm;
    BGSSkinForm skinForm;
    BGSKeywordForm keywordForm;
    BGSAttackDataForm attackDataForm;
    BGSPerkRankArray perkRanks;

    bool IsGhostBase() const noexcept { return actorData.IsGhostBase(); }
    bool IsInvulnerableBase() const noexcept { return actorData.IsInvulnerableBase(); }
};

#ifdef _MSC_VER
static_assert(offsetof(TESActorBase, spellList) == 0xA0);
#else
static_assert(offsetof(TESActorBase, spellList) == 0x98);
#endif
