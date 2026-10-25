#pragma once

#include <atomic>
#include <cstdint>
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
 * - Executes Party Wipes & pre-checkpoint rollbacks seamlessly IN-SESSION (SkyMP / MMO model) without
 *   calling m_transport.Close(), reloading .ess files in combat, or touching stale saves from older sessions.
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
    ~CoordinatedSaveService() = default;

    TP_NOCOPYMOVE(CoordinatedSaveService);

    [[nodiscard]] bool HandleEngineSaveAttempt(const char* acpFileName) noexcept;
    [[nodiscard]] bool HandleEngineLoadAttempt(const char* acpFileName) noexcept;
    void PrepareEngineForWorldLoad(const char* acpFileName) noexcept;
    void NotifyEngineSaveCompleted(const char* acpFileName) noexcept;
    void NotifyEngineLoadCompleted(const char* acpFileName, bool aSucceeded) noexcept;
    void ProcessReadyDeferredSaveOrLoad(BGSSaveLoadManager* apMgr) noexcept;

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

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    bool m_pendingPreJoinBackup{false};
    bool m_preJoinBackupWrittenThisSession{false};
    bool m_isExecutingPreJoinBackup{false};
    double m_preJoinBackupDelaySec{0.0};

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

    std::string m_authorizedPendingSaveSlot{};
    std::string m_authorizedPendingLoadSlot{};
    std::string m_deferredLoadSlot{};
    double m_lastInterceptNoticeCooldownSec{0.0};

    uint64_t m_lastCommittedEpoch{0};
    std::string m_lastCommittedSlot{};
    std::unordered_set<std::string> m_writtenSlotsInSession{};

    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_prepareSaveConnection;
    entt::scoped_connection m_commitSaveConnection;
    entt::scoped_connection m_executeLoadConnection;
};
