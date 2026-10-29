#include <GameServer.h>
#include <Components.h>

#include <World.h>
#include <Services/QuestService.h>
#include <Services/PartyService.h>
#include <Events/PlayerEnterWorldEvent.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/RequestQuestSceneUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/NotifyQuestSceneUpdate.h>

#include <Setting.h>
namespace
{
Console::Setting bEnableMiscQuestSync{"Gameplay:bEnableMiscQuestSync", "Syncs dungeon, bounty, and miscellaneous quests across the party", true};
Console::Setting uQuestHistoryExpiration("GameServer:uQuestHistoryExpiration", "Time in milliseconds to retain quest progression changes for deduplication", 30000U);

void UpsertOrRemovePlayerQuestLogEntry(Player* apTarget, const GameId& acQuestId, uint16_t aStage, uint8_t aStatus) noexcept
{
    if (!apTarget)
        return;

    auto& entries = apTarget->GetQuestLogComponent().QuestContent.Entries;
    auto it = std::find_if(entries.begin(), entries.end(), [&acQuestId](const auto& e) { return e.Id == acQuestId; });
    if (aStatus == NotifyQuestUpdate::Stopped)
    {
        if (it != entries.end())
            entries.erase(it);
        return;
    }

    if (it != entries.end())
        it->Stage = aStage;
    else
        entries.emplace_back(acQuestId, aStage);
}
}

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_questUpdateConnection = aDispatcher.sink<PacketEvent<RequestQuestUpdate>>().connect<&QuestService::OnQuestChanges>(this);
    m_questSceneUpdateConnection = aDispatcher.sink<PacketEvent<RequestQuestSceneUpdate>>().connect<&QuestService::OnQuestSceneChanges>(this);
    m_joinConnection = aDispatcher.sink<PlayerEnterWorldEvent>().connect<&QuestService::OnPlayerEnterWorld>(this);
}

