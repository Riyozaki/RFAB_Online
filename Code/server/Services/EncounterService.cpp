#include <Services/EncounterService.h>

#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <GameServer.h>
#include <Messages/CombatEncounterStateReport.h>
#include <Messages/NotifyPlayerDownedState.h>
#include <Messages/NotifyReviveState.h>
#include <Messages/ReviveActionRequest.h>
#include <Messages/SyncPlayerDownedState.h>
#include <Services/SaveSnapshotService.h>
#include <Setting.h>
#include <World.h>

#include <chrono>

namespace
{
Console::Setting fReviveDurationSec{"Gameplay:fReviveDurationSec", "Seconds required to revive a downed ally with E", 4.0f};
Console::Setting fReviveHealthFraction{"Gameplay:fReviveHealthFraction", "Fraction of MaxHP restored upon revive (0.05..1.0)", 0.30f};
Console::Setting fPostReviveGraceSec{"Gameplay:fPostReviveGraceSec", "Post-revive invulnerability grace window in seconds", 3.0f};
Console::Setting fCombatLeashRadiusMeters{"Gameplay:fCombatLeashRadiusMeters", "Max distance (m) from fallen allies for honest encounter completion", 120.0f};

uint64_t NowEncounterMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

EncounterService::EncounterService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&EncounterService::OnUpdate>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&EncounterService::OnPlayerLeave>(this))
    , m_downedStateConnection(aDispatcher.sink<PacketEvent<SyncPlayerDownedState>>().connect<&EncounterService::OnSyncPlayerDownedState>(this))
    , m_reviveRequestConnection(aDispatcher.sink<PacketEvent<ReviveActionRequest>>().connect<&EncounterService::OnReviveActionRequest>(this))
    , m_combatReportConnection(aDispatcher.sink<PacketEvent<CombatEncounterStateReport>>().connect<&EncounterService::OnCombatEncounterStateReport>(this))
{
    EncounterCoordinator::Config cfg{};
    cfg.ReviveDurationSec = std::clamp(fReviveDurationSec.as_float(), 0.5f, 30.0f);
    cfg.ReviveHealthFraction = std::clamp(fReviveHealthFraction.as_float(), 0.05f, 1.0f);
    cfg.PostReviveGraceSec = std::clamp(fPostReviveGraceSec.as_float(), 0.0f, 15.0f);
    cfg.LeashRadiusMeters = std::clamp(fCombatLeashRadiusMeters.as_float(), 15.0f, 500.0f);
    m_coordinator.SetConfig(cfg);
}

void EncounterService::ResetAllStatesAfterRollback() noexcept
{
    m_coordinator.ResetAll();
}

void EncounterService::BroadcastPlayerDownedState(Player* apPlayer, bool aIsDowned, float aX, float aY, float aZ) noexcept
{
    if (!apPlayer)
        return;

    NotifyPlayerDownedState notify{};
    notify.PlayerId = apPlayer->GetId();
    notify.CharacterServerId = apPlayer->GetCharacter().has_value() ? World::ToInteger(*apPlayer->GetCharacter()) : 0;
    notify.IsDowned = aIsDowned;
    notify.PositionX = aX;
    notify.PositionY = aY;
    notify.PositionZ = aZ;

    GameServer::Get()->SendToPlayers(notify, apPlayer);
}

void EncounterService::OnSyncPlayerDownedState(const PacketEvent<SyncPlayerDownedState>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const auto& pkt = acMessage.Packet;
    m_coordinator.SetPlayerDowned(
        pSender->GetId(),
        pkt.IsDowned,
        pkt.PositionX,
        pkt.PositionY,
        pkt.PositionZ,
        pkt.NearbyHostileCount,
        NowEncounterMs());

    BroadcastPlayerDownedState(pSender, pkt.IsDowned, pkt.PositionX, pkt.PositionY, pkt.PositionZ);
}

void EncounterService::OnReviveActionRequest(const PacketEvent<ReviveActionRequest>& acMessage) noexcept
{
    Player* pReviver = acMessage.pPlayer;
    if (!pReviver)
        return;

    const auto& pkt = acMessage.Packet;
    const auto& cfg = m_coordinator.GetConfig();

    if (pkt.Action == ReviveActionRequest::ActionType::StartChannel)
    {
        if (m_coordinator.TryStartAllyRevive(pReviver->GetId(), pkt.TargetPlayerId, NowEncounterMs()))
        {
            NotifyReviveState started{};
            started.Status = NotifyReviveState::ReviveStatus::ChannelStarted;
            started.TargetPlayerId = pkt.TargetPlayerId;
            started.ReviverPlayerId = pReviver->GetId();
            started.DurationSec = cfg.ReviveDurationSec;
            started.HealthFraction = cfg.ReviveHealthFraction;
            started.GraceSec = cfg.PostReviveGraceSec;
            GameServer::Get()->SendToPlayers(started, nullptr);
        }
    }
    else
    {
        if (m_coordinator.CancelAllyRevive(pReviver->GetId(), pkt.TargetPlayerId))
        {
            NotifyReviveState cancelled{};
            cancelled.Status = NotifyReviveState::ReviveStatus::ChannelCancelled;
            cancelled.TargetPlayerId = pkt.TargetPlayerId;
            cancelled.ReviverPlayerId = pReviver->GetId();
            GameServer::Get()->SendToPlayers(cancelled, nullptr);
        }
    }
}

