#include <TiltedOnlinePCH.h>

#include <Events/ConnectedEvent.h>

#include <Services/QuestService.h>
#include <Services/ImguiService.h>

#include <PlayerCharacter.h>
#include <Forms/TESQuest.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <AI/Movement/PlayerControls.h>

#include <Events/EventDispatcher.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/RequestQuestSceneUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/NotifyQuestSceneUpdate.h>
#include <common/RfabModpackCompat.h>
#include <common/CoopTeleportGuard.h>

namespace spdfmt = spdlog::fmt_lib;

namespace
{
thread_local bool s_inRemoteQuestUpdate = false;

struct ScopedRemoteQuestUpdateGuard
{
    ScopedRemoteQuestUpdateGuard() noexcept
        : m_prev(s_inRemoteQuestUpdate)
    {
        s_inRemoteQuestUpdate = true;
    }
    ~ScopedRemoteQuestUpdateGuard() noexcept { s_inRemoteQuestUpdate = m_prev; }
    bool m_prev{false};
    CoopTeleportGuard::ScopedRemoteQuestMoveToSuppression m_moveToSuppression{
        CoopTeleportGuard::NowSteadyNs(),
        CoopTeleportGuard::RemoteQuestMoveToGuard::kExtendedLateSessionPapyrusHoldNs};
};

const char* ResolveQuestEditorId(TESQuest* apQuest) noexcept
{
    if (!apQuest)
        return "";
    if (const char* idStr = apQuest->idName.AsAscii(); idStr && idStr[0] != '\0')
        return idStr;
    if (const char* edid = apQuest->GetFormEditorID(); edid && edid[0] != '\0')
        return edid;
    return "";
}

uint8_t NormalizeNetworkQuestType(TESQuest* apQuest) noexcept
{
    if (!apQuest)
        return 0;
    const auto rawType = static_cast<uint8_t>(apQuest->type);
    if (rawType == static_cast<uint8_t>(TESQuest::Type::None) &&
        RfabCompat::IsDungeonOrStoryQuestEditorId(ResolveQuestEditorId(apQuest)))
    {
        return static_cast<uint8_t>(TESQuest::Type::SideQuest);
    }
    return rawType;
}
} // namespace

static TESQuest* FindQuestByNameId(const String& name)
{
    auto* pModManager = ModManager::Get();
    if (!pModManager)
        return nullptr;

    auto& questRegistry = pModManager->quests;
    auto it = std::find_if(questRegistry.begin(), questRegistry.end(), [&name](auto* pQuest) {
        return pQuest && pQuest->idName.AsAscii() && std::strcmp(pQuest->idName.AsAscii(), name.c_str()) == 0;
    });

    return it != questRegistry.end() ? *it : nullptr;
}

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_joinedConnection = aDispatcher.sink<ConnectedEvent>().connect<&QuestService::OnConnected>(this);
    m_questUpdateConnection = aDispatcher.sink<NotifyQuestUpdate>().connect<&QuestService::OnQuestUpdate>(this);
    m_questSceneUpdateConnection = aDispatcher.sink<NotifyQuestSceneUpdate>().connect<&QuestService::OnQuestSceneUpdate>(this);
    m_playerId = 0;

    // A note about the Gameevents:
    // TESQuestStageItemDoneEvent gets fired too late, we instead use TESQuestStageEvent, because it responds immediately.
    // TESQuestInitEvent can be instead managed by start stop quest management.
    // bind game event listeners
    auto* pEventList = EventDispatcherManager::Get();
    pEventList->questStartStopEvent.RegisterSink(this);
    pEventList->questStageEvent.RegisterSink(this);

    pEventList->scenePhaseEvent.RegisterSink(this);
    pEventList->sceneActionEvent.RegisterSink(this);
    pEventList->sceneEvent.RegisterSink(this);
}

