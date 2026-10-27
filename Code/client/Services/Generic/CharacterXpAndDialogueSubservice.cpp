#include <TiltedOnlinePCH.h>

#include <Services/CharacterService.h>

#include <Actor.h>
#include <Components.h>
#include <CoopAnimationState.h>
#include <Events/AddExperienceEvent.h>
#include <Events/DialogueEvent.h>
#include <Events/SubtitleEvent.h>
#include <Forms/TESGlobal.h>
#include <Forms/TESNPC.h>
#include <Games/ActorExtension.h>
#include <Games/Misc/SubtitleManager.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/TES.h>
#include <Messages/DialogueRequest.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/NotifySyncRfabExperience.h>
#include <Messages/SubtitleRequest.h>
#include <Messages/SyncExperienceRequest.h>
#include <Messages/SyncRfabExperienceRequest.h>
#include <PlayerCharacter.h>
#include <Services/PartyService.h>
#include <Services/RfabAdapterService.h>
#include <Services/TransportService.h>
#include <Utils.h>
#include <World.h>

namespace
{
TESGlobal* ResolveRfabXpGlobal() noexcept
{
    return World::Get().GetRfabAdapterService().GetRfabXpGlobal();
}

int32_t ReadRfabXpWallet(TESGlobal* apGlobal) noexcept
{
    return RfabAdapterService::ReadRfabXp(apGlobal);
}

bool ApplyRfabExperienceDelta(int32_t aDelta, int32_t& aNewExperience) noexcept
{
    return World::Get().GetRfabAdapterService().ApplyDualPoolExperienceDelta(aDelta, aNewExperience);
}
} // namespace

void CharacterService::NotifyRemoteQuestStageApplied() noexcept
{
    m_rfabXpTracker.OnRemoteQuestStageApplied(NowSteadyWorldViewMs());
}

void CharacterService::OnAddExperienceEvent(const AddExperienceEvent& acEvent) noexcept
{
    // When drake1ch/RFAB is loaded (RFAB_XP_Amount global present), progression is governed strictly by
    // RFAB's XP pool rather than vanilla skill-use ticks — never leak spurious skill XP packets across party!
    if (ResolveRfabXpGlobal() != nullptr)
    {
        m_cachedExperience = 0.f;
        return;
    }

    m_cachedExperience += acEvent.Experience;
}

void CharacterService::OnNotifySyncExperience(const NotifySyncExperience& acMessage) noexcept
{
    if (ResolveRfabXpGlobal() != nullptr)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || PlayerCharacter::LastUsedCombatSkill == -1)
        return;

    pPlayer->AddSkillExperience(PlayerCharacter::LastUsedCombatSkill, acMessage.Experience);
}

void CharacterService::OnNotifySyncRfabExperience(const NotifySyncRfabExperience& acMessage) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESGlobal* pRfabXp = ResolveRfabXpGlobal();
    if (pPlayer == nullptr || pRfabXp == nullptr)
        return;

    const uint64_t nowMs = NowSteadyWorldViewMs();
    const float localMultiplier = m_world.GetRfabAdapterService().GetLocalXpMultiplier();
    const int32_t delta = m_rfabXpTracker.EvaluateIncomingRemoteXp(acMessage.Experience, localMultiplier, nowMs);
    if (delta <= 0)
        return;

    const int32_t currentExperience = ReadRfabXpWallet(pRfabXp);
    int32_t newExperience = currentExperience;
    if (!ApplyRfabExperienceDelta(delta, newExperience))
    {
        spdlog::warn("Unable to apply RFAB XP: dual-pool XP global/player unavailable");
        return;
    }

    m_rfabXpTracker.CommitAppliedRemoteXp(currentExperience, delta, nowMs);
    spdlog::debug("Received RFAB experience {} (total {})", delta, newExperience);
}

void CharacterService::ApplyAuthoritativeRfabXpDelta(int32_t aDelta) noexcept
{
    if (aDelta == 0)
        return;

    TESGlobal* pRfabXp = ResolveRfabXpGlobal();
    if (pRfabXp == nullptr)
        return;

    int32_t newExperience = ReadRfabXpWallet(pRfabXp);
    if (!ApplyRfabExperienceDelta(aDelta, newExperience))
        return;

    m_rfabXpTracker.CommitAuthoritativeDelta(newExperience, aDelta, NowSteadyWorldViewMs());
}

void CharacterService::RunExperienceUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    if (m_cachedExperience == 0.f || !World::Get().GetPartyService().IsInParty())
        return;

    SyncExperienceRequest message;
    message.Experience = m_cachedExperience;
    m_cachedExperience = 0.f;

    m_transport.Send(message);
    spdlog::debug("Sending over experience {}", message.Experience);
}

