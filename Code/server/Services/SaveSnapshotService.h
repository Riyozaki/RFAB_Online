#pragma once

#include <Events/PacketEvent.h>
#include <Services/RfabProgressionService.h>
#include <Structs/GameId.h>
#include <Structs/GridCellCoords.h>
#include <Structs/Inventory.h>
#include <Structs/LockData.h>
#include <Structs/QuestLog.h>
#include <CoopCheckpointStore.h>
#include <CoopFlightDigest.h>
#include <common/CoopLootLedger.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <glm/vec3.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct World;
struct UpdateEvent;
struct PlayerLeaveEvent;
struct RequestCoordinatedSaveLoad;
struct CoordinatedSaveAck;

/**
 * Pure two-phase barrier + ring-buffered slot allocator + history stack for server checkpoints.
 * Shared directly by SaveSnapshotService and Catch2 verification suites.
 */
class SaveSnapshotCoordinator
{
public:
    static constexpr uint64_t kDefaultBarrierTimeoutMs = 8000;
    static constexpr size_t kDefaultRingSize = 3;
    static constexpr size_t kMaxSnapshotHistory = 8;

    struct PendingBarrier
    {
        uint64_t EpochId{};
        std::string SlotName{};
        bool IsAutoCheckpoint{false};
        uint64_t StartedAtMs{};
        std::unordered_set<uint32_t> ExpectedPlayerIds{};
        std::unordered_set<uint32_t> AckedPlayerIds{};
        bool Aborted{false};
        std::string AbortReason{};
    };

    enum class AckResolution : uint8_t
    {
        Ignored = 0,
        WaitingForMorePlayers = 1,
        ReadyToCommit = 2,
        Aborted = 3,
    };

    explicit SaveSnapshotCoordinator(std::string aSessionTag = "S0001", size_t aRingSize = kDefaultRingSize)
        : m_sessionTag(std::move(aSessionTag))
        , m_ringSize(aRingSize > 0 ? aRingSize : kDefaultRingSize)
    {
    }

    [[nodiscard]] const std::string& GetSessionTag() const noexcept { return m_sessionTag; }
    [[nodiscard]] uint64_t GetNextEpochId() const noexcept { return m_nextEpochId; }
    [[nodiscard]] size_t GetManualRingCursor() const noexcept { return m_manualRingCursor; }
    [[nodiscard]] size_t GetAutoRingCursor() const noexcept { return m_autoRingCursor; }
    [[nodiscard]] bool HasPendingBarrier() const noexcept { return m_pending.has_value(); }
    [[nodiscard]] const std::optional<PendingBarrier>& GetPendingBarrier() const noexcept { return m_pending; }
    [[nodiscard]] uint64_t GetLastCommittedEpoch() const noexcept { return m_lastCommittedEpoch; }
    [[nodiscard]] const std::string& GetLastCommittedSlot() const noexcept { return m_lastCommittedSlot; }
    [[nodiscard]] uint64_t GetCurrentWorldEpoch() const noexcept { return m_worldGeneration; }
    [[nodiscard]] const CoopFlight::FlightRingBuffer<512>& GetFlightRing() const noexcept { return m_flightRing; }

    void RestorePersistedState(
        std::string aSessionTag,
        uint64_t aNextEpochId,
        uint64_t aWorldGeneration,
        size_t aManualRingCursor,
        size_t aAutoRingCursor,
        uint64_t aLastCommittedEpoch,
        std::string aLastCommittedSlot) noexcept
    {
        if (!aSessionTag.empty())
            m_sessionTag = std::move(aSessionTag);
        m_lastCommittedEpoch = aLastCommittedEpoch;
        m_nextEpochId = aNextEpochId < m_lastCommittedEpoch ? m_lastCommittedEpoch : aNextEpochId;
        m_worldGeneration = aWorldGeneration > 0 ? aWorldGeneration : 1;
        m_manualRingCursor = aManualRingCursor;
        m_autoRingCursor = aAutoRingCursor;
        m_lastCommittedSlot = std::move(aLastCommittedSlot);
        m_pending.reset();
    }

