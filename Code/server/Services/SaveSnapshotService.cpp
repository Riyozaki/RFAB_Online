#include <Services/SaveSnapshotService.h>

#include <common/RfabModpackCompat.h>
#include <Components.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <GameServer.h>
#include <Messages/CoordinatedSaveAck.h>
#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/NotifyCommitSave.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/NotifyExecuteLoad.h>
#include <Messages/NotifyPrepareSave.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/RequestCoordinatedSaveLoad.h>
#include <Services/CalendarService.h>
#include <Services/EncounterService.h>
#include <Services/PartyService.h>
#include <Setting.h>
#include <World.h>

#include <chrono>
#include <cstdio>
#include <random>

namespace
{
constexpr uint32_t kHealthAvIndex = 24;

Console::Setting uPostCombatAutosaveInterval{
    "Gameplay:uPostCombatAutosaveInterval",
    "Minimum seconds between automatic post-combat checkpoints (boss kills always save immediately)",
    180U};

uint64_t NowSteadyMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string MakeSessionTag()
{
    std::random_device rd;
    const uint32_t val = rd() & 0xFFFFu;
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "S%04X", val);
    return std::string(buf);
}
} // namespace

SaveSnapshotService::SaveSnapshotService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_coordinator(MakeSessionTag(), SaveSnapshotCoordinator::kDefaultRingSize)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&SaveSnapshotService::OnUpdate>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&SaveSnapshotService::OnPlayerLeave>(this))
    , m_requestConnection(aDispatcher.sink<PacketEvent<RequestCoordinatedSaveLoad>>().connect<&SaveSnapshotService::OnSaveLoadRequest>(this))
    , m_ackConnection(aDispatcher.sink<PacketEvent<CoordinatedSaveAck>>().connect<&SaveSnapshotService::OnSaveAck>(this))
{
}

bool SaveSnapshotService::IsPlayerAuthorizedHost(Player* apPlayer) noexcept
{
    if (!apPlayer)
        return false;

    if (auto* pServer = GameServer::Get())
    {
        if (pServer->GetAdminSessions().contains(apPlayer->GetConnectionId()))
            return true;
    }

    auto& partyService = m_world.GetPartyService();
    if (partyService.IsPlayerInParty(apPlayer))
        return partyService.IsPlayerLeader(apPlayer);

    // If no party exists yet, treat the lowest-ID connected player (first host) as authorized.
    Player* pFirstPlayer = nullptr;
    for (Player* pCandidate : m_world.GetPlayerManager())
    {
        if (!pFirstPlayer || pCandidate->GetId() < pFirstPlayer->GetId())
            pFirstPlayer = pCandidate;
    }

    return pFirstPlayer == apPlayer;
}

bool SaveSnapshotService::TriggerCoordinatedSave(bool aIsAutoCheckpoint, Player* apRequester) noexcept
{
    if (apRequester && !IsPlayerAuthorizedHost(apRequester))
        return false;

    std::vector<uint32_t> playerIds;
    for (Player* pPlayer : m_world.GetPlayerManager())
        playerIds.push_back(pPlayer->GetId());

    const uint64_t nowMs = NowSteadyMs();
    const auto barrierOpt = m_coordinator.BeginBarrier(playerIds, aIsAutoCheckpoint, nowMs);
    if (!barrierOpt.has_value())
        return false;

    NotifyPrepareSave prepare{};
    prepare.EpochId = barrierOpt->EpochId;
    prepare.SlotName = String(barrierOpt->SlotName.c_str());
    prepare.IsAutoCheckpoint = aIsAutoCheckpoint;

    GameServer::Get()->SendToPlayers(prepare, nullptr);
    spdlog::info(
        "[RFAB SaveSnapshot] Started two-phase save barrier Epoch={} Slot='{}' Auto={} Players={}",
        barrierOpt->EpochId,
        barrierOpt->SlotName,
        aIsAutoCheckpoint,
        playerIds.size());
    return true;
}