void QuestService::OnConnected(const ConnectedEvent& apEvent) noexcept
{
    // TODO: this should be followed with whatever the quest leader selected
    /*
    // deselect any active quests
    auto* pPlayer = PlayerCharacter::Get();
    for (auto& objective : pPlayer->objectives)
    {
        if (auto* pQuest = objective.instance->quest)
            pQuest->SetActive(false);
    }
    */
    m_playerId = apEvent.PlayerId;
}

BSTEventResult QuestService::OnEvent(const TESQuestStartStopEvent* apEvent,
                                     const EventDispatcher<TESQuestStartStopEvent>*)
{
    if (s_inRemoteQuestUpdate || !apEvent || !World::IsCreated() || !World::IsConnectedCached() || ScopedSaveLoadOverride::IsOverriden())
        return BSTEventResult::kOk;

    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId));
    if (pQuest == nullptr || IsNonSyncableQuest(pQuest))
        return BSTEventResult::kOk;

    const uint32_t formId = pQuest->formID;
    const uint16_t stageId = pQuest->currentStage;
    const bool stopped = pQuest->IsStopped();
    const uint8_t netQuestType = NormalizeNetworkQuestType(pQuest);

    m_world.GetRunner().Queue(
        [this, formId, stageId, stopped, netQuestType]()
        {
            if (!m_world.GetPartyService().IsInParty())
                return;

            GameId Id{};
            if (!m_world.GetModSystem().GetServerModId(formId, Id))
                return;

            TESQuest* pLiveQuest = Cast<TESQuest>(TESForm::GetById(formId));
            const char* qName = (pLiveQuest && pLiveQuest->fullName.value.AsAscii()) ? pLiveQuest->fullName.value.AsAscii() : "";
            const bool isLeader = m_world.GetPartyService().IsLeader();
            spdlog::info(
                "QuestService::OnEvent: {} quest formId: {:X}, questStage: {}, netQuestType: {}, {} {}, name: {}",
                stopped ? "stopped" : "started",
                formId,
                stageId,
                netQuestType,
                isLeader ? "leader" : "player",
                PlayerId(),
                qName);

            RequestQuestUpdate update;
            update.Id = Id;
            update.Stage = stageId;
            update.Status = stopped ? RequestQuestUpdate::Stopped : RequestQuestUpdate::Started;
            update.ClientQuestType = netQuestType;
            m_world.GetTransport().Send(update);
        });

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESQuestStageEvent* apEvent, const EventDispatcher<TESQuestStageEvent>*)
{
    if (s_inRemoteQuestUpdate || !apEvent || !World::IsCreated() || !World::IsConnectedCached() || ScopedSaveLoadOverride::IsOverriden())
        return BSTEventResult::kOk;

    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId));
    if (pQuest == nullptr || IsNonSyncableQuest(pQuest))
        return BSTEventResult::kOk;

    // Only treat stage 0 as an explicit quest reset if the quest already had prior executed stages;
    // Skyrim & RFAB frequently initialize startup stage 0 on controller/dialogue quests with normal StageUpdate semantics.
    const bool hadExecutedStages = pQuest->pExecutedStages && !pQuest->pExecutedStages->Empty();
    const bool isResetUpdate = apEvent->stageId == 0 &&
        hadExecutedStages &&
        pQuest->currentStage > 0 &&
        (pQuest->flags & (TESQuest::Flags::Enabled | TESQuest::Flags::StageWait)) != (TESQuest::Flags::Enabled | TESQuest::Flags::StageWait);

    const uint32_t formId = apEvent->formId;
    const uint16_t stageId = apEvent->stageId;
    const uint8_t netQuestType = NormalizeNetworkQuestType(pQuest);

    m_world.GetRunner().Queue(
        [this, formId, stageId, isReset = isResetUpdate, netQuestType]()
        {
            if (!m_world.GetPartyService().IsInParty())
                return;

            GameId Id{};
            if (!m_world.GetModSystem().GetServerModId(formId, Id))
                return;

            TESQuest* pLiveQuest = Cast<TESQuest>(TESForm::GetById(formId));
            const char* qName = (pLiveQuest && pLiveQuest->fullName.value.AsAscii()) ? pLiveQuest->fullName.value.AsAscii() : "";
            const bool isLeader = m_world.GetPartyService().IsLeader();
            if (isReset)
            {
                spdlog::debug(
                    "QuestService::OnEvent: quest stage-0 reset formId: {:X}, netQuestType: {}, {} {}, name: {}",
                    formId, netQuestType, isLeader ? "leader" : "player", PlayerId(), qName);
            }
            else
            {
                spdlog::info(
                    "QuestService::OnEvent: stage update quest formId: {:X}, questStage: {}, netQuestType: {}, {} {}, name: {}",
                    formId, stageId, netQuestType, isLeader ? "leader" : "player", PlayerId(), qName);
            }

            RequestQuestUpdate update;
            update.Id = Id;
            update.Stage = stageId;
            update.Status = isReset ? RequestQuestUpdate::Reset : RequestQuestUpdate::StageUpdate;
            update.ClientQuestType = netQuestType;
            m_world.GetTransport().Send(update);
        });

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESSceneEvent* apEvent, const EventDispatcher<TESSceneEvent>*)
{
    if (s_inRemoteQuestUpdate || !apEvent || !World::IsCreated() || !World::IsConnectedCached() || ScopedSaveLoadOverride::IsOverriden())
        return BSTEventResult::kOk;
    auto pScene = Cast<BGSScene>(TESForm::GetById(apEvent->sceneFormId));
    if (!pScene)
        return BSTEventResult::kOk;
    auto pQuest = pScene->owningQuest;
    if (pQuest == nullptr || QuestService::IsNonSyncableQuest(pQuest))
        return BSTEventResult::kOk;

    const uint8_t netQuestType = NormalizeNetworkQuestType(pQuest);

    m_world.GetRunner().Queue(
        [this, sceneId = apEvent->sceneFormId, questId = pQuest->formID, stageId = pQuest->currentStage, netQuestType, sceneType = apEvent->sceneType]()
        {
            if (!m_world.GetPartyService().IsInParty() || !m_world.GetPartyService().IsLeader())
                return;

            GameId sceneGameId{};
            GameId questGameId{};
            auto& modSys = m_world.GetModSystem();
            if (modSys.GetServerModId(sceneId, sceneGameId) && modSys.GetServerModId(questId, questGameId))
            {
                TESQuest* pLiveQuest = Cast<TESQuest>(TESForm::GetById(questId));
                const uint16_t liveStageId = pLiveQuest ? pLiveQuest->currentStage : stageId;
                RequestQuestSceneUpdate update;
                update.SceneId = sceneGameId;
                update.QuestId = questGameId;
                update.Stage = liveStageId;
                update.Status = RequestQuestUpdate::StageUpdateForced;
                update.ClientQuestType = netQuestType;
                update.SceneType = sceneType;

                m_world.GetTransport().Send(update);
            }
        });

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESSceneActionEvent* apEvent, const EventDispatcher<TESSceneActionEvent>*)
{
    if (apEvent)
        spdlog::debug("TESSceneActionEvent: questId {:X}, scene id {:X}, action index {}", apEvent->questFormId, apEvent->sceneFormId, apEvent->actionIndex);
    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESScenePhaseEvent* apEvent, const EventDispatcher<TESScenePhaseEvent>*)
{
    if (s_inRemoteQuestUpdate || !apEvent || !World::IsCreated() || !World::IsConnectedCached() || ScopedSaveLoadOverride::IsOverriden())
        return BSTEventResult::kOk;

    const TESSceneEvent event = {nullptr, apEvent->sceneFormId, 0};
    if (apEvent->sceneType == 0 || apEvent->phaseIndex == 0)
        OnEvent(&event, nullptr);

    return BSTEventResult::kOk;
}

