#pragma once

#include <common/RfabModpackCompat.h>
#include <Games/Primitives.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct PartyJoinedEvent;
struct PartyLeftEvent;
struct NotifyRfabProgressionEvent;
struct Actor;
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
    [[nodiscard]] BGSListForm* GetNoAllyDamageAllowedSpellsList() noexcept;

    [[nodiscard]] bool IsAllyProtectionEnabled() const noexcept { return m_allyProtectionEnabled.load(std::memory_order_relaxed); }
    [[nodiscard]] bool IsLocalPlayerAllyProtectionHost() const noexcept;
    void SetLocalAllyProtectionEnabled(bool aEnabled, bool aShowHudNotification = false) noexcept;
    void ToggleLocalAllyProtection() noexcept;
    void NotifyPartyLeadershipChanged() noexcept;
    void RefreshCoopCombatSnapshot() noexcept;
    [[nodiscard]] Actor* FindBestStandingPlayerTargetFor(Actor* apAttacker) const noexcept;
    [[nodiscard]] RfabCompat::CoopAllyActionPolicy EvaluateCoopAction(
        Actor* apAttacker,
        Actor* apTarget,
        bool aIsBeneficialHealOrBuff) const noexcept;

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
    void OnPartyJoined(const PartyJoinedEvent& acEvent) noexcept;
    void OnPartyLeft(const PartyLeftEvent& acEvent) noexcept;
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyProgressionEvent(const NotifyRfabProgressionEvent& acMessage) noexcept;

    void PollReadXpBooks() noexcept;
    void PollSyncedGlobals() noexcept;
    void SelfHealFreeRoamMenuControls() noexcept;
    void SuppressSoloRfabAutosave() noexcept;
    void RestoreSoloRfabAutosave() noexcept;
    void ApplyAllyHitGameSetting(bool aProtectAllies) noexcept;
    void SendHostAllyProtectionRequest(bool aEnabled, bool aNotifyPeers) const noexcept;
    void PushAllyProtectionStateToOverlay() const noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    RfabCompat::RfabBindingsConfig m_bindings{};
    RfabCompat::RfabLiveXpTable m_liveXpTable{};

    TESGlobal* m_pCachedXpGlobal{nullptr};
    TESGlobal* m_pCachedXpMultiplierGlobal{nullptr};
    TESGlobal* m_pNoDeathHandlingGlobal{nullptr};
    BGSListForm* m_pXpBooksFormList{nullptr};
    std::atomic<BGSListForm*> m_pNoAllyDamageAllowedSpellsList{nullptr};
    mutable std::mutex m_allowedSpellsMutex{};
    bool m_scannedGlobalsOnce{false};
    float m_savedNoDeathHandlingValue{0.0f};
    bool m_hasSavedNoDeathHandlingValue{false};

    struct StandingPlayerSnapshotEntry
    {
        uint32_t FormId{0};
        uint32_t HandleBits{0};
        uint32_t WorldSpaceId{0};
        uint32_t CellId{0};
        NiPoint3 Position{};
    };

    struct CoopCombatSnapshot
    {
        bool Connected{false};
        bool InParty{false};
        bool AllyProtectionEnabled{true};
        std::unordered_map<uint32_t, uint32_t> FormIdToPlayerId{};
        std::unordered_set<uint32_t> PartyMemberIds{};
        std::vector<StandingPlayerSnapshotEntry> StandingPlayers{};
    };
    mutable std::mutex m_coopSnapshotMutex{};
    CoopCombatSnapshot m_coopCombatSnapshot{};

    std::atomic<bool> m_allyProtectionEnabled{true};
    uint32_t m_localPlayerId{0};
    bool m_hasLocalPlayerId{false};
    bool m_isServerHost{false};
    int32_t m_savedAllyHitNonCombatAllowed{3};
    int32_t m_savedAllyHitCombatAllowed{1000};
    bool m_hasSavedAllyHitSettings{false};

    std::filesystem::path m_autosaveIniPath{};
    std::string m_originalAutosaveIniContent{};
    bool m_autosaveSuppressed{false};

    double m_pollAccumulatorSec{0.0};
    double m_controlMapHealAccumulatorSec{0.0};
    uint64_t m_currentWorldEpoch{1};
    bool m_seededReadBooks{false};
    std::unordered_set<uint32_t> m_knownReadBookFormIds{};
    std::unordered_map<std::string, TESGlobal*> m_watchedGlobals{};
    std::unordered_map<std::string, float> m_lastGlobalValues{};

    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_progressionConnection;
};