SaveSnapshotService::WorldSnapshot SaveSnapshotService::CaptureWorldSnapshot(
    uint64_t aEpochId,
    const std::string& acSlotName,
    bool aIsAuto,
    uint64_t aNowMs) const
{
    WorldSnapshot snap{};
    snap.EpochId = aEpochId;
    snap.SlotName = acSlotName;
    snap.IsAutoCheckpoint = aIsAuto;
    snap.CreatedAtMs = aNowMs;

    // 1. Capture per-player and merged quest state
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        const auto& qlog = pPlayer->GetQuestLogComponent().QuestContent;
        snap.QuestLogsByPlayerId[pPlayer->GetId()] = qlog;

        for (const auto& entry : qlog.Entries)
        {
            auto it = std::find_if(
                snap.SharedPartyQuestLog.Entries.begin(),
                snap.SharedPartyQuestLog.Entries.end(),
                [&entry](const auto& existing) { return existing.Id == entry.Id; });

            if (it == snap.SharedPartyQuestLog.Entries.end())
                snap.SharedPartyQuestLog.Entries.push_back(entry);
            else if (entry.Stage > it->Stage)
                it->Stage = entry.Stage;
        }
    }

    // 2. Capture all tracked NPC/boss health & alive/dead flags
    auto actorView = m_world.view<CharacterComponent>();
    for (auto entity : actorView)
    {
        const auto& charComp = actorView.get<CharacterComponent>(entity);
        if (charComp.IsPlayer())
            continue;

        ActorStateRecord rec{};
        rec.IsDead = charComp.IsDead();
        if (const auto* pAv = m_world.try_get<ActorValuesComponent>(entity))
        {
            if (const auto hpIt = pAv->CurrentActorValues.ActorValuesList.find(kHealthAvIndex);
                hpIt != pAv->CurrentActorValues.ActorValuesList.end())
            {
                rec.CurrentHealth = hpIt->second;
            }
            if (const auto maxIt = pAv->CurrentActorValues.ActorMaxValuesList.find(kHealthAvIndex);
                maxIt != pAv->CurrentActorValues.ActorMaxValuesList.end())
            {
                rec.MaxHealth = maxIt->second;
            }
        }
        snap.TrackedActors[World::ToInteger(entity)] = rec;
    }

    // 3. Capture authoritative RFAB XP ledger, read XP books, globals & ambush history
    snap.Progression = m_world.GetRfabProgressionService().GetLedger().CaptureSnapshot();

    // 4. Capture world calendar state
    const auto& calendar = m_world.GetCalendarService();
    const auto [hour, minute] = calendar.GetTime();
    const auto [day, month, year] = calendar.GetDate();
    snap.Calendar.Hour = hour;
    snap.Calendar.Minute = minute;
    snap.Calendar.TimeScale = calendar.GetTimeScale();
    snap.Calendar.Day = day;
    snap.Calendar.Month = static_cast<int>(month);
    snap.Calendar.Year = static_cast<float>(year);

    // 5. Capture all world containers & lock states (ObjectComponent + InventoryComponent)
    auto objectView = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();
    for (auto entity : objectView)
    {
        ObjectStateRecord objRec{};
        objRec.Id = objectView.get<FormIdComponent>(entity).Id;
        objRec.CurrentLockData = objectView.get<ObjectComponent>(entity).CurrentLockData;
        objRec.CurrentInventory = objectView.get<InventoryComponent>(entity).Content;
        if (const auto* pCellComp = m_world.try_get<CellIdComponent>(entity))
        {
            objRec.CellId = pCellComp->Cell;
            objRec.WorldSpaceId = pCellComp->WorldSpaceId;
            objRec.CenterCoords = pCellComp->CenterCoords;
        }
        snap.TrackedObjects.push_back(std::move(objRec));
    }

    return snap;
}