void QuestService::OnQuestUpdate(const NotifyQuestUpdate& aUpdate) noexcept
{
    auto Id = aUpdate.Id;
    auto formId = World::Get().GetModSystem().GetGameId(aUpdate.Id);
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(formId));
    if (pQuest == nullptr || !m_world.Get().GetPartyService().IsInParty())
        return;

    const auto startStop = pQuest->IsStopped() ? "stopped" : "started";
    const bool isMiscNone = (pQuest->type == TESQuest::Type::None || pQuest->type == TESQuest::Type::Miscellaneous); // If we can't get the GameId we can't sync anyway.
    const TiltedPhoques::String miscQuest(isMiscNone ? spdfmt::format("none/misc quest gameId {:X},", Id.LogFormat()) : "quest");
    const bool isLeader = m_world.Get().GetPartyService().IsLeader();
    const auto playerString = isLeader ? "leader" : "player";
    const auto isSceneMaster = aUpdate.SceneMaster == PlayerId();
    const char* questName = pQuest->fullName.value.AsAscii() ? pQuest->fullName.value.AsAscii() : "";

    // Quest OnEvent()s send updates to the server where the Leader deduplicates them,
    // so most remote updates (QuestService::OnQuestUpdate) are coming from the Leader.
    //
    // But when Party Members are all playing a scene, the scene itself is advancing
    // the quest, so everyone is sending the same quest updates close enough together some
    // can sneak through the dedup code. That's solved client-side by not allowing rewind
    // when in a scene.
    //
    // The initiator of the scene is remembered as SceneMaster and sent in NotifyQuestUpdate.
    // Currently only used in experiments & logs, we may end up needing it.
    //
    // FIXME: Experiment: To unstick non-SceneMasters if they get stuck on an interaction,
    // the SceneMaster sends a forced quest stage update at the end of the scene. This may
    // be unnecessary now that we have fixed scene playing speed sync to be better.
    bool updateDisabled = pQuest->IsAnyCutscenePlaying() && (aUpdate.Status == NotifyQuestUpdate::StageUpdate && aUpdate.Stage < pQuest->currentStage);

    if (updateDisabled)
    {
        spdlog::debug(
            __FUNCTION__ ": suppressing quest stage update while playing a scene: gameId: {:X}, formId: {:X}, "
                         "questStage: {}, questStatus: {}, questType: {}, SceneMaster {}, "
                         "isAnyCutscenePlaying {}, player {}, formId: {:X}, name: {}",
            aUpdate.Id.LogFormat(), formId, aUpdate.Stage, aUpdate.Status, aUpdate.ClientQuestType, PlayerId() == aUpdate.SceneMaster, pQuest->IsAnyCutscenePlaying(), PlayerId(),
            formId, questName);
        return;
    }

    ScopedRemoteQuestUpdateGuard remoteQuestGuard;

    bool wasUpdated = false;
    const bool isRunning = pQuest->getState() == TESQuest::State::Running;
    switch (aUpdate.Status)
    {
    case NotifyQuestUpdate::Started:
        if (isRunning)
        {
            spdlog::debug(
                __FUNCTION__ ": suppressing duplicate start {} formId: {:X}, questStage: {}, "
                             "questType: {}, isStopped: {}, flags: {:X}, {} {}, name: {}",
                miscQuest, formId, aUpdate.Stage, static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type), pQuest->IsStopped(),
                pQuest->flags, playerString, PlayerId(), questName);
        }
        else
        {
            spdlog::debug(
                __FUNCTION__ ": remotely started {} formId: {:X}, questStage: {}, "
                             "questType: {}, isStopped: {}, flags: {:X}, {} {}, name: {}",
                miscQuest, formId, aUpdate.Stage, static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type), pQuest->IsStopped(),
                pQuest->flags, playerString, PlayerId(), questName);

            bool started = false;
            pQuest->EnsureQuestStarted(started, false);
            if (aUpdate.Stage > 0 || !pQuest->IsStageDone(aUpdate.Stage))
                pQuest->ScriptSetStage(aUpdate.Stage, true);
            pQuest->SetActive(true);
        }
        wasUpdated = true;
        break;

    case NotifyQuestUpdate::Reset:
        spdlog::critical(
            __FUNCTION__ ": REPORT THIS LOG, experimental remote reset {} formId: {:X}, questStage: {}, questType: {}, "
                         "isStopped: {}, flags: {:X}, {} {}, name: {}",
            miscQuest, formId, aUpdate.Stage, static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type), pQuest->IsStopped(),
            pQuest->flags, playerString, PlayerId(), questName);

        pQuest->ScriptResetAndUpdate();
        wasUpdated = true;
        break;

    case NotifyQuestUpdate::StageUpdate:
        spdlog::debug(
            __FUNCTION__ ": remotely updated {} formId: {:X}, questStage: {}, questType: {}, "
                         "isStopped: {}, flags: {:X}, {} {}, name: {}",
            miscQuest, formId, aUpdate.Stage, static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type), pQuest->IsStopped(),
            pQuest->flags, playerString, PlayerId(), questName);

        if (pQuest->IsStageDone(aUpdate.Stage) && pQuest->currentStage >= aUpdate.Stage)
        {
            wasUpdated = true;
        }
        else
        {
            if (pQuest->IsStopped())
            {
                bool started = false;
                pQuest->EnsureQuestStarted(started, false);
            }
            wasUpdated = pQuest->ScriptSetStage(aUpdate.Stage, true);
        }
        break;

    case NotifyQuestUpdate::Stopped:
        spdlog::debug(
            __FUNCTION__ ": remotely stopped {} formId: {:X}, questStage: {}, questType: {}, "
                         "isStopped: {}, flags: {:X}, {} {}, name: {}",
            miscQuest, formId, aUpdate.Stage, static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type), pQuest->IsStopped(),
            pQuest->flags, playerString, PlayerId(), questName);
        wasUpdated = StopQuest(formId);
        break;

    default:
        spdlog::error(
            __FUNCTION__ ": unknown remote status {} {} formId: {:X}, questStage: {}, "
                         "questType: {}, isStopped: {}, flags: {:X}, {} {}, name: {}",
            aUpdate.Status, miscQuest, formId, aUpdate.Stage, static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type),
            pQuest->IsStopped(), pQuest->flags, playerString, PlayerId(), questName);
        break;
    }

    if (!wasUpdated)
        spdlog::error(
            __FUNCTION__ ": failed to remotely update status {} {} formId: {:X}, questStage: "
                         "{}, questType: {}, isStopped: {}, flags: {:X}, {} {}, name: {}",
            aUpdate.Status, miscQuest, formId, aUpdate.Stage, static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type),
            pQuest->IsStopped(), pQuest->flags, playerString, PlayerId(), questName);
    else
        m_world.GetCharacterService().NotifyRemoteQuestStageApplied();
}

