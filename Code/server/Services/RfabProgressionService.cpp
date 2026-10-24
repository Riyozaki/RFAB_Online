#include <Services/RfabProgressionService.h>

#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <GameServer.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/SyncRfabProgressionEvent.h>
#include <Services/SaveSnapshotService.h>
#include <World.h>

namespace
{
constexpr int32_t kMaxSingleXpGrant = 1'000'000;
}

RfabProgressionService::RfabProgressionService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_progressionConnection(aDispatcher.sink<PacketEvent<SyncRfabProgressionEvent>>().connect<&RfabProgressionService::OnSyncProgressionEvent>(this))
    , m_playerJoinConnection(aDispatcher.sink<PlayerJoinEvent>().connect<&RfabProgressionService::OnPlayerJoin>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&RfabProgressionService::OnPlayerLeave>(this))
{
}

void RfabProgressionService::OnSyncProgressionEvent(const PacketEvent<SyncRfabProgressionEvent>& acMessage) noexcept
{
    const auto& pkt = acMessage.Packet;
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const uint64_t currentWorldEpoch = m_world.GetSaveSnapshotService().GetCurrentWorldEpoch();
    if (pkt.EpochId != 0 && pkt.EpochId < currentWorldEpoch)
    {
        m_world.GetSaveSnapshotService().GetCoordinator().RecordFlightEvent(
            0, CoopFlight::FlightEventKind::kStaleEpochRejected, pSender->GetId(), pkt.EpochId);
        spdlog::debug(
            "[RFAB Progression] Discarded stale progression packet from player {} (pktEpoch={} < currentWorldEpoch={})",
            pSender->GetId(),
            pkt.EpochId,
            currentWorldEpoch);
        return;
    }

    const uint64_t currentEpoch = currentWorldEpoch;
    const std::string sourceKey(pkt.SourceKey.c_str(), pkt.SourceKey.size());
    const std::string editorId(pkt.EditorId.c_str(), pkt.EditorId.size());

    switch (pkt.Kind)
    {
    case SyncRfabProgressionEvent::EventKind::BookRead:
    {
        const bool isNewBook = m_ledger.RecordBookRead(pkt.BookId);
        const int32_t clampedXp = std::clamp(pkt.XpDelta, 0, kMaxSingleXpGrant);
        const auto outcome = isNewBook ? m_ledger.TryGrantXp(sourceKey, clampedXp)
                                       : RfabProgressionLedger::XpGrantOutcome{false, 0, -clampedXp, m_ledger.GetTotalSharedXp()};

        if (isNewBook)
        {
            NotifyRfabProgressionEvent broadcast{};
            broadcast.Kind = NotifyRfabProgressionEvent::EventKind::BookRead;
            broadcast.EpochId = currentEpoch;
            broadcast.SourceKey = pkt.SourceKey;
            broadcast.BookId = pkt.BookId;
            broadcast.XpDelta = outcome.AppliedDelta;
            broadcast.AuthoritativeTotalXp = outcome.AuthoritativeTotalXp;
            GameServer::Get()->SendToPlayers(broadcast, pSender);
        }
        else if (outcome.DuplicateRebateDelta < 0)
        {
            NotifyRfabProgressionEvent rebate{};
            rebate.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpRebate;
            rebate.EpochId = currentEpoch;
            rebate.SourceKey = pkt.SourceKey;
            rebate.BookId = pkt.BookId;
            rebate.XpDelta = outcome.DuplicateRebateDelta;
            rebate.AuthoritativeTotalXp = outcome.AuthoritativeTotalXp;
            pSender->Send(rebate);
        }
        break;
    }

    case SyncRfabProgressionEvent::EventKind::LedgerXpGrant:
    {
        const int32_t clampedXp = std::clamp(pkt.XpDelta, 0, kMaxSingleXpGrant);
        if (clampedXp <= 0)
            return;

        const auto outcome = m_ledger.TryGrantXp(sourceKey, clampedXp);
        if (outcome.Accepted)
        {
            NotifyRfabProgressionEvent broadcast{};
            broadcast.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpGrant;
            broadcast.EpochId = currentEpoch;
            broadcast.SourceKey = pkt.SourceKey;
            broadcast.XpDelta = outcome.AppliedDelta;
            broadcast.AuthoritativeTotalXp = outcome.AuthoritativeTotalXp;
            GameServer::Get()->SendToPlayers(broadcast, pSender);
        }
        else if (outcome.DuplicateRebateDelta < 0)
        {
            NotifyRfabProgressionEvent rebate{};
            rebate.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpRebate;
            rebate.EpochId = currentEpoch;
            rebate.SourceKey = pkt.SourceKey;
            rebate.XpDelta = outcome.DuplicateRebateDelta;
            rebate.AuthoritativeTotalXp = outcome.AuthoritativeTotalXp;
            pSender->Send(rebate);
        }
        break;
    }

    case SyncRfabProgressionEvent::EventKind::GlobalValueSync:
    {
        if (!m_ledger.UpdateGlobal(editorId, pkt.GlobalValue))
            return;

        NotifyRfabProgressionEvent broadcast{};
        broadcast.Kind = NotifyRfabProgressionEvent::EventKind::GlobalValueSync;
        broadcast.EpochId = currentEpoch;
        broadcast.EditorId = pkt.EditorId;
        broadcast.GlobalValue = pkt.GlobalValue;
        broadcast.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
        GameServer::Get()->SendToPlayers(broadcast, pSender);
        break;
    }

    case SyncRfabProgressionEvent::EventKind::AllyProtectionSync:
    {
        const bool enabled = (pkt.GlobalValue >= 0.5f);
        m_playerAllyProtection[pSender->GetId()] = enabled;

        NotifyRfabProgressionEvent broadcast{};
        broadcast.Kind = NotifyRfabProgressionEvent::EventKind::AllyProtectionSync;
        broadcast.EpochId = currentEpoch;
        broadcast.XpDelta = static_cast<int32_t>(pSender->GetId());
        broadcast.GlobalValue = enabled ? 1.0f : 0.0f;
        broadcast.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
        GameServer::Get()->SendToPlayers(broadcast, pSender);
        break;
    }
    }
}

