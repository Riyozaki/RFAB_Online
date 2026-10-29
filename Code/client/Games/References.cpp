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

#include <atomic>

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
    // Lock-free O(1) atomic cache safe for concurrent combat & magic worker threads (`HookDamageActor`,
    // `HookAddTarget`): once `ModManager` has loaded plugins, scan the immutable plugin list once without
    // touching non-thread-safe `RfabFormResolver` state on worker threads.
    static std::atomic<int8_t> s_cachedRfabActive{-1};
    const int8_t cached = s_cachedRfabActive.load(std::memory_order_acquire);
    if (cached >= 0)
        return cached == 1;

#ifdef _WIN32
    if (GetModuleHandleW(L"RFAB.dll") != nullptr)
    {
        s_cachedRfabActive.store(1, std::memory_order_release);
        return true;
    }
#endif

    auto* const pModManager = ModManager::Get();
    if (!pModManager)
        return false;

    if (pModManager->GetByName("RFAB.esp") != nullptr ||
        pModManager->GetByName("Requiem for a Balance.esp") != nullptr ||
        pModManager->GetByName("Requiem.esp") != nullptr)
    {
        s_cachedRfabActive.store(1, std::memory_order_release);
        return true;
    }

    bool hasAnyLoadedMod = false;
    bool detectedRfab = false;
    for (auto* pMod : pModManager->mods)
    {
        if (!pMod || !pMod->IsLoaded() || pMod->filename[0] == '\0')
            continue;

        hasAnyLoadedMod = true;
        if (RfabCompat::Detail::StartsWithIgnoreCase(pMod->filename, "rfab") ||
            RfabCompat::Detail::StartsWithIgnoreCase(pMod->filename, "requiem"))
        {
            detectedRfab = true;
            break;
        }
    }

    if (hasAnyLoadedMod)
        s_cachedRfabActive.store(detectedRfab ? 1 : 0, std::memory_order_release);

    return detectedRfab;
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
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    bool isPlayer = pPlayer && apHittee == pPlayer;
    float multiplier = 1.0f;

    if (Settings::IsRfabModpackActive())
    {
        // RFAB.dll evaluates GMST fDiffMultHPToPCVH (player 0x14 victim) and fDiffMultHPByPCVH (player 0x14 aggressor),
        // corresponding to Странник (x2/x0.5), Довакин (x1/x1), Легенда (x1/x2), and Проклятый (x0.5 outgoing).
        multiplier = isPlayer ? Settings::GetRfabIncomingDamageMult() : Settings::GetRfabOutgoingDamageMult();
    }
    else if (pPlayer)
    {
        using TGetDifficultyMultiplier = float(int32_t, int32_t, bool);
        POINTER_SKYRIMSE(TGetDifficultyMultiplier, s_getDifficultyMultiplier, 25920);
        multiplier = s_getDifficultyMultiplier(pPlayer->difficulty, ActorValueInfo::kHealth, isPlayer);
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

    if (arActor && arActor->GetExtension() && arActor->GetExtension()->IsRemote())
        return;

    if (World::IsConnectedCached() && arActor && apPackage)
        World::Get().GetRunner().Trigger(InitPackageEvent(arActor->formID, apPackage->formID));

#endif
    return TiltedPhoques::ThisCall(RealInitFromPackage, apThis, apPackage, apTarget, arActor);
}

TP_THIS_FUNCTION(TSetCurrentPickREFR, void, Console, BSPointerHandle<TESObjectREFR>* apRefr);
static TSetCurrentPickREFR* RealSetCurrentPickREFR = nullptr;

void TP_MAKE_THISCALL(HookSetCurrentPickREFR, Console, BSPointerHandle<TESObjectREFR>* apRefr)
{
    uint32_t formId = 0;

    if (apRefr)
    {
        if (GamePtr<TESObjectREFR> spObject = TESObjectREFR::LookupByHandle(apRefr->handle.iBits))
            formId = spObject->formID;
    }

    if (World::IsCreated())
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