    void RecordFlightEvent(uint64_t aNowMs, CoopFlight::FlightEventKind aKind, uint32_t aSubjectId = 0, uint64_t aAuxValue = 0) noexcept
    {
        m_flightRing.Record(aNowMs, m_worldGeneration, aKind, aSubjectId, aAuxValue);
    }

    std::optional<PendingBarrier> BeginBarrier(
        const std::vector<uint32_t>& acConnectedPlayerIds,
        bool aIsAutoCheckpoint,
        uint64_t aNowMs)
    {
        if (m_pending.has_value() || acConnectedPlayerIds.empty())
            return std::nullopt;

        const uint64_t epochId = ++m_nextEpochId;
        const size_t slotIdx = aIsAutoCheckpoint
            ? (m_autoRingCursor++ % m_ringSize)
            : (m_manualRingCursor++ % m_ringSize);

        const std::string slotName = std::string("RFAB_MP_") + m_sessionTag +
            (aIsAutoCheckpoint ? "_Auto_" : "_Manual_") + std::to_string(slotIdx);

        PendingBarrier barrier{};
        barrier.EpochId = epochId;
        barrier.SlotName = slotName;
        barrier.IsAutoCheckpoint = aIsAutoCheckpoint;
        barrier.StartedAtMs = aNowMs;
        barrier.ExpectedPlayerIds.insert(acConnectedPlayerIds.begin(), acConnectedPlayerIds.end());

        m_pending = barrier;
        RecordFlightEvent(aNowMs, CoopFlight::FlightEventKind::kSavePrepared, 0, epochId);
        return barrier;
    }

    AckResolution RecordClientAck(uint32_t aPlayerId, uint64_t aEpochId, bool aSuccess, const std::string& acReason)
    {
        if (!m_pending.has_value() || m_pending->EpochId != aEpochId)
            return AckResolution::Ignored;

        if (!m_pending->ExpectedPlayerIds.contains(aPlayerId))
            return AckResolution::Ignored;

        if (!aSuccess)
        {
            m_pending->Aborted = true;
            m_pending->AbortReason = acReason.empty() ? "Client rejected save barrier" : acReason;
            return AckResolution::Aborted;
        }

        m_pending->AckedPlayerIds.insert(aPlayerId);
        if (m_pending->AckedPlayerIds.size() >= m_pending->ExpectedPlayerIds.size())
            return AckResolution::ReadyToCommit;

        return AckResolution::WaitingForMorePlayers;
    }

    AckResolution HandlePlayerDisconnected(uint32_t aPlayerId)
    {
        if (!m_pending.has_value())
            return AckResolution::Ignored;

        m_pending->ExpectedPlayerIds.erase(aPlayerId);
        m_pending->AckedPlayerIds.erase(aPlayerId);

        if (m_pending->ExpectedPlayerIds.empty())
        {
            m_pending->Aborted = true;
            m_pending->AbortReason = "All players disconnected during save barrier";
            return AckResolution::Aborted;
        }

        if (m_pending->AckedPlayerIds.size() >= m_pending->ExpectedPlayerIds.size())
            return AckResolution::ReadyToCommit;

        return AckResolution::WaitingForMorePlayers;
    }

    [[nodiscard]] bool HasBarrierTimedOut(uint64_t aNowMs, uint64_t aTimeoutMs = kDefaultBarrierTimeoutMs) const noexcept
    {
        if (!m_pending.has_value())
            return false;

        return aNowMs > m_pending->StartedAtMs && (aNowMs - m_pending->StartedAtMs) >= aTimeoutMs;
    }

    PendingBarrier ConsumeAndCommit(uint64_t aNowMs)
    {
        PendingBarrier finished = std::move(*m_pending);
        m_pending.reset();

        m_lastCommittedEpoch = finished.EpochId;
        m_lastCommittedSlot = finished.SlotName;
        if (m_worldGeneration < finished.EpochId)
            m_worldGeneration = finished.EpochId;
        if (finished.IsAutoCheckpoint)
            m_lastAutoSaveCommittedMs = aNowMs;

        RecordFlightEvent(aNowMs, CoopFlight::FlightEventKind::kSaveCommitted, 0, finished.EpochId);
        return finished;
    }