void CharacterService::RunRfabExperienceUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    if (!m_transport.IsConnected())
    {
        (void)m_rfabXpTracker.EvaluateLocalWalletPoll(0, false, false, 1.0f, NowSteadyWorldViewMs());
        return;
    }

    TESGlobal* pRfabXp = ResolveRfabXpGlobal();
    if (pRfabXp == nullptr)
        return;

    const int32_t currentExperience = ReadRfabXpWallet(pRfabXp);
    bool isCustomSkillMenuOpen = false;
    if (auto* pUi = UI::Get(); pUi && pUi->GetMenuOpen(BSFixedString("CustomMenu")))
        isCustomSkillMenuOpen = true;

    const float senderMultiplier = m_world.GetRfabAdapterService().GetLocalXpMultiplier();
    const auto pollDecision = m_rfabXpTracker.EvaluateLocalWalletPoll(
        currentExperience,
        true,
        isCustomSkillMenuOpen,
        senderMultiplier,
        NowSteadyWorldViewMs());

    if (pollDecision.RefundToApply > 0)
    {
        int32_t adjustedExperience = currentExperience;
        if (ApplyRfabExperienceDelta(-pollDecision.RefundToApply, adjustedExperience))
            m_rfabXpTracker.CommitLocalRefundApplied(pollDecision.RefundToApply, adjustedExperience);
    }

    if (pollDecision.BroadcastBaseXp > 0)
    {
        SyncRfabExperienceRequest message;
        message.Experience = pollDecision.BroadcastBaseXp;
        m_transport.Send(message);
        spdlog::debug("Sending over RFAB experience {}", message.Experience);
    }
}

void CharacterService::OnDialogueEvent(const DialogueEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    const bool isLeader = World::Get().GetPartyService().IsLeader();
    const entt::entity speakerEntity = FindActorEntityByFormId(acEvent.ActorID);
    if (speakerEntity == entt::null)
    {
        spdlog::debug(__FUNCTION__ ": failed to find speaking Actor's FormIdComponent, formId {:X}, isLeader {}", acEvent.ActorID, isLeader);
        return;
    }

    auto serverIdRes = Utils::GetServerId(speakerEntity);
    if (!serverIdRes)
    {
        spdlog::debug(__FUNCTION__ ": server id not found for formId {:X}, isLeader {}", acEvent.ActorID, isLeader);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorID));
    if (!pActor || pActor->IsDisabled() || pActor->IsDeleted() || !pActor->GetNiNode() || !pActor->GetExtension())
        return;

    const bool isLocal = pActor->GetExtension()->IsLocal();
    const bool isInScene = pActor->IsInScene();
    const auto sceneId = (isInScene && pActor->GetCurrentScene()) ? pActor->GetCurrentScene()->formID : 0;
    const bool isTaskDialogue = pActor->IsTalking() && pActor->IsInDialogueWithPlayer();
    const bool isSpeakingInScene = pActor->IsSpeakingInScene();
    const bool willSync = isTaskDialogue || (isLocal && !isInScene);

    spdlog::debug(
        __FUNCTION__ ": isLocal {}, isInScene {}, isSpeakingInScene {}, isTaskDialogue {}, willSync {}, scene {:X}, Actor "
                     "{:X}, serverId {:X}, isLeader {}, name {}, soundFile {}",
        isLocal,
        isInScene,
        isSpeakingInScene,
        isTaskDialogue,
        willSync,
        sceneId,
        pActor->formID,
        serverIdRes.value(),
        isLeader,
        pActor->baseForm ? pActor->baseForm->GetName() : "<null>",
        acEvent.VoiceFile);

    if (willSync)
    {
        DialogueRequest request{};
        request.ServerId = serverIdRes.value();
        request.SoundFilename = acEvent.VoiceFile;
        m_transport.Send(request);
    }
}