void RfabProgressionService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    Player* pNewPlayer = acEvent.pPlayer;
    if (!pNewPlayer)
        return;

    const uint64_t currentEpoch = m_world.GetSaveSnapshotService().GetCurrentWorldEpoch();

    // Sync current WorldEpoch even if no books/globals have been recorded yet.
    NotifyRfabProgressionEvent epochMarker{};
    epochMarker.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpGrant;
    epochMarker.EpochId = currentEpoch;
    epochMarker.XpDelta = 0;
    epochMarker.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
    pNewPlayer->Send(epochMarker);

    // Sync all already-read XP books so the newly joined player adds them to XPBooksReaded immediately.
    for (const auto& bookId : m_ledger.GetReadBooks())
    {
        NotifyRfabProgressionEvent bookSync{};
        bookSync.Kind = NotifyRfabProgressionEvent::EventKind::BookRead;
        bookSync.EpochId = currentEpoch;
        bookSync.BookId = bookId;
        bookSync.XpDelta = 0; // Do not re-grant historical XP bursts; sync the FormList guard cleanly.
        bookSync.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
        pNewPlayer->Send(bookSync);
    }

    // Sync all authoritative RFAB TESGlobals (counters, boss phases, unlock triggers).
    for (const auto& [editorId, val] : m_ledger.GetSyncedGlobals())
    {
        NotifyRfabProgressionEvent globalSync{};
        globalSync.Kind = NotifyRfabProgressionEvent::EventKind::GlobalValueSync;
        globalSync.EpochId = currentEpoch;
        globalSync.EditorId = String(editorId.c_str());
        globalSync.GlobalValue = val;
        globalSync.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
        pNewPlayer->Send(globalSync);
    }

    // Sync existing players' Co-Op Ally Protection («Единство в битве») states to the newly joined player.
    m_playerAllyProtection.try_emplace(pNewPlayer->GetId(), true);
    for (const auto& [pid, protEnabled] : m_playerAllyProtection)
    {
        if (pid == pNewPlayer->GetId())
            continue;

        NotifyRfabProgressionEvent allySync{};
        allySync.Kind = NotifyRfabProgressionEvent::EventKind::AllyProtectionSync;
        allySync.EpochId = currentEpoch;
        allySync.XpDelta = static_cast<int32_t>(pid);
        allySync.GlobalValue = protEnabled ? 1.0f : 0.0f;
        allySync.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
        pNewPlayer->Send(allySync);
    }
}

void RfabProgressionService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (acEvent.pPlayer)
        m_playerAllyProtection.erase(acEvent.pPlayer->GetId());
}
