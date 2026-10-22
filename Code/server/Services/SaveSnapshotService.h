#pragma once

#include <Events/PacketEvent.h>
#include <Services/RfabProgressionService.h>
#include <Structs/GameId.h>
#include <Structs/QuestLog.h>

#include <cstdint>
#include <deque>
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

    [[nodiscard]] bool HasPendingBarrier() const noexcept { return m_pending.has_value(); }
    [[nodiscard]] const std::optional<PendingBarrier>& GetPendingBarrier() const noexcept { return m_pending; }
    [[nodiscard]] uint64_t GetLastCommittedEpoch() const noexcept { return m_lastCommittedEpoch; }
    [[nodiscard]] const std::string& GetLastCommittedSlot() const noexcept { return m_lastCommittedSlot; }

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
        if (finished.IsAutoCheckpoint)
            m_lastAutoSaveCommittedMs = aNowMs;

        return finished;
    }

    PendingBarrier ConsumeAborted()
    {
        PendingBarrier aborted = std::move(*m_pending);
        m_pending.reset();
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

    void NotifyRolledBackTo(uint64_t aEpochId, const std::string& acSlotName)
    {
        m_pending.reset();
        m_lastCommittedEpoch = aEpochId;
        m_lastCommittedSlot = acSlotName;
    }

private:
    std::string m_sessionTag;
    size_t m_ringSize;
    uint64_t m_nextEpochId{0};
    size_t m_manualRingCursor{0};
    size_t m_autoRingCursor{0};
    uint64_t m_lastCommittedEpoch{0};
    std::string m_lastCommittedSlot{};
    uint64_t m_lastAutoSaveCommittedMs{0};
    std::optional<PendingBarrier> m_pending{};
};

class SaveSnapshotService
{
public:
    struct ActorStateRecord
    {
        float CurrentHealth{};
        float MaxHealth{};
        bool IsDead{false};
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

    struct WorldSnapshot
    {
        uint64_t EpochId{};
        std::string SlotName{};
        bool IsAutoCheckpoint{false};
        uint64_t CreatedAtMs{};

        std::unordered_map<uint32_t, QuestLog> QuestLogsByPlayerId{};
        QuestLog SharedPartyQuestLog{};
        std::unordered_map<uint32_t, ActorStateRecord> TrackedActors{};
        RfabProgressionLedger::Snapshot Progression{};
        CalendarStateRecord Calendar{};
    };

    SaveSnapshotService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

    bool TriggerCoordinatedSave(bool aIsAutoCheckpoint, Player* apRequester = nullptr) noexcept;
    bool TriggerRollbackToLatest(bool aIsPartyWipe) noexcept;
    bool TriggerRollbackToPrevious() noexcept;
    bool TriggerRollbackToSlot(const std::string& acSlotName, bool aIsPartyWipe) noexcept;

    void NotifyEncounterVictory(bool aBossDefeated) noexcept;

    [[nodiscard]] uint64_t GetLastCommittedEpoch() const noexcept { return m_coordinator.GetLastCommittedEpoch(); }
    [[nodiscard]] const std::string& GetLastCommittedSlot() const noexcept { return m_coordinator.GetLastCommittedSlot(); }
    [[nodiscard]] size_t GetSnapshotCount() const noexcept { return m_snapshots.size(); }
    [[nodiscard]] bool IsPlayerAuthorizedHost(Player* apPlayer) noexcept;

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnSaveLoadRequest(const PacketEvent<RequestCoordinatedSaveLoad>& acMessage) noexcept;
    void OnSaveAck(const PacketEvent<CoordinatedSaveAck>& acMessage) noexcept;

    void FinalizeCommit() noexcept;
    void AbortBarrier(const std::string& acReason) noexcept;
    [[nodiscard]] WorldSnapshot CaptureWorldSnapshot(uint64_t aEpochId, const std::string& acSlotName, bool aIsAuto, uint64_t aNowMs) const;
    void ApplyWorldSnapshot(const WorldSnapshot& acSnapshot, bool aIsPartyWipe) noexcept;

    World& m_world;
    SaveSnapshotCoordinator m_coordinator;
    std::deque<WorldSnapshot> m_snapshots{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_requestConnection;
    entt::scoped_connection m_ackConnection;
};