void SaveSnapshotService::FinalizeCommit() noexcept
{
    const uint64_t nowMs = NowSteadyMs();
    const auto committed = m_coordinator.ConsumeAndCommit(nowMs);

    m_snapshots.push_back(CaptureWorldSnapshot(committed.EpochId, committed.SlotName, committed.IsAutoCheckpoint, nowMs));
    while (m_snapshots.size() > SaveSnapshotCoordinator::kMaxSnapshotHistory)
        m_snapshots.pop_front();

    NotifyCommitSave commitMsg{};
    commitMsg.EpochId = committed.EpochId;
    commitMsg.SlotName = String(committed.SlotName.c_str());
    commitMsg.Committed = true;
    commitMsg.Message = committed.IsAutoCheckpoint
        ? String("RFAB Online: Чекпоинт после боя сохранён")
        : String("RFAB Online: Сохранение группы завершено");

    GameServer::Get()->SendToPlayers(commitMsg, nullptr);
    spdlog::info(
        "[RFAB SaveSnapshot] Committed Epoch={} Slot='{}' (Total snapshots={})",
        committed.EpochId,
        committed.SlotName,
        m_snapshots.size());
}

void SaveSnapshotService::AbortBarrier(const std::string& acReason) noexcept
{
    if (!m_coordinator.HasPendingBarrier())
        return;

    const auto aborted = m_coordinator.ConsumeAborted();

    NotifyCommitSave abortMsg{};
    abortMsg.EpochId = aborted.EpochId;
    abortMsg.SlotName = String(aborted.SlotName.c_str());
    abortMsg.Committed = false;
    abortMsg.Message = String(("RFAB Online: Сохранение отложено (" + acReason + ")").c_str());

    GameServer::Get()->SendToPlayers(abortMsg, nullptr);
    spdlog::warn("[RFAB SaveSnapshot] Aborted save barrier Epoch={}: {}", aborted.EpochId, acReason);
}

