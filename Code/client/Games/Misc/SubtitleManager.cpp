#include "SubtitleManager.h"

#include <Events/SubtitleEvent.h>

#include <TESObjectREFR.h>
#include <Games/ActorExtension.h>
#include <Forms/TESQuest.h>

#include <Forms/TESTopicInfo.h>
#include <Misc/BSFixedString.h>
#include <common/CoopWorldStabilizer.h>

SubtitleManager* SubtitleManager::Get() noexcept
{
    POINTER_SKYRIMSE(SubtitleManager*, s_singleton, 514283);
    return *s_singleton.Get();
}

TP_THIS_FUNCTION(TShowSubtitle, void, SubtitleManager, TESObjectREFR* apSpeaker, const char* apSubtitleText, bool aIsInDialogue);
static TShowSubtitle* RealShowSubtitle = nullptr;

void SubtitleManager::ShowSubtitle(TESObjectREFR* apSpeaker, const char* apSubtitleText, TESTopicInfo* apTopicInfo, bool aUnk1) noexcept
{
    TiltedPhoques::ThisCall(RealShowSubtitle, this, apSpeaker, apSubtitleText, aUnk1);
}

void* SubtitleManager::HideSubtitle(TESObjectREFR* apSpeaker) noexcept
{
    TP_THIS_FUNCTION(THideSubtitle, void*, SubtitleManager, TESObjectREFR* apSpeaker);
    POINTER_SKYRIMSE(THideSubtitle, s_hideSubtitle, 51754);
    return TiltedPhoques::ThisCall(s_hideSubtitle, this, apSpeaker);
}

void TP_MAKE_THISCALL(HookShowSubtitle, SubtitleManager, TESObjectREFR* apSpeaker, const char* apSubtitleText, bool aIsInDialogue)
{
    const bool worldReady = World::IsCreated() && World::IsConnectedCached();
    Actor* pActor = (worldReady && apSpeaker) ? Cast<Actor>(apSpeaker) : nullptr;
    const ActorExtension* pExt = (pActor && pActor->formID != 0) ? pActor->GetExtension() : nullptr;
    if (apSubtitleText && apSubtitleText[0] != '\0' &&
        CoopWorldStabilizer::ShouldTriggerSubtitleSync(
            worldReady,
            pExt != nullptr,
            pExt && pExt->IsRemote(),
            pExt && pExt->IsPlayer()))
    {
        String safeSubtitle(apSubtitleText);
        World::Get().GetRunner().Trigger(SubtitleEvent(pActor->formID, std::move(safeSubtitle)));
    }

    TiltedPhoques::ThisCall(RealShowSubtitle, apThis, apSpeaker, apSubtitleText, aIsInDialogue);
}

static TiltedPhoques::Initializer s_subtitleHooks(
    []()
    {
        POINTER_SKYRIMSE(TShowSubtitle, s_showSubtitle, 51753);

        RealShowSubtitle = s_showSubtitle.Get();

        TP_HOOK(&RealShowSubtitle, HookShowSubtitle);
    });