void CharacterService::OnNotifyDialogue(const NotifyDialogue& acMessage) noexcept
{
    const bool isLeader = World::Get().GetPartyService().IsLeader();
    const entt::entity speakerEntity = FindEntityByServerId(acMessage.ServerId);
    if (speakerEntity == entt::null || !m_world.all_of<FormIdComponent>(speakerEntity) || m_world.all_of<ObjectComponent>(speakerEntity))
    {
        spdlog::debug(__FUNCTION__ ": failed to find speaking Actor's FormIdComponent, serverId {:X}, isLeader {}", acMessage.ServerId, isLeader);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(m_world.get<FormIdComponent>(speakerEntity).Id));
    if (!pActor || pActor->IsDisabled() || pActor->IsDeleted() || !pActor->GetNiNode())
        return;

    if (TESNPC* pBaseNpc = Cast<TESNPC>(pActor->baseForm))
    {
        const bool actorIsFemale = ((pBaseNpc->actorData.flags & 1u) != 0u);
        if (CoopAnimationState::HasVoicePathGenderMismatch(acMessage.SoundFilename, actorIsFemale))
        {
            if (const auto modelIt = m_remoteModels.find(acMessage.ServerId);
                modelIt != m_remoteModels.end() && modelIt->second.SpawnRequest.BaseId != GameId{})
            {
                (void)ReconcilePlacedActorBaseFormInPlace(pActor, modelIt->second.SpawnRequest.BaseId);
            }

            TESNPC* pUpdatedNpc = Cast<TESNPC>(pActor->baseForm);
            const bool updatedIsFemale = pUpdatedNpc ? ((pUpdatedNpc->actorData.flags & 1u) != 0u) : actorIsFemale;
            if (CoopAnimationState::HasVoicePathGenderMismatch(acMessage.SoundFilename, updatedIsFemale))
            {
                spdlog::debug(
                    "[RFAB VoiceGuard] Suppressed gender-mismatched voice clip '{}' on actor {:X} (isFemale={})",
                    acMessage.SoundFilename,
                    pActor->formID,
                    updatedIsFemale);
                return;
            }
        }
    }

    spdlog::debug(
        __FUNCTION__ ": playing dialogue Actor {:X}, serverId {:X}, isLeader {}, name {}, soundFile {}",
        pActor->formID,
        acMessage.ServerId,
        isLeader,
        pActor->baseForm ? pActor->baseForm->GetName() : "<null>",
        acMessage.SoundFilename);

    pActor->StopCurrentDialogue(true);
    pActor->SpeakSound(acMessage.SoundFilename.c_str());
}

void CharacterService::OnSubtitleEvent(const SubtitleEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    const bool isLeader = World::Get().GetPartyService().IsLeader();
    const entt::entity speakerEntity = FindActorEntityByFormId(acEvent.SpeakerID);
    if (speakerEntity == entt::null)
    {
        spdlog::debug(__FUNCTION__ ": failed to find subtitle Actor's FormIdComponent, formId {:X}, isLeader {}", acEvent.SpeakerID, isLeader);
        return;
    }

    auto serverIdRes = Utils::GetServerId(speakerEntity);
    if (!serverIdRes)
    {
        spdlog::debug(__FUNCTION__ ": server id not found for formId {:X}, isLeader {}", acEvent.SpeakerID, isLeader);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.SpeakerID));
    if (!pActor || pActor->IsDisabled() || pActor->IsDeleted() || !pActor->GetNiNode() || !pActor->GetExtension())
        return;

    const bool isLocal = pActor->GetExtension()->IsLocal();
    const bool isInScene = pActor->IsInScene();
    const auto isSpeakingInScene = pActor->IsSpeakingInScene();
    const auto sceneId = (isInScene && pActor->GetCurrentScene()) ? pActor->GetCurrentScene()->formID : 0;
    const bool isTaskDialogue = pActor->IsTalking() && pActor->IsInDialogueWithPlayer();
    const bool willSync = isTaskDialogue || (isLocal && !isInScene);

    spdlog::debug(
        __FUNCTION__ ": isLocal {}, isInScene {}, isSpeakingInScene {}, isTaskDialogue {}, willSync {}, scene {:X}, Actor "
                     "{:X}, serverId {:X}, isLeader {}, name {}, subtitle {}",
        isLocal,
        isInScene,
        isSpeakingInScene,
        isTaskDialogue,
        willSync,
        sceneId,
        pActor->formID,
        serverIdRes.value(),
        isLeader,
        pActor->baseForm ? pActor->baseForm->GetName() : "<null>",
        acEvent.Text);

    if (willSync)
    {
        SubtitleRequest request{};
        request.ServerId = serverIdRes.value();
        request.Text = acEvent.Text;
        request.TopicFormId = acEvent.TopicFormID;
        m_transport.Send(request);
    }
}

void CharacterService::OnNotifySubtitle(const NotifySubtitle& acMessage) noexcept
{
    const bool isLeader = m_world.Get().GetPartyService().IsLeader();
    const entt::entity speakerEntity = FindEntityByServerId(acMessage.ServerId);
    if (speakerEntity == entt::null || !m_world.all_of<FormIdComponent>(speakerEntity) || m_world.all_of<ObjectComponent>(speakerEntity))
    {
        spdlog::debug(__FUNCTION__ ": failed to find subtitle Actor's FormIdComponent, serverId {:X}, isLeader {}", acMessage.ServerId, isLeader);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(m_world.get<FormIdComponent>(speakerEntity).Id));
    if (!pActor)
        return;

    TESTopicInfo* pInfo = Cast<TESTopicInfo>(TESForm::GetById(acMessage.TopicFormId));

    spdlog::debug(
        __FUNCTION__ ": showing subtitle Actor {:X}, serverId {:X}, isLeader {}, name {}, message: {}",
        pActor->formID,
        acMessage.ServerId,
        isLeader,
        pActor->baseForm ? pActor->baseForm->GetName() : "<null>",
        acMessage.Text);

    SubtitleManager::Get()->HideSubtitle(pActor);
    SubtitleManager::Get()->ShowSubtitle(pActor, acMessage.Text.c_str(), pInfo);
}