void SaveSnapshotService::ApplyWorldSnapshot(const WorldSnapshot& acSnapshot, bool aIsPartyWipe) noexcept
{
    m_coordinator.NotifyRolledBackTo(acSnapshot.EpochId, acSnapshot.SlotName, NowSteadyMs());

    // 1. Restore RFAB progression ledger (so bosses/quests attempted AFTER this checkpoint award full XP again!)
    m_world.GetRfabProgressionService().GetLedger().RestoreSnapshot(acSnapshot.Progression);

    // 2. Restore quest logs for connected players AND flush transient 30s QuestStageDedupHistory + SceneMaster
    // so replaying a quest stage or dialogue scene right after F9 / Party Wipe is NEVER suppressed as a "duplicate"!
    auto& partyService = m_world.GetPartyService();
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (PartyService::Party* pParty = partyService.GetPlayerParty(pPlayer))
        {
            pParty->GetQuestStageDedupHistory().Reset();
            pParty->GetQuestStageDedupHistory().ResetSceneMaster();
        }

        if (const auto it = acSnapshot.QuestLogsByPlayerId.find(pPlayer->GetId()); it != acSnapshot.QuestLogsByPlayerId.end())
            pPlayer->GetQuestLogComponent().QuestContent = it->second;
        else
            pPlayer->GetQuestLogComponent().QuestContent = acSnapshot.SharedPartyQuestLog;
    }

    // 3. Restore tracked NPC/boss alive/dead state & health on the server
    for (const auto& [serverEntityId, savedState] : acSnapshot.TrackedActors)
    {
        const entt::entity entity = static_cast<entt::entity>(serverEntityId);
        if (!m_world.valid(entity))
            continue;

        if (auto* pChar = m_world.try_get<CharacterComponent>(entity))
            pChar->SetDead(savedState.IsDead);

        if (auto* pAv = m_world.try_get<ActorValuesComponent>(entity))
        {
            if (savedState.MaxHealth > 0.0f)
                pAv->CurrentActorValues.ActorMaxValuesList[kHealthAvIndex] = savedState.MaxHealth;
            pAv->CurrentActorValues.ActorValuesList[kHealthAvIndex] = savedState.CurrentHealth;
        }
    }

    // 3b. Restore world containers & lock states (ObjectComponent + InventoryComponent),
    // removing any objects first registered AFTER this checkpoint and restoring pre-checkpoint loot/locks!
    {
        auto objectView = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();
        std::vector<entt::entity> postCheckpointObjects;
        for (auto entity : objectView)
        {
            const GameId& objId = objectView.get<FormIdComponent>(entity).Id;
            auto it = std::find_if(
                acSnapshot.TrackedObjects.begin(),
                acSnapshot.TrackedObjects.end(),
                [&objId](const ObjectStateRecord& rec) { return rec.Id == objId; });

            if (it == acSnapshot.TrackedObjects.end())
            {
                postCheckpointObjects.push_back(entity);
            }
            else
            {
                objectView.get<ObjectComponent>(entity).CurrentLockData = it->CurrentLockData;
                objectView.get<InventoryComponent>(entity).Content = it->CurrentInventory;
            }
        }
        for (entt::entity entity : postCheckpointObjects)
        {
            if (m_world.valid(entity))
                m_world.destroy(entity);
        }
    }

    // 4. Restore server calendar date & time
    auto& calendar = m_world.GetCalendarService();
    calendar.SetDate(acSnapshot.Calendar.Day, acSnapshot.Calendar.Month, acSnapshot.Calendar.Year);
    calendar.SetTime(acSnapshot.Calendar.Hour, acSnapshot.Calendar.Minute, acSnapshot.Calendar.TimeScale);

    // 5. Clear all downed/revive/wipe states before clients reload
    m_world.GetEncounterService().ResetAllStatesAfterRollback();

    // 6. Drop snapshots strictly newer than the restored epoch so subsequent wipes land cleanly here
    while (!m_snapshots.empty() && m_snapshots.back().EpochId > acSnapshot.EpochId)
        m_snapshots.pop_back();

    // 7. Command all connected clients to load the matching slot synchronously
    NotifyExecuteLoad loadCmd{};
    loadCmd.EpochId = acSnapshot.EpochId;
    loadCmd.SlotName = String(acSnapshot.SlotName.c_str());
    loadCmd.IsPartyWipe = aIsPartyWipe;
    loadCmd.AllowPreJoinFallback = true;
    loadCmd.Message = aIsPartyWipe
        ? String("RFAB Online: Отряд пал — возврат к последнему чекпоинту")
        : String("RFAB Online: Синхронная загрузка чекпоинта группы");

    GameServer::Get()->SendToPlayers(loadCmd, nullptr);

    // Broadcast monotonic WorldGeneration marker & authoritative RFAB globals so every client immediately
    // advances its local WorldEpoch and discards any pre-rollback progression events still in flight.
    const uint64_t worldGen = m_coordinator.GetCurrentWorldEpoch();
    auto& ledger = m_world.GetRfabProgressionService().GetLedger();
    for (const auto& [editorId, val] : ledger.GetSyncedGlobals())
    {
        NotifyRfabProgressionEvent globalResync{};
        globalResync.Kind = NotifyRfabProgressionEvent::EventKind::GlobalValueSync;
        globalResync.EpochId = worldGen;
        globalResync.EditorId = String(editorId.c_str());
        globalResync.GlobalValue = val;
        globalResync.AuthoritativeTotalXp = ledger.GetTotalSharedXp();
        GameServer::Get()->SendToPlayers(globalResync, nullptr);
    }

    NotifyRfabProgressionEvent epochMarker{};
    epochMarker.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpGrant;
    epochMarker.EpochId = worldGen;
    epochMarker.XpDelta = 0;
    epochMarker.AuthoritativeTotalXp = ledger.GetTotalSharedXp();
    GameServer::Get()->SendToPlayers(epochMarker, nullptr);

    spdlog::info(
        "[RFAB SaveSnapshot] Rolled back server world & dispatched synchronous load to Epoch={} WorldGen={} Slot='{}' (PartyWipe={}, Digest={})",
        acSnapshot.EpochId,
        worldGen,
        acSnapshot.SlotName,
        aIsPartyWipe,
        BuildLiveWorldDigest().ComputeHexDigest());
}

