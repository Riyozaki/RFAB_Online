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
#include <Save/Barrier.h>

// The barrier logic moved to Code/common/Save/Barrier.h so the tests can run it
// without the server, the network stack or a clock. The old name still works.
using SaveSnapshotCoordinator = Save::Coordinator;

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
        // Runtime provenance only; never serialized. IDs read from disk cannot identify a player
        // in the current process. This applies to every loaded checkpoint, including older ones.
        bool LootClaimantsFromDisk{false};
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
    void BeginWorldReloadBarrier(uint64_t aEpochId, const std::string& acSlotName, uint64_t aNowMs) noexcept;
    void ReleaseHostReloadGateToGuests(uint64_t aEpochId, const std::string& acSlotName) noexcept;
    void SyncPostReloadStateToPlayer(Player* apPlayer) noexcept;

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