    PendingBarrier ConsumeAborted(uint64_t aNowMs = 0)
    {
        PendingBarrier aborted = std::move(*m_pending);
        m_pending.reset();
        RecordFlightEvent(aNowMs, CoopFlight::FlightEventKind::kSaveAborted, 0, aborted.EpochId);
        return aborted;
    }

    [[nodiscard]] bool ShouldTriggerPostCombatAutosave(
        uint64_t aNowMs,
        uint32_t aIntervalSec,
        bool aBossDefeated) const noexcept
    {
        if (m_pending.has_value())
            return false;

        // Boss kills always checkpoint immediately regardless of interval timer.
        if (aBossDefeated)
            return true;

        if (m_lastAutoSaveCommittedMs == 0)
            return true;

        const uint64_t intervalMs = static_cast<uint64_t>(aIntervalSec) * 1000ULL;
        return aNowMs >= m_lastAutoSaveCommittedMs && (aNowMs - m_lastAutoSaveCommittedMs) >= intervalMs;
    }

    void NotifyRolledBackTo(uint64_t aEpochId, const std::string& acSlotName, uint64_t aNowMs = 0)
    {
        m_pending.reset();
        m_lastCommittedEpoch = aEpochId;
        m_lastCommittedSlot = acSlotName;
        // World timeline epoch is strictly monotonic across rollbacks (cranebeaker/coop-anything Bug Class #12: no_world_epoch)
        // so any packet sent on the wire BEFORE a checkpoint load is deterministically recognized as stale and discarded.
        ++m_worldGeneration;
        if (m_nextEpochId < m_worldGeneration)
            m_nextEpochId = m_worldGeneration;
        RecordFlightEvent(aNowMs, CoopFlight::FlightEventKind::kWorldRollback, 0, aEpochId);
    }

private:
    std::string m_sessionTag;
    size_t m_ringSize;
    uint64_t m_nextEpochId{0};
    uint64_t m_worldGeneration{1};
    size_t m_manualRingCursor{0};
    size_t m_autoRingCursor{0};
    uint64_t m_lastCommittedEpoch{0};
    std::string m_lastCommittedSlot{};
    uint64_t m_lastAutoSaveCommittedMs{0};
    std::optional<PendingBarrier> m_pending{};
    CoopFlight::FlightRingBuffer<512> m_flightRing{};
};

class SaveSnapshotService
{
public:
    struct ActorStateRecord
    {
        GameId FormId{};
        GameId BaseId{};
        float CurrentHealth{};
        float MaxHealth{};
        bool IsDead{false};
        bool HasSpatialState{false};
        GameId CellId{};
        GameId WorldSpaceId{};
        GridCellCoords CenterCoords{};
        glm::vec3 Position{0.0f, 0.0f, 0.0f};
        glm::vec3 Rotation{0.0f, 0.0f, 0.0f};
    };

    struct PlayerStateRecord
    {
        bool HasValidTransform{false};
        GameId CellId{};
        GameId WorldSpaceId{};
        GridCellCoords CenterCoords{};
        glm::vec3 Position{0.0f, 0.0f, 0.0f};
        glm::vec3 Rotation{0.0f, 0.0f, 0.0f};
        bool HasInventory{false};
        Inventory CurrentInventory{};
    };

    struct CalendarStateRecord
    {
        int Hour{8};
        int Minute{0};
        float TimeScale{5.0f};
        int Day{17};
        int Month{7};
        float Year{201.0f};
    };

    struct ObjectStateRecord
    {
        GameId Id{};
        GameId CellId{};
        GameId WorldSpaceId{};
        GridCellCoords CenterCoords{};
        LockData CurrentLockData{};
        Inventory CurrentInventory{};
    };

    struct WorldSnapshot
    {
        uint64_t EpochId{};
        std::string SlotName{};
        bool IsAutoCheckpoint{false};
        uint64_t CreatedAtMs{};