bool SaveSnapshotService::TriggerRollbackToLatest(bool aIsPartyWipe) noexcept
{
    if (!m_snapshots.empty())
    {
        const WorldSnapshot target = m_snapshots.back();
        ApplyWorldSnapshot(target, aIsPartyWipe);
        return true;
    }

    // Even if a wipe occurs BEFORE the first multiplayer checkpoint was created in this session,
    // advance WorldGeneration, flush transient quest dedup cache, clear server downed/wipe state,
    // and instruct every client to reload its PreJoin backup save!
    m_coordinator.NotifyRolledBackTo(0, "", NowSteadyMs());
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(pPlayer))
        {
            pParty->GetQuestStageDedupHistory().Reset();
            pParty->GetQuestStageDedupHistory().ResetSceneMaster();
        }
    }
    m_world.GetEncounterService().ResetAllStatesAfterRollback();
    {
        auto objectView = m_world.view<ObjectComponent>();
        std::vector<entt::entity> toDestroy(objectView.begin(), objectView.end());
        for (entt::entity entity : toDestroy)
        {
            if (m_world.valid(entity))
                m_world.destroy(entity);
        }
    }

    NotifyExecuteLoad fallbackLoad{};
    fallbackLoad.EpochId = 0;
    fallbackLoad.SlotName = "";
    fallbackLoad.IsPartyWipe = aIsPartyWipe;
    fallbackLoad.AllowPreJoinFallback = true;
    fallbackLoad.Message = String("RFAB Online: Отряд пал — загрузка стартового сохранения сессии");
    GameServer::Get()->SendToPlayers(fallbackLoad, nullptr);

    NotifyRfabProgressionEvent epochMarker{};
    epochMarker.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpGrant;
    epochMarker.EpochId = m_coordinator.GetCurrentWorldEpoch();
    epochMarker.XpDelta = 0;
    epochMarker.AuthoritativeTotalXp = m_world.GetRfabProgressionService().GetLedger().GetTotalSharedXp();
    GameServer::Get()->SendToPlayers(epochMarker, nullptr);

    spdlog::info("[RFAB SaveSnapshot] Triggered pre-checkpoint session backup reload (PartyWipe={})", aIsPartyWipe);
    return true;
}

bool SaveSnapshotService::TriggerRollbackToPrevious() noexcept
{
    if (m_snapshots.size() < 2)
        return TriggerRollbackToLatest(false);

    const WorldSnapshot target = m_snapshots[m_snapshots.size() - 2];
    ApplyWorldSnapshot(target, false);
    return true;
}

bool SaveSnapshotService::TriggerRollbackToSlot(const std::string& acSlotName, bool aIsPartyWipe) noexcept
{
    const std::string normTarget = RfabCompat::Detail::NormalizeSaveSlotStem(acSlotName);
    for (auto it = m_snapshots.rbegin(); it != m_snapshots.rend(); ++it)
    {
        if (!normTarget.empty() && RfabCompat::Detail::NormalizeSaveSlotStem(it->SlotName) == normTarget)
        {
            const WorldSnapshot target = *it;
            ApplyWorldSnapshot(target, aIsPartyWipe);
            return true;
        }
    }

    return TriggerRollbackToLatest(aIsPartyWipe);
}

void SaveSnapshotService::NotifyEncounterVictory(bool aBossDefeated) noexcept
{
    const uint64_t nowMs = NowSteadyMs();
    const uint32_t intervalSec = uPostCombatAutosaveInterval.value_as<uint32_t>();
    if (!m_coordinator.ShouldTriggerPostCombatAutosave(nowMs, intervalSec, aBossDefeated))
        return;

    TriggerCoordinatedSave(true, nullptr);
}

void SaveSnapshotService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_coordinator.HasBarrierTimedOut(NowSteadyMs()))
        AbortBarrier("таймаут ожидания одного из игроков в меню/загрузке");
}

void SaveSnapshotService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (!acEvent.pPlayer)
        return;

    const auto res = m_coordinator.HandlePlayerDisconnected(acEvent.pPlayer->GetId());
    if (res == SaveSnapshotCoordinator::AckResolution::ReadyToCommit)
        FinalizeCommit();
    else if (res == SaveSnapshotCoordinator::AckResolution::Aborted)
        AbortBarrier("игрок отключился");
}

