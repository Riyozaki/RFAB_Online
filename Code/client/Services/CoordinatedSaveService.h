#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

struct World;
struct TransportService;
struct BGSSaveLoadManager;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct NotifyPrepareSave;
struct NotifyCommitSave;
struct NotifyExecuteLoad;

/**
 * Client-side participant in RFAB_Online's coordinated save & seamless in-session checkpoint rollback:
 * - Intercepts outer BGSSaveLoadManager save/load entry points while connected (never mid-save after
 *   opening .ess files on disk):
 *   * Silently blocks uncoordinated solo autosaves (including RFAB.dll's [RFAB] Autosave).
 *   * Converts F5 / Pause->Save into a party-wide two-phase checkpoint request.
 *   * Converts Party Leader's F9 / Pause->Load into a synchronized server+party checkpoint rollback.
 * - Detaches dynamic 0xFFxxxxxx remote clones & pooled TESNPCs for 2 calm frames before executing Save_Impl
 *   directly inside BGSSaveLoadManager::ProcessEvents (SE 34862), verifies the written "TESV_SAVEGAME"
 *   header on disk, restores remote actors after Save_Impl completes, and reports CoordinatedSaveAck.
 * - Executes BOTH Party Wipes and Manual Slot Loads (`F9`) via `WorldReloadBarrier` synchronized `.ess`
 *   world reloads while keeping `TransportService` socket, `PlayerId`, and `PartyService` open (zero
 *   `m_transport.Close()`), preserving 100% fidelity for Papyrus VM, `RFAB.dll`, `CustomSkills`,
 *   `StorageUtil`/`JContainers`, and SkyUI hotkeys. Non-destructive transform/health fallback is kept
 *   strictly as an emergency safety net if a newly joined peer has zero current-session `.ess` on disk.
 */
class CoordinatedSaveService
{
public:
    struct ScopedSaveLoadAllowance
    {
        ScopedSaveLoadAllowance() noexcept { s_allowDepth.fetch_add(1, std::memory_order_acq_rel); }
        ~ScopedSaveLoadAllowance() noexcept { s_allowDepth.fetch_sub(1, std::memory_order_acq_rel); }

        [[nodiscard]] static bool IsAllowed() noexcept
        {
            return s_allowDepth.load(std::memory_order_acquire) > 0;
        }

    private:
        inline static std::atomic<uint32_t> s_allowDepth{0};
    };

    CoordinatedSaveService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~CoordinatedSaveService() noexcept;

    TP_NOCOPYMOVE(CoordinatedSaveService);

    [[nodiscard]] bool HandleEngineSaveAttempt(const char* acpFileName) noexcept;
    [[nodiscard]] bool HandleEngineLoadAttempt(const char* acpFileName) noexcept;
    void PrepareEngineForWorldLoad(const char* acpFileName) noexcept;
    void NotifyEngineSaveCompleted(const char* acpFileName) noexcept;
    void NotifyEngineLoadCompleted(const char* acpFileName, bool aSucceeded) noexcept;
    void ProcessReadyDeferredSaveOrLoad(BGSSaveLoadManager* apMgr) noexcept;
    void TickWhilePaused(double aDeltaSeconds) noexcept;

    [[nodiscard]] bool IsHoldingWorldDiscoveryForHostBarrier() const noexcept
    {
        return m_liveSessionWorldReloadPending || m_liveSessionWorldReloadInFlight ||
               m_postReloadSyncPending || m_waitingForHostBarrierRelease;
    }

    [[nodiscard]] uint64_t GetLastCommittedEpoch() const noexcept { return m_lastCommittedEpoch; }
    [[nodiscard]] const std::string& GetLastCommittedSlot() const noexcept { return m_lastCommittedSlot; }

private:
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyPrepareSave(const NotifyPrepareSave& acMessage) noexcept;
    void OnNotifyCommitSave(const NotifyCommitSave& acMessage) noexcept;
    void OnNotifyExecuteLoad(const NotifyExecuteLoad& acMessage) noexcept;

    [[nodiscard]] bool IsLocalPlayerAuthorizedLeader() const noexcept;
    [[nodiscard]] bool CanSafelySaveNow(std::string& aOutReason) const noexcept;
    [[nodiscard]] bool IsLoadingMenuOpen() const noexcept;
    [[nodiscard]] bool IsBlockingInteractiveMenuOpen(std::string& aOutReason) const noexcept;
    void QueuePurgedAuthorizedSave(const std::string& acSlotName, uint64_t aEpochId, bool aSendServerAck) noexcept;
    void ApplyInSessionRollback(const NotifyExecuteLoad& acMessage) noexcept;
    void SendReloadBarrierReadyAck(uint64_t aEpochId, const std::string& acSlotName, bool aSuccess) noexcept;
    void CompleteLiveWorldReloadAfterEngineReady() noexcept;
    void ReleaseGuestWorldDiscoveryHold() noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    bool m_pendingPreJoinBackup{false};
    bool m_preJoinBackupWrittenThisSession{false};
    bool m_isExecutingPreJoinBackup{false};
    double m_preJoinBackupDelaySec{0.0};

    bool m_liveSessionWorldReloadPending{false};
    bool m_liveSessionWorldReloadInFlight{false};
    bool m_postReloadSyncPending{false};
    bool m_waitingForHostBarrierRelease{false};
    uint64_t m_activeReloadBarrierEpoch{0};
    std::string m_activeReloadBarrierSlot{};
    double m_hostBarrierHoldRemainingSec{0.0};

    bool m_autoReconnectPendingAfterLoad{false};
    bool m_wasAutoReconnecting{false};
    bool m_sawLoadingScreenForReconnect{false};
    double m_autoReconnectDelaySec{0.0};
    uint32_t m_autoReconnectAttemptsLeft{0};

    bool m_initialPartyCheckpointSent{false};
    uint8_t m_initialPartyCheckpointRetries{0};
    double m_partyCheckpointSettleSec{0.0};
    std::string m_preJoinBackupSlot{"RFAB_MP_PreJoin_Backup"};

    bool m_hasDeferredPrepareSave{false};
    uint64_t m_deferredPrepareEpochId{0};
    std::string m_deferredPrepareSlotName{};
    double m_deferredPrepareWaitSec{0.0};

    std::string m_pendingSaveSlot{};
    uint64_t m_pendingSaveEpochId{0};
    bool m_pendingSaveSendAck{false};
    uint8_t m_pendingSavePurgeFrames{0};
    uint8_t m_pendingSaveRetriesLeft{0};
    double m_pendingSaveRetryDelaySec{0.0};

    std::string m_authorizedPendingSaveSlot{};
    std::string m_authorizedPendingLoadSlot{};
    std::string m_deferredLoadSlot{};
    std::unique_ptr<NotifyExecuteLoad> m_deferredInSessionRollback{};
    double m_lastInterceptNoticeCooldownSec{0.0};

    uint64_t m_lastCommittedEpoch{0};
    std::string m_lastCommittedSlot{};
    std::string m_lastWrittenCoopSlot{};
    std::unordered_set<std::string> m_writtenSlotsInSession{};

    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_prepareSaveConnection;
    entt::scoped_connection m_commitSaveConnection;
    entt::scoped_connection m_executeLoadConnection;
};