void QuestService::OnQuestSceneUpdate(const NotifyQuestSceneUpdate& aUpdate) noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();
    auto pQuest = Cast<TESQuest>(TESForm::GetById(modSystem.GetGameId(aUpdate.QuestId)));
    auto pScene = Cast<BGSScene>(TESForm::GetById(modSystem.GetGameId(aUpdate.SceneId)));
    const bool bIsLeader = m_world.GetPartyService().IsLeader();
    const auto playerType = bIsLeader ? "leaderId" : "playerId";

    if (!pQuest)
    {
        spdlog::error(__FUNCTION__ ": failed to find questId: {:X}, {} {}", aUpdate.QuestId.LogFormat(), playerType, PlayerId());
        return;
    }
    if (!pScene)
    {
        spdlog::error(__FUNCTION__ ": failed to find sceneId: {:X}, {} {}", aUpdate.SceneId.LogFormat(), playerType, PlayerId());
        return;
    }

    ScopedRemoteQuestUpdateGuard remoteSceneGuard;

    if (aUpdate.SceneType == 0) // Scene Begin (Host-canonical)
    {
        if (!pQuest->IsEnabled())
        {
            bool isStarted = false;
            pQuest->EnsureQuestStarted(isStarted, false);
        }

        // On non-leader clients, scene actors are Remote puppets driven by Host's SpeakSound, Subtitles, and
        // AnimationSystem; ensure no conflicting local BGSScene instance hijacks puppet packages until Scene End
        // delivers Host's authoritative StageUpdateForced progression.
        if (!bIsLeader && pScene->isPlaying)
        {
            pScene->ScriptStop();
        }
    }
    else // Scene End (Host-canonical)
    {
        if (pScene->isPlaying)
        {
            spdlog::debug(__FUNCTION__ ": stopping completed host scene questId {:X}, sceneId {:X}, {} {}", aUpdate.QuestId.LogFormat(), aUpdate.SceneId.LogFormat(), playerType, PlayerId());
            pScene->ScriptStop();
        }
    }
}