void SaveSnapshotService::OnSaveLoadRequest(const PacketEvent<RequestCoordinatedSaveLoad>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const auto& req = acMessage.Packet;

    // Allow ANY connected player in the session to request a party-wide ManualSave (F5 / Menu -> Save)
    // so both players' characters, RFAB XP, items, and world positions are ALWAYS saved together seamlessly!
    if (req.Action == RequestCoordinatedSaveLoad::ActionType::ManualSave)
    {
        TriggerCoordinatedSave(false, nullptr);
        return;
    }

    if (!IsPlayerAuthorizedHost(pSender))
    {
        spdlog::debug("[RFAB SaveSnapshot] Ignoring non-host load request from player {}", pSender->GetId());
        return;
    }

    switch (req.Action)
    {
    case RequestCoordinatedSaveLoad::ActionType::ManualSave:
        TriggerCoordinatedSave(false, nullptr);
        break;
    case RequestCoordinatedSaveLoad::ActionType::LoadLatestCheckpoint:
        TriggerRollbackToLatest(false);
        break;
    case RequestCoordinatedSaveLoad::ActionType::LoadPreviousCheckpoint:
        TriggerRollbackToPrevious();
        break;
    case RequestCoordinatedSaveLoad::ActionType::LoadSpecificSlot:
        TriggerRollbackToSlot(std::string(req.SlotName.c_str(), req.SlotName.size()), false);
        break;
    }
}

void SaveSnapshotService::OnSaveAck(const PacketEvent<CoordinatedSaveAck>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const auto& ack = acMessage.Packet;
    const std::string reason(ack.Reason.c_str(), ack.Reason.size());
    const auto res = m_coordinator.RecordClientAck(pSender->GetId(), ack.EpochId, ack.Success, reason);
    if (res == SaveSnapshotCoordinator::AckResolution::ReadyToCommit)
        FinalizeCommit();
    else if (res == SaveSnapshotCoordinator::AckResolution::Aborted)
        AbortBarrier(reason);
}

CoopFlight::BucketedStateDigest SaveSnapshotService::BuildLiveWorldDigest() const
{
    CoopFlight::BucketedStateDigest digest;

    const auto& ledger = m_world.GetRfabProgressionService().GetLedger();
    digest.Put(CoopFlight::StateCategory::kSharedXp, "total_xp", static_cast<uint64_t>(ledger.GetTotalSharedXp()));

    for (const auto& bookId : ledger.GetReadBooks())
    {
        char key[32]{};
        std::snprintf(key, sizeof(key), "%08X:%08X", bookId.ModId, bookId.BaseId);
        digest.Put(CoopFlight::StateCategory::kReadBook, key, 1ULL);
    }

    for (const auto& [editorId, val] : ledger.GetSyncedGlobals())
    {
        const auto quantized = static_cast<uint64_t>(static_cast<int64_t>(val * 1000.0f));
        digest.Put(CoopFlight::StateCategory::kSyncedGlobal, editorId, quantized);
    }

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        for (const auto& q : pPlayer->GetQuestLogComponent().QuestContent.Entries)
        {
            char key[32]{};
            std::snprintf(key, sizeof(key), "%08X:%08X", q.Id.ModId, q.Id.BaseId);
            digest.Put(CoopFlight::StateCategory::kQuestStage, key, static_cast<uint64_t>(q.Stage));
        }
    }

    auto ownerView = m_world.view<OwnerComponent>();
    for (auto entity : ownerView)
    {
        const auto& ownerComp = ownerView.get<OwnerComponent>(entity);
        char key[24]{};
        std::snprintf(key, sizeof(key), "%08X", World::ToInteger(entity));
        const uint32_t ownerId = ownerComp.GetOwner() ? ownerComp.GetOwner()->GetId() : 0;
        const uint64_t combined = (ownerComp.Epoch << 16) ^ static_cast<uint64_t>(ownerId);
        digest.Put(CoopFlight::StateCategory::kActorOwnership, key, combined);
    }

    digest.Put(
        CoopFlight::StateCategory::kSaveCheckpoint,
        m_coordinator.GetLastCommittedSlot().empty() ? "none" : m_coordinator.GetLastCommittedSlot(),
        m_coordinator.GetLastCommittedEpoch());

    return digest;
}