void EncounterService::OnCombatEncounterStateReport(const PacketEvent<CombatEncounterStateReport>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const auto& pkt = acMessage.Packet;
    m_coordinator.UpdatePlayerCombatReport(
        pSender->GetId(),
        pkt.InCombat,
        pkt.NearbyHostileCount,
        pkt.NearestHostileDistanceMeters,
        pkt.BossDefeatedInEncounter,
        pkt.PositionX,
        pkt.PositionY,
        pkt.PositionZ,
        NowEncounterMs());
}

void EncounterService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (!acEvent.pPlayer)
        return;

    m_coordinator.RemovePlayer(acEvent.pPlayer->GetId());
}

void EncounterService::OnUpdate(const UpdateEvent&) noexcept
{
    EncounterCoordinator::Config cfg = m_coordinator.GetConfig();
    cfg.ReviveDurationSec = std::clamp(fReviveDurationSec.as_float(), 0.5f, 30.0f);
    cfg.ReviveHealthFraction = std::clamp(fReviveHealthFraction.as_float(), 0.05f, 1.0f);
    cfg.PostReviveGraceSec = std::clamp(fPostReviveGraceSec.as_float(), 0.0f, 15.0f);
    cfg.LeashRadiusMeters = std::clamp(fCombatLeashRadiusMeters.as_float(), 15.0f, 500.0f);
    m_coordinator.SetConfig(cfg);

    const auto events = m_coordinator.Tick(NowEncounterMs());

    for (const auto& [reviverId, targetId] : events.CancelledAllyRevives)
    {
        NotifyReviveState msg{};
        msg.Status = NotifyReviveState::ReviveStatus::ChannelCancelled;
        msg.TargetPlayerId = targetId;
        msg.ReviverPlayerId = reviverId;
        GameServer::Get()->SendToPlayers(msg, nullptr);
    }

    for (const auto& [reviverId, targetId] : events.CompletedAllyRevives)
    {
        NotifyReviveState msg{};
        msg.Status = NotifyReviveState::ReviveStatus::RevivedByAlly;
        msg.TargetPlayerId = targetId;
        msg.ReviverPlayerId = reviverId;
        msg.HealthFraction = cfg.ReviveHealthFraction;
        msg.GraceSec = cfg.PostReviveGraceSec;
        GameServer::Get()->SendToPlayers(msg, nullptr);

        if (Player* pRevived = m_world.GetPlayerManager().GetById(targetId))
            BroadcastPlayerDownedState(pRevived, false, 0.f, 0.f, 0.f);
    }

    for (const uint32_t targetId : events.PostCombatRevivedPlayers)
    {
        NotifyReviveState msg{};
        msg.Status = NotifyReviveState::ReviveStatus::RevivedPostCombat;
        msg.TargetPlayerId = targetId;
        msg.ReviverPlayerId = 0;
        msg.HealthFraction = cfg.ReviveHealthFraction;
        msg.GraceSec = cfg.PostReviveGraceSec;
        GameServer::Get()->SendToPlayers(msg, nullptr);

        if (Player* pRevived = m_world.GetPlayerManager().GetById(targetId))
            BroadcastPlayerDownedState(pRevived, false, 0.f, 0.f, 0.f);
    }

    if (events.PartyWipeWarningStarted)
    {
        NotifyReviveState wipeWarn{};
        wipeWarn.Status = NotifyReviveState::ReviveStatus::PartyWipeImminent;
        wipeWarn.DurationSec = cfg.PartyWipeDelaySec;
        GameServer::Get()->SendToPlayers(wipeWarn, nullptr);
    }

    if (events.PartyWipeTriggered)
    {
        m_world.GetSaveSnapshotService().TriggerRollbackToLatest(true);
        return;
    }

    if (events.EncounterVictoryResolved)
    {
        m_world.GetSaveSnapshotService().NotifyEncounterVictory(events.BossDefeatedInResolvedEncounter);
    }
}