void QuestService::OnQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    auto* pPlayer = acMessage.pPlayer;
    if (!pPlayer || !message.Id)
        return;

    // We'll want to know the player party status for messaging & decisions
    // We want the leader to change things whenever possible, if a Member
    // advances a quest we can't undo it and need to remember.
    auto& partyService = m_world.GetPartyService();
    bool inParty = partyService.IsPlayerInParty(pPlayer);
    bool isLeader = partyService.IsPlayerLeader(pPlayer);
    auto playerTypeString = isLeader ? "leader" : "player";
    auto partyId = pPlayer->GetParty().JoinedPartyId;
    PartyService::Party* pParty = partyService.GetPlayerParty(pPlayer);
    Player* pLeader = isLeader ? pPlayer : (pParty ? m_world.GetPlayerManager().GetById(pParty->LeaderPlayerId) : nullptr);

    // Find the corresponding quest log entry
    auto& questComponent = pPlayer->GetQuestLogComponent();
    auto& entries = questComponent.QuestContent.Entries;
    auto questIt = std::find_if(entries.begin(), entries.end(), [&message](const auto& e) { return e.Id == message.Id; });

    NotifyQuestUpdate notify{};
    notify.Id = message.Id;
    notify.Stage = message.Stage;
    notify.SceneMaster = pParty ? pParty->GetQuestStageDedupHistory().GetSceneMaster() : 0;
    notify.ClientQuestType = message.ClientQuestType;

    // Update QuestComponent. In order to prevent bugs when
    // we "discover" a quest in-progress (first seen with something
    // Other than RequestQuestUpdate::Start), we add it as a new
    // quest record if not found
    if (questIt != entries.end())
    {
        questIt->Id = message.Id;
        questIt->Stage = message.Stage;
    }

    else
    {
        entries.emplace_back(message.Id, message.Stage);
        questIt = std::prev(entries.end());
        spdlog::debug("{}: started/discovered quest: {:X}, stage: {}, by {} {:X}", __FUNCTION__, message.Id.LogFormat(), notify.Stage, playerTypeString, pPlayer->GetId());
    }

    bool useAggressiveSend = false;
    switch (message.Status)
    {
    case RequestQuestUpdate::Started: notify.Status = NotifyQuestUpdate::Started; break;

    case RequestQuestUpdate::StageUpdateForced: useAggressiveSend = true; [[fallthrough]];
    case RequestQuestUpdate::StageUpdate:
        notify.Status = NotifyQuestUpdate::StageUpdate;
        spdlog::debug(
            "{}: updated quest: {:X}, stage: {}, SceneMaster {}, by {} {:X}", __FUNCTION__, message.Id.LogFormat(), notify.Stage, notify.SceneMaster, playerTypeString,
            pPlayer->GetId());
        break;

    case RequestQuestUpdate::Stopped:
        notify.Status = NotifyQuestUpdate::Stopped;
        spdlog::debug("{}: stopped quest: {:X}, stage: {}, by {} {:X}", __FUNCTION__, message.Id.LogFormat(), notify.Stage, playerTypeString, pPlayer->GetId());

        if (questIt != entries.end())
            entries.erase(questIt);
        else
        {
            spdlog::warn("{}: unable to delete quest object {:X} (already stopped or first update is stopped)", __FUNCTION__, notify.Id.LogFormat());
        }
        break;

    case RequestQuestUpdate::Reset:
        useAggressiveSend = true;
        notify.Status = NotifyQuestUpdate::Reset;
        spdlog::warn("{}: FIXME/REVIEW reset quest: {:X}, stage: {}, by {} {:X}", __FUNCTION__, message.Id.LogFormat(), notify.Stage, playerTypeString, pPlayer->GetId());
        break;

    default:
        spdlog::error(
            "{}: unknown quest status {} quest: {:X}, stage: {}, status {}, by {} {:X}", __FUNCTION__, message.Status, message.Id.LogFormat(), notify.Stage, notify.Status,
            playerTypeString, pPlayer->GetId());
        return;
    }

    const uint32_t partyNumber = partyId.value_or(0);
    // All side effects have been generated. Now just logging and a forwarding decision left.
    if (inParty && pParty)
    {
        auto& dedupHistory = pParty->GetQuestStageDedupHistory();
        if (notify.ClientQuestType == 0 ||
            notify.ClientQuestType == 6) // Types None or Miscellaneous. Hard-coded to avoid including client header file.
        {
            if (!bEnableMiscQuestSync)
                return;

            spdlog::debug(
                "{}: syncing type none/misc quest to party, quest: {:X}, questStage: {}, questStatus: {}, "
                "questType: {}",
                __FUNCTION__, notify.Id.LogFormat(), notify.Stage, notify.Status, notify.ClientQuestType);
        }

        // Direct 1-hop deduplicated party quest broadcast:
        // Whether the quest stage was advanced by the Party Leader or by a Party Member (e.g. pulling a dungeon lever,
        // picking up a Dragonstone, reading an Elder Scroll, or completing a dialogue), broadcast directly to all other
        // party members and record them in dedupHistory so remote ScriptSetStage application never loops back.
        bool allPartyMembersAlreadyHaveStage = true;
        if (notify.Status != NotifyQuestUpdate::Stopped)
        {
            for (Player* pMember : pParty->Members)
            {
                if (!pMember || pMember == pPlayer)
                    continue;
                const auto& memberEntries = pMember->GetQuestLogComponent().QuestContent.Entries;
                const auto memberQuestIt = std::find_if(
                    memberEntries.begin(),
                    memberEntries.end(),
                    [&notify](const auto& e) { return e.Id == notify.Id; });
                if (memberQuestIt == memberEntries.end() || memberQuestIt->Stage < notify.Stage)
                {
                    allPartyMembersAlreadyHaveStage = false;
                    break;
                }
            }
        }

        if (!useAggressiveSend &&
            (dedupHistory.FoundWPlayerId(notify.Id, notify.Stage, notify.Status, pPlayer->GetId()) ||
             (partyNumber != 0 && allPartyMembersAlreadyHaveStage &&
              dedupHistory.FoundWPartyId(notify.Id, notify.Stage, notify.Status, partyNumber))))
        {
            spdlog::debug(
                "{}: dropping duplicate quest update: {:X}, stage: {}, status: {}, by {} {:X}",
                __FUNCTION__,
                notify.Id.LogFormat(),
                notify.Stage,
                notify.Status,
                playerTypeString,
                pPlayer->GetId());
            return;
        }

        dedupHistory.Add(notify.Id, notify.Stage, notify.Status, pPlayer->GetId(), std::chrono::steady_clock::now(), partyNumber);

        for (Player* pMember : m_world.GetPlayerManager())
        {
            if (!pMember || pMember == pPlayer)
                continue;

            const auto& partyComponent = pMember->GetParty();
            if (partyComponent.JoinedPartyId != partyId)
                continue;

            UpsertOrRemovePlayerQuestLogEntry(pMember, notify.Id, notify.Stage, notify.Status);

            if (!useAggressiveSend && dedupHistory.FoundWPlayerId(notify.Id, notify.Stage, notify.Status, pMember->GetId()))
            {
                spdlog::debug(
                    "{}: skipping duplicate send quest: {:X}, stage: {}, status: {}, SceneMaster {}, to player {:X}",
                    __FUNCTION__,
                    notify.Id.LogFormat(),
                    notify.Stage,
                    notify.Status,
                    notify.SceneMaster,
                    pMember->GetId());
                continue;
            }

            dedupHistory.Add(notify.Id, notify.Stage, notify.Status, pMember->GetId(), std::chrono::steady_clock::now(), partyNumber);
            pMember->Send(notify);
        }
    }
}

