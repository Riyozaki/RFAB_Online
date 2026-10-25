#include <TiltedOnlinePCH.h>

#include <Games/References.h>

#include <RTTI.h>

#include <Forms/BGSHeadPart.h>
#include <Forms/TESNPC.h>
#include <Forms/TESPackage.h>
#include <Forms/TESWeather.h>
#include <SaveLoad.h>
#include <ExtraData/ExtraDataList.h>

#include <Misc/GameVM.h>

#include <Games/TES.h>
#include <Games/Overrides.h>
#include <Games/Misc/Lock.h>
#include <AI/AIProcess.h>
#include <Magic/MagicCaster.h>
#include <Sky/Sky.h>

#include <Events/InitPackageEvent.h>

#include <TiltedCore/Serialization.hpp>

#include <Services/PapyrusService.h>
#include <Services/DebugService.h>
#include <Services/RfabAdapterService.h>
#include <World.h>

#include <AI/AITimer.h>
#include <Combat/CombatTargetSelector.h>
#include <RfabModpackCompat.h>

namespace Settings
{
int32_t* GetDifficulty() noexcept
{
    POINTER_SKYRIMSE(int32_t, s_difficulty, 509231);
    return s_difficulty.Get();
}

float* GetGreetDistance() noexcept
{
    POINTER_SKYRIMSE(float, s_greetDistance, 503019);
    return s_greetDistance.Get();
}

bool IsRfabModpackActive() noexcept
{
    if (auto* pModManager = ModManager::Get())
    {
        if (pModManager->GetByName("RFAB.esp") != nullptr ||
            pModManager->GetByName("Requiem for a Balance.esp") != nullptr ||
            pModManager->GetByName("Requiem.esp") != nullptr)
        {
            return true;
        }
    }

    return World::Get().GetRfabAdapterService().ResolveRfabPluginMod() != nullptr;
}

float GetRfabOutgoingDamageMult() noexcept
{
    if (auto* pGmst = GameSettingCollection::Get())
    {
        if (auto* pSetting = pGmst->GetSetting("fDiffMultHPByPCVH"))
            return RfabCompat::SanitizeDifficultyMultiplier(pSetting->f);
    }

    return 1.0f;
}

float GetRfabIncomingDamageMult() noexcept
{
    if (auto* pGmst = GameSettingCollection::Get())
    {
        if (auto* pSetting = pGmst->GetSetting("fDiffMultHPToPCVH"))
            return RfabCompat::SanitizeDifficultyMultiplier(pSetting->f);
    }

    return 1.0f;
}

} // namespace Settings

namespace GameplayFormulas
{

float CalculateRealDamage(Actor* apHittee, float aDamage, bool aKillMove) noexcept
{
    bool isPlayer = apHittee == PlayerCharacter::Get();
    float multiplier = 1.0f;

    if (Settings::IsRfabModpackActive())
    {
        // RFAB.dll evaluates GMST fDiffMultHPToPCVH (player 0x14 victim) and fDiffMultHPByPCVH (player 0x14 aggressor),
        // corresponding to Странник (x2/x0.5), Довакин (x1/x1), Легенда (x1/x2), and Проклятый (x0.5 outgoing).
        multiplier = isPlayer ? Settings::GetRfabIncomingDamageMult() : Settings::GetRfabOutgoingDamageMult();
    }
    else
    {
        using TGetDifficultyMultiplier = float(int32_t, int32_t, bool);
        POINTER_SKYRIMSE(TGetDifficultyMultiplier, s_getDifficultyMultiplier, 25920);
        multiplier = s_getDifficultyMultiplier(PlayerCharacter::Get()->difficulty, ActorValueInfo::kHealth, isPlayer);
    }

    float realDamage = aDamage * multiplier;

    return realDamage;
}

} // namespace GameplayFormulas

void FadeOutGame(bool aFadingOut, bool aBlackFade, float aFadeDuration, bool aRemainVisible, float aSecondsToFade) noexcept
{
    using TFadeOutGame = void(bool, bool, float, bool, float);
    POINTER_SKYRIMSE(TFadeOutGame, fadeOutGame, 51909);
    fadeOutGame.Get()(aFadingOut, aBlackFade, aFadeDuration, aRemainVisible, aSecondsToFade);
}

// Disable AI sync for now, experiment didn't work, code might be useful later on though.
#define AI_SYNC 0

TP_THIS_FUNCTION(TCheckForNewPackage, bool, void, Actor* apActor, uint64_t aUnk1);
static TCheckForNewPackage* RealCheckForNewPackage = nullptr;

bool TP_MAKE_THISCALL(HookCheckForNewPackage, void, Actor* apActor, uint64_t aUnk1)
{
#if AI_SYNC
    if (apActor && apActor->GetExtension()->IsRemote())
        return false;

#endif
    return TiltedPhoques::ThisCall(RealCheckForNewPackage, apThis, apActor, aUnk1);
}

TP_THIS_FUNCTION(TInitFromPackage, void, void, TESPackage* apPackage, TESObjectREFR* apTarget, Actor* arActor);
static TInitFromPackage* RealInitFromPackage = nullptr;

void TP_MAKE_THISCALL(HookInitFromPackage, void, TESPackage* apPackage, TESObjectREFR* apTarget, Actor* arActor)
{
#if AI_SYNC
    // This guard is here for when the client sets the package based on a remote message
    if (s_execInitPackage)
        return TiltedPhoques::ThisCall(RealInitFromPackage, apThis, apPackage, apTarget, arActor);

    if (arActor && arActor->GetExtension()->IsRemote())
        return;

    if (arActor && apPackage)
        World::Get().GetRunner().Trigger(InitPackageEvent(arActor->formID, apPackage->formID));

#endif
    return TiltedPhoques::ThisCall(RealInitFromPackage, apThis, apPackage, apTarget, arActor);
}

TP_THIS_FUNCTION(TSetCurrentPickREFR, void, Console, BSPointerHandle<TESObjectREFR>* apRefr);
static TSetCurrentPickREFR* RealSetCurrentPickREFR = nullptr;

void TP_MAKE_THISCALL(HookSetCurrentPickREFR, Console, BSPointerHandle<TESObjectREFR>* apRefr)
{
    uint32_t formId = 0;

    TESObjectREFR* pObject = TESObjectREFR::GetByHandle(apRefr->handle.iBits);
    if (pObject)
        formId = pObject->formID;

    World::Get().GetDebugService().SetDebugId(formId);

    return TiltedPhoques::ThisCall(RealSetCurrentPickREFR, apThis, apRefr);
}

static TiltedPhoques::Initializer s_referencesHooks(
    []()
    {
        POINTER_SKYRIMSE(TCheckForNewPackage, s_checkForNewPackage, 38156);
        POINTER_SKYRIMSE(TInitFromPackage, s_initFromPackage, 38005);
        POINTER_SKYRIMSE(TSetCurrentPickREFR, s_setCurrentPickREFR, 50164);

        RealCheckForNewPackage = s_checkForNewPackage.Get();
        RealInitFromPackage = s_initFromPackage.Get();
        RealSetCurrentPickREFR = s_setCurrentPickREFR.Get();

        TP_HOOK(&RealCheckForNewPackage, HookCheckForNewPackage);
        TP_HOOK(&RealInitFromPackage, HookInitFromPackage);
        TP_HOOK(&RealSetCurrentPickREFR, HookSetCurrentPickREFR);
    });
