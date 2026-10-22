#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct NotifyPrepareSave;
struct NotifyCommitSave;
struct NotifyExecuteLoad;

/**
 * Client-side participant in RFAB_Online's two-phase coordinated save/load protocol:
 * - Intercepts BGSSaveLoadManager::Save_Impl (SE 34818) & Load_Impl (SE 34819) while connected:
 *   * Silently blocks uncoordinated solo autosaves (including RFAB.dll's [RFAB] Autosave).
 *   * Converts Party Leader's F5 / Pause->Save into a party-wide two-phase checkpoint request.
 *   * Converts Party Leader's F9 / Pause->Load into a synchronized server+party checkpoint rollback.
 *   * Prevents accidental non-leader F5/F9 desyncs with clear Russian HUD feedback.
 * - Executes server-prepared ring-buffer saves ("RFAB_MP_<Session>_Manual_<0..2>" / "_Auto_<0..2>")
 *   under ScopedSaveLoadAllowance and reports immediate CoordinatedSaveAck.
 * - Creates a local "RFAB_MP_PreJoin_Backup" slot upon connection and safely defers LoadNamed
 *   until any active cell transition ("Loading Menu") completes to prevent engine double-load CTDs.
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

    // Returns true if the native engine Save_Impl / Load_Impl should proceed immediately,
    // or false if intercepted/converted into a coordinated multiplayer action.
    [[nodiscard]] bool HandleEngineSaveAttempt(const char* acpFileName) noexcept;
    [[nodiscard]] bool HandleEngineLoadAttempt(const char* acpFileName) noexcept;

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
    bool ExecuteAuthorizedSave(const std::string& acSlotName) noexcept;
    bool ExecuteAuthorizedLoad(const std::string& acSlotName) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    bool m_pendingPreJoinBackup{false};
    std::string m_preJoinBackupSlot{"RFAB_MP_PreJoin_Backup"};
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