void QuestService::OnQuestSceneChanges(const PacketEvent<RequestQuestSceneUpdate>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;

    auto* pPlayer = acMessage.pPlayer;
    if (!pPlayer || !message.QuestId || !message.SceneId)
        return;
    auto& partyService = m_world.GetPartyService();
    bool isLeader = partyService.IsPlayerLeader(pPlayer);
    auto playerTypeString = isLeader ? "leader" : "player";
    auto sceneTypeString = message.SceneType ? "End" : "Begin";

    const auto& partyComponent = pPlayer->GetParty();
    if (!partyComponent.JoinedPartyId.has_value())
        return;

    PartyService::Party* pParty = partyService.GetPlayerParty(pPlayer);
    if (!pParty)
        return;

    Player* pLeader = isLeader ? pPlayer : m_world.GetPlayerManager().GetById(pParty->LeaderPlayerId);
    if (pLeader != nullptr && !isLeader)
    {
        // Host-canonical BGSScene authority: never allow a non-leader guest who walked into a scene trigger
        // early to hijack SceneMaster and block the Party Leader's authoritative SceneEnd + StageUpdateForced!
        spdlog::debug(
            "{}: ignoring non-leader scene {} update quest {:X}, scene {:X} from player {}",
            __FUNCTION__,
            sceneTypeString,
            message.QuestId.LogFormat(),
            message.SceneId.LogFormat(),
            pPlayer->GetId());
        return;
    }

    auto& dedupHistory = pParty->GetQuestStageDedupHistory();

    // Make sure party knows scene state.
    NotifyQuestSceneUpdate notify{};
    notify.SceneId = message.SceneId;
    notify.QuestId = message.QuestId;
    notify.SceneType = message.SceneType;

    if (message.SceneType == 0) // BEGIN (Party Leader authoritative)
    {
        dedupHistory.Add(message.SceneId, 0, RequestQuestUpdate::StatusCode::SceneUpdate, pPlayer->GetId());
        dedupHistory.SetSceneMaster(pPlayer->GetId());
        spdlog::debug(
            "{}: leader {} is SceneMaster, sending scene {} update quest {:X}, scene {:X}",
            __FUNCTION__,
            pPlayer->GetId(),
            sceneTypeString,
            notify.QuestId.LogFormat(),
            notify.SceneId.LogFormat());
        GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
    }
    else // END scene (Party Leader authoritative)
    {
        spdlog::debug(
            "{}: sending scene {} update quest {:X}, scene {:X}, by {} {}",
            __FUNCTION__,
            sceneTypeString,
            notify.QuestId.LogFormat(),
            notify.SceneId.LogFormat(),
            playerTypeString,
            pPlayer->GetId());
        GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());

        // Force an update out to bring anyone behind to the leader's current quest stage.
        RequestQuestUpdate newUpdate;
        newUpdate.Id = message.QuestId;
        newUpdate.Stage = message.Stage;
        newUpdate.Status = RequestQuestUpdate::StageUpdateForced;
        newUpdate.ClientQuestType = message.ClientQuestType;
        const PacketEvent<RequestQuestUpdate> newMessage(&newUpdate, pPlayer);
        QuestService::OnQuestChanges(newMessage);

        dedupHistory.ResetSceneMaster();
    }
}

void QuestService::OnPlayerEnterWorld(const PlayerEnterWorldEvent& acEvent) noexcept
{
    if (!acEvent.pPlayer)
        return;

    Player* pMutablePlayer = m_world.GetPlayerManager().GetById(acEvent.pPlayer->GetId());
    SyncPartyQuestsForPlayer(pMutablePlayer);
}

void QuestService::SyncLeaderQuestLogToMember(Player* apLeader, Player* apMember) noexcept
{
    if (!apLeader || !apMember || apLeader == apMember)
        return;

    PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(apLeader);
    if (!pParty)
        return;

    const uint32_t partyNumber = apLeader->GetParty().JoinedPartyId.value_or(0);
    auto& dedupHistory = pParty->GetQuestStageDedupHistory();
    const auto& leaderEntries = apLeader->GetQuestLogComponent().QuestContent.Entries;
    auto& memberEntries = apMember->GetQuestLogComponent().QuestContent.Entries;

    for (const auto& leaderQuest : leaderEntries)
    {
        if (!leaderQuest.Id || leaderQuest.Stage == 0)
            continue;

        const auto memberIt = std::find_if(
            memberEntries.begin(),
            memberEntries.end(),
            [&leaderQuest](const auto& e) { return e.Id == leaderQuest.Id; });

        if (memberIt != memberEntries.end() && memberIt->Stage >= leaderQuest.Stage)
            continue;

        if (memberIt != memberEntries.end())
            memberIt->Stage = leaderQuest.Stage;
        else
            memberEntries.emplace_back(leaderQuest.Id, leaderQuest.Stage);

        NotifyQuestUpdate catchupNotify{};
        catchupNotify.Id = leaderQuest.Id;
        catchupNotify.Stage = leaderQuest.Stage;
        catchupNotify.Status = NotifyQuestUpdate::StageUpdate;
        catchupNotify.SceneMaster = dedupHistory.GetSceneMaster();
        catchupNotify.ClientQuestType = 8; // SideQuest / general syncable quest

        dedupHistory.Add(catchupNotify.Id, catchupNotify.Stage, catchupNotify.Status, apMember->GetId(), std::chrono::steady_clock::now(), partyNumber);
        apMember->Send(catchupNotify);
    }
}