bool QuestService::StopQuest(uint32_t aformId)
{
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(aformId));
    if (pQuest)
    {
        if (pQuest->getState() == TESQuest::State::Stopped) // Supress duplicate or loopback quest stop
        {
            const char* questName = pQuest->fullName.value.AsAscii() ? pQuest->fullName.value.AsAscii() : "";
            spdlog::debug(
                __FUNCTION__ ": suppressing duplicate quest stop formId: {:X}, questStage: {}, questFlags: {:X}, questType: {}, formId: {:X}, name: {}", aformId,
                pQuest->currentStage, static_cast<uint8_t>(pQuest->flags), static_cast<uint16_t>(pQuest->type), aformId, questName);
        }
        else
        {
            pQuest->SetActive(false);
            pQuest->SetStopped();
        }

        return true;
    }

    return false;
}

static constexpr std::array kNonSyncableQuestIds = std::to_array<uint32_t>({
    0x2BA16,   // Werewolf transformation quest
    0x20071D0, // Vampire transformation quest
    0x3AC44,   // MS13BleakFallsBarrowLeverScene
    // 0xFE014801,  // Unknown dynamic ID, kept as note, maybe lookup correct ID this game?
    0xF2593 // Skill experience quest
});

bool QuestService::IsNonSyncableQuest(TESQuest* apQuest)
{
    if (!apQuest)
        return true;

    // Quests with no quest stages in BOTH executed and waiting lists are handler/MCM quests and never synced.
    const bool noExecutedStages = !apQuest->pExecutedStages || apQuest->pExecutedStages->Empty();
    const bool noWaitingStages = !apQuest->pWaitingStages || apQuest->pWaitingStages->Empty();
    if (noExecutedStages && noWaitingStages)
        return true;

    const char* edid = ResolveQuestEditorId(apQuest);
    if (RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId(edid, apQuest->formID))
        return true;

    if (std::find(kNonSyncableQuestIds.begin(), kNonSyncableQuestIds.end(), apQuest->formID) != kNonSyncableQuestIds.end())
        return true;

    if (apQuest->type == TESQuest::Type::None && !RfabCompat::IsDungeonOrStoryQuestEditorId(edid))
        return true;

    return false;
}

void QuestService::DebugDumpQuests()
{
    auto* pModManager = ModManager::Get();
    if (!pModManager)
        return;

    auto& quests = pModManager->quests;
    for (TESQuest* pQuest : quests)
    {
        if (!pQuest)
            continue;
        const char* idStr = pQuest->idName.AsAscii() ? pQuest->idName.AsAscii() : "";
        spdlog::info("{:X}|{}|{}|{}", pQuest->formID, (uint8_t)pQuest->type, pQuest->priority, idStr);
    }
}
