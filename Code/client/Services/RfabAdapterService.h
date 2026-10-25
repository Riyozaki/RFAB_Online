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
struct NotifyPlayerJoined;
struct NotifyPlayerLeft;
struct NotifyPlayerLevel;
struct NotifyPlayerCellChanged;
struct NotifyPlayerHealthUpdate;
struct NotifyPlayerList;
struct Actor;
struct TESForm;
struct TESGlobal;
struct BGSListForm;
struct Mod;

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
    [[nodiscard]] Mod* ResolveRfabPluginMod() const noexcept;
    [[nodiscard]] TESForm* ResolveFormByEditorIdOrBaseId(const std::string& acEditorId, uint32_t aFallbackBaseId = 0) const noexcept;
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
    [[nodiscard]] bool IsBossActor(Actor* apActor) const noexcept;
    void BroadcastArenaBarrierUnlock(const GameId& acObjectId) const noexcept;
    void TriggerContextualTacticalPing(bool aForceDanger, const std::string& acCustomLabel = "") noexcept;
    void TriggerExplicitTacticalPing(RfabCompat::TacticalPingKind aKind, const std::string& acCustomLabel = "") noexcept;
    void ClearLocalTacticalPings() noexcept;
    void ExecuteTacticalChatCommand(const RfabCompat::TacticalChatCommand& acCmd) noexcept;
    void SetTacticalHudMode(RfabCompat::TacticalHudMode aMode, bool aShowNotice = true) noexcept;
    [[nodiscard]] RfabCompat::TacticalHudMode GetTacticalHudMode() const noexcept { return m_tacticalHudMode; }
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
    void OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept;
    void OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept;
    void OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept;
    void OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) noexcept;
    void OnPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept;
    void OnPlayerList(const NotifyPlayerList& acMessage) noexcept;
    void OnDrawTacticalHud() noexcept;

    void PollReadXpBooks() noexcept;
    void PollSyncedGlobals() noexcept;
    void PollBossEncounterState() noexcept;
    void UpdateTacticalPingsAndInput(double aDeltaSec) noexcept;
    void UpsertActiveTacticalPing(const RfabCompat::TacticalPingPayload& acPayload) noexcept;
    void RefreshTacticalHudSnapshot() noexcept;
    void ClearTacticalHudSnapshot() noexcept;
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
    std::unordered_map<uint32_t, bool> m_lastBossGhostByServerId{};

    struct RemotePeerHudState
    {
        uint32_t PlayerId{0};
        std::string Username{};
        uint16_t Level{1};
        float HealthPercent{100.0f};
        std::string CellName{};
    };

    struct ActiveTacticalPing
    {
        RfabCompat::TacticalPingPayload Payload{};
        double RemainingSec{12.0};
        double TotalDurationSec{12.0};
    };

    std::unordered_map<uint32_t, RemotePeerHudState> m_partyHudPeers{};
    std::vector<ActiveTacticalPing> m_activeTacticalPings{};
    RfabCompat::TacticalHudMode m_tacticalHudMode{RfabCompat::TacticalHudMode::Full};
    double m_pingCooldownRemainingSec{0.0};
    bool m_pingKeyWasDown{false};
    bool m_hudKeyWasDown{false};

    struct DownedBeaconSnapshotEntry
    {
        std::string AllyName{};
        float DistMeters{0.0f};
        float NormScreenX{0.5f};
        float NormScreenY{0.5f};
        float NormScreenZ{1.0f};
        bool BeingRevived{false};
    };

    struct TacticalPingSnapshotEntry
    {
        RfabCompat::TacticalPingKind Kind{RfabCompat::TacticalPingKind::MoveHere};
        std::string TargetLabel{};
        float DistMeters{0.0f};
        float NormScreenX{0.5f};
        float NormScreenY{0.5f};
        float NormScreenZ{1.0f};
        double RemainingSec{0.0};
        double TotalDurationSec{12.0};
    };

    struct BossHudSnapshotEntry
    {
        bool Active{false};
        std::string Name{};
        std::string TargetPlayerName{};
        uint16_t Level{1};
        float CurHp{0.0f};
        float MaxHp{1.0f};
        float HpRatio{1.0f};
        bool IsEtherealPhase{false};
    };

    struct PartyMemberHudSnapshotEntry
    {
        std::string Name{};
        std::string CellName{};
        uint16_t Level{1};
        bool IsLeader{false};
        bool IsDowned{false};
        bool IsBeingRevived{false};
        bool IsGraceActive{false};
        bool IsInCombat{false};
        bool IsLoaded3D{false};
        float DistMeters{0.0f};
        float HpPct{100.0f};
        float MpPct{100.0f};
        float SpPct{100.0f};
    };

    struct TacticalHudRenderSnapshot
    {
        bool Visible{false};
        RfabCompat::TacticalHudMode Mode{RfabCompat::TacticalHudMode::Full};
        bool AllyProtectionEnabled{true};
        bool IsChannelingRevive{false};
        float ReviveChannelProgress{0.0f};
        bool IsLocalPlayerDowned{false};
        bool IsLocalPlayerBeingRevived{false};
        bool IsLocalPlayerInvulnerable{false};
        double GraceRemainingSec{0.0};
        std::vector<DownedBeaconSnapshotEntry> DownedBeacons{};
        std::vector<TacticalPingSnapshotEntry> ActivePings{};
        BossHudSnapshotEntry Boss{};
        std::vector<PartyMemberHudSnapshotEntry> PartyRows{};
    };

    mutable std::mutex m_tacticalHudSnapshotMutex{};
    TacticalHudRenderSnapshot m_tacticalHudSnapshot{};

    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_progressionConnection;
    entt::scoped_connection m_playerJoinedConnection;
    entt::scoped_connection m_playerLeftConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_playerCellChangedConnection;
    entt::scoped_connection m_playerHealthUpdateConnection;
    entt::scoped_connection m_playerListConnection;
    entt::scoped_connection m_drawImGuiConnection;
};
