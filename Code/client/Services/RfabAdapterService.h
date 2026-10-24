#pragma once

#include <common/RfabModpackCompat.h>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct NotifyRfabProgressionEvent;
struct TESGlobal;
struct BGSListForm;

/**
 * Runtime adapter between RFAB_Online and the Requiem for a Balance (drake1ch/RFAB) modpack:
 * - Zero hardcoded numeric FormIDs: resolves globals & FormLists via PO3 LookupByEditorID (SE 14464)
 *   plus live TESDataHandler (ModManager) array scanning and Data/SKSE/Plugins/RFAB_Online_Bindings.json.
 * - Reads RFAB's live StorageUtil tables (Experience.ini, Quests.ini, RFAB_XP_Settings.json).
 * - Syncs XPBooksReaded (BGSListForm::AddForm, SE 20470), deduplicated XP grants/rebates, and shared RFAB TESGlobals.
 * - Enforces REQ_Combat_NoDeathHandling = 1 and suppresses [RFAB] Autosave.ini while connected,
 *   restoring exact solo state upon disconnect or exit.
 */
class RfabAdapterService
{
public:
    RfabAdapterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~RfabAdapterService() noexcept;

    TP_NOCOPYMOVE(RfabAdapterService);

    [[nodiscard]] TESGlobal* GetRfabXpGlobal() noexcept;
    [[nodiscard]] float GetLocalXpMultiplier() noexcept;
    [[nodiscard]] TESGlobal* ResolveGlobalByEditorId(const std::string& acEditorId, uint32_t aFallbackBaseId = 0) noexcept;
    [[nodiscard]] BGSListForm* ResolveFormListByEditorId(const std::string& acEditorId) noexcept;

    [[nodiscard]] static int32_t ReadRfabXp(TESGlobal* apGlobal) noexcept;
    bool ApplyDualPoolExperienceDelta(int32_t aDelta, int32_t& aOutNewTotalXp) noexcept;

    [[nodiscard]] const RfabCompat::RfabBindingsConfig& GetBindings() const noexcept { return m_bindings; }
    [[nodiscard]] const RfabCompat::RfabLiveXpTable& GetLiveXpTable() const noexcept { return m_liveXpTable; }
    [[nodiscard]] uint64_t GetCurrentWorldEpoch() const noexcept { return m_currentWorldEpoch; }

    void RefreshRuntimeBindings() noexcept;
    void NotifySaveLoadReset() noexcept;

private:
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyProgressionEvent(const NotifyRfabProgressionEvent& acMessage) noexcept;

    void PollReadXpBooks() noexcept;
    void PollSyncedGlobals() noexcept;
    void SuppressSoloRfabAutosave() noexcept;
    void RestoreSoloRfabAutosave() noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    RfabCompat::RfabBindingsConfig m_bindings{};
    RfabCompat::RfabLiveXpTable m_liveXpTable{};

    TESGlobal* m_pCachedXpGlobal{nullptr};
    TESGlobal* m_pCachedXpMultiplierGlobal{nullptr};
    TESGlobal* m_pNoDeathHandlingGlobal{nullptr};
    BGSListForm* m_pXpBooksFormList{nullptr};
    bool m_scannedGlobalsOnce{false};
    float m_savedNoDeathHandlingValue{0.0f};
    bool m_hasSavedNoDeathHandlingValue{false};

    std::filesystem::path m_autosaveIniPath{};
    std::string m_originalAutosaveIniContent{};
    bool m_autosaveSuppressed{false};

    double m_pollAccumulatorSec{0.0};
    uint64_t m_currentWorldEpoch{1};
    bool m_seededReadBooks{false};
    std::unordered_set<uint32_t> m_knownReadBookFormIds{};
    std::unordered_map<std::string, TESGlobal*> m_watchedGlobals{};
    std::unordered_map<std::string, float> m_lastGlobalValues{};

    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_progressionConnection;
};