        std::unordered_map<uint32_t, QuestLog> QuestLogsByPlayerId{};
        std::unordered_map<std::string, QuestLog> QuestLogsByIdentityKey{};
        QuestLog SharedPartyQuestLog{};
        std::unordered_map<uint32_t, PlayerStateRecord> PlayersById{};
        std::unordered_map<std::string, PlayerStateRecord> PlayersByIdentityKey{};
        std::unordered_map<uint32_t, ActorStateRecord> TrackedActors{};
        std::unordered_map<uint64_t, ActorStateRecord> PersistentActorsByKey{};
        std::vector<ObjectStateRecord> TrackedObjects{};
        CoopLootLedger::ContainerLootLedger::Snapshot LootLedger{};
        RfabProgressionLedger::Snapshot Progression{};
        CalendarStateRecord Calendar{};
    };

    SaveSnapshotService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

    void HydratePersistedWorldState() noexcept;

    bool TriggerCoordinatedSave(bool aIsAutoCheckpoint, Player* apRequester = nullptr) noexcept;
    bool TriggerRollbackToLatest(bool aIsPartyWipe) noexcept;
    bool TriggerRollbackToPrevious() noexcept;
    bool TriggerRollbackToSlot(const std::string& acSlotName, bool aIsPartyWipe) noexcept;

    void NotifyEncounterVictory(bool aBossDefeated) noexcept;

    bool SaveCheckpointsToDisk(const std::filesystem::path& acPath) const noexcept;
    bool LoadCheckpointsFromDisk(const std::filesystem::path& acPath) noexcept;
    [[nodiscard]] static std::filesystem::path GetDefaultCheckpointStorePath();

    [[nodiscard]] SaveSnapshotCoordinator& GetCoordinator() noexcept { return m_coordinator; }
    [[nodiscard]] const SaveSnapshotCoordinator& GetCoordinator() const noexcept { return m_coordinator; }
    [[nodiscard]] uint64_t GetCurrentWorldEpoch() const noexcept { return m_coordinator.GetCurrentWorldEpoch(); }
    [[nodiscard]] uint64_t GetLastCommittedEpoch() const noexcept { return m_coordinator.GetLastCommittedEpoch(); }
    [[nodiscard]] const std::string& GetLastCommittedSlot() const noexcept { return m_coordinator.GetLastCommittedSlot(); }
    [[nodiscard]] size_t GetSnapshotCount() const noexcept { return m_snapshots.size(); }
    [[nodiscard]] bool IsPlayerAuthorizedHost(Player* apPlayer) noexcept;
    [[nodiscard]] CoopFlight::BucketedStateDigest BuildLiveWorldDigest() const;

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnSaveLoadRequest(const PacketEvent<RequestCoordinatedSaveLoad>& acMessage) noexcept;
    void OnSaveAck(const PacketEvent<CoordinatedSaveAck>& acMessage) noexcept;

    void FinalizeCommit() noexcept;
    void AbortBarrier(const std::string& acReason) noexcept;
    [[nodiscard]] std::optional<PlayerStateRecord> CapturePlayerState(Player* apPlayer) const noexcept;
    void RefreshPlayerJoinBaselines() noexcept;
    [[nodiscard]] WorldSnapshot CaptureWorldSnapshot(uint64_t aEpochId, const std::string& acSlotName, bool aIsAuto, uint64_t aNowMs) const;
    void ApplyWorldSnapshot(const WorldSnapshot& acSnapshot, bool aIsPartyWipe) noexcept;

    World& m_world;
    SaveSnapshotCoordinator m_coordinator;
    std::deque<WorldSnapshot> m_snapshots{};
    std::unordered_map<uint32_t, PlayerStateRecord> m_playerJoinBaselines{};
    std::unordered_map<std::string, PlayerStateRecord> m_playerJoinBaselinesByIdentityKey{};
    bool m_deferredManualSaveAfterCombat{false};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_requestConnection;
    entt::scoped_connection m_ackConnection;
};