void QuestService::SyncPartyQuestsForPlayer(Player* apPlayer) noexcept
{
    if (!apPlayer)
        return;

    auto& partyService = m_world.GetPartyService();
    if (!partyService.IsPlayerInParty(apPlayer))
        return;

    PartyService::Party* pParty = partyService.GetPlayerParty(apPlayer);
    if (!pParty)
        return;

    Player* pLeader = m_world.GetPlayerManager().GetById(pParty->LeaderPlayerId);
    if (pLeader)
    {
        if (apPlayer == pLeader)
        {
            for (Player* pMember : pParty->Members)
            {
                if (pMember && pMember != pLeader)
                    SyncLeaderQuestLogToMember(pLeader, pMember);
            }
        }
        else
        {
            SyncLeaderQuestLogToMember(pLeader, apPlayer);
        }
    }

    // Reconnect catch-up: also replay any higher quest stages held by other online party members
    // back to the reconnecting/joining player (including when the party leader reconnects).
    for (Player* pSource : pParty->Members)
    {
        if (!pSource || pSource == apPlayer)
            continue;
        SyncLeaderQuestLogToMember(pSource, apPlayer);
    }
}


void inline QuestStageDedupHistory::Expire() noexcept
{
    const auto expiration = std::chrono::steady_clock::now() - uQuestHistoryExpiration.value_as<std::chrono::milliseconds>();

    while (!m_cache.empty() && m_cache.front().timestamp < expiration)
    {
        auto& it = m_cache.front();
        spdlog::debug(
            "{}: expiring dedup history entry quest/scene: {:X}, stage: {}, status: {}, by player {:X}", __FUNCTION__,
            it.questOrSceneId.LogFormat(), it.questStage, it.questStatus, it.playerId);
        m_cache.pop_front();
    }
}


QuestStageDedupHistory::const_iterator QuestStageDedupHistory::Find(const QuestOrSceneId& acQuestOrSceneId,
                                                                    const QuestStage acQuestStage,
                                                                    const QuestStatus acQuestStatus) noexcept
{
    Expire();

    return std::find_if(m_cache.begin(), m_cache.end(), [&](const QuestStageDedupHistory::Entry& e) {
        return e.questOrSceneId == acQuestOrSceneId && e.questStage == acQuestStage && e.questStatus == acQuestStatus;
    });
}

QuestStageDedupHistory::const_iterator QuestStageDedupHistory::FindWPlayerId(const QuestOrSceneId& acQuestOrSceneId,
                                                                             const QuestStage acQuestStage,
                                                                             const QuestStatus acQuestStatus,
                                                                             const PlayerId& acPlayerId) noexcept
{
    Expire();

    return std::find_if(m_cache.begin(), m_cache.end(), [&](const QuestStageDedupHistory::Entry& e) {
        return e.questOrSceneId == acQuestOrSceneId && e.questStage == acQuestStage && e.questStatus == acQuestStatus &&
               acPlayerId == e.playerId;
    });
}

QuestStageDedupHistory::const_iterator QuestStageDedupHistory::FindWPartyId(const QuestOrSceneId& acQuestOrSceneId,
                                                                            const QuestStage acQuestStage,
                                                                            const QuestStatus acQuestStatus,
                                                                            uint32_t aPartyId) noexcept
{
    Expire();

    return std::find_if(m_cache.begin(), m_cache.end(), [&](const QuestStageDedupHistory::Entry& e) {
        return e.questOrSceneId == acQuestOrSceneId && e.questStage == acQuestStage && e.questStatus == acQuestStatus &&
               e.partyId != 0 && e.partyId == aPartyId;
    });
}

void QuestStageDedupHistory::Add(const QuestOrSceneId& acQuestOrSceneId, const QuestStage acQuestStage,
                                 const QuestStatus acQuestStatus, const PlayerId& acPlayerId,
                                 const TimeStamp acTimeStamp, uint32_t aPartyId)
{
    Expire();
    m_cache.emplace_back(acQuestOrSceneId, acQuestStage, acQuestStatus, acPlayerId, aPartyId, acTimeStamp);
}
