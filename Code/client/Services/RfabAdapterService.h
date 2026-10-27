#pragma once

#include <Services/Rfab/RfabCombatRules.h>
#include <Services/Rfab/RfabFormResolver.h>
#include <Services/Rfab/RfabHudAndPings.h>
#include <Services/Rfab/RfabXpAndBookSync.h>
#include <atomic>
#include <mutex>
#include <string>

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
 * Facade & cross-thread boundary coordinator for RFAB_Online's Requiem for a Balance integration.
 * Delegates domain logic to four dedicated single-responsibility modules:
 * - RfabFormResolver (PO3 EditorID + binary ESP index + RFAB_Online_Bindings.json)
 * - RfabXpAndBookSync (dual-pool XP, XPBooksReaded, watched TESGlobals, autosave & NoDeathHandling guards)
 * - RfabCombatRules (ally-hit GMSTs, CoopCombatSnapshot rules, boss detection & arena barriers)
 * - RfabHudAndPings (peer HUD roster, 3D tactical pings, ControlMap self-healing, D3D11 HUD snapshot rendering)
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
    [[nodiscard]] GamePtr<Actor> FindBestStandingPlayerTargetFor(Actor* apAttacker) const noexcept;
    void CollectPartyPeerActorFormIds(TiltedPhoques::Vector<uint32_t>& aOutFormIds) const noexcept;
    [[nodiscard]] bool IsBossActor(Actor* apActor) const noexcept;
    void BroadcastArenaBarrierUnlock(const GameId& acObjectId) const noexcept;
    void TriggerContextualTacticalPing(bool aForceDanger, const std::string& acCustomLabel = "") noexcept;
    void TriggerExplicitTacticalPing(RfabCompat::TacticalPingKind aKind, const std::string& acCustomLabel = "") noexcept;
    void ClearLocalTacticalPings() noexcept;
    void ExecuteTacticalChatCommand(const RfabCompat::TacticalChatCommand& acCmd) noexcept;
    void SetTacticalHudMode(RfabCompat::TacticalHudMode aMode, bool aShowNotice = true) noexcept;
    [[nodiscard]] RfabCompat::TacticalHudMode GetTacticalHudMode() const noexcept { return m_hudAndPings.GetMode(); }
    [[nodiscard]] RfabCompat::CoopAllyActionPolicy EvaluateCoopAction(
        Actor* apAttacker,
        Actor* apTarget,
        bool aIsBeneficialHealOrBuff) const noexcept;

    [[nodiscard]] static int32_t ReadRfabXp(TESGlobal* apGlobal) noexcept;
    bool ApplyDualPoolExperienceDelta(int32_t aDelta, int32_t& aOutNewTotalXp) noexcept;

    [[nodiscard]] const RfabCompat::RfabBindingsConfig& GetBindings() const noexcept { return m_formResolver.GetBindings(); }
    [[nodiscard]] const RfabCompat::RfabLiveXpTable& GetLiveXpTable() const noexcept { return m_formResolver.GetLiveXpTable(); }
    [[nodiscard]] uint64_t GetCurrentWorldEpoch() const noexcept { return m_xpAndBookSync.GetCurrentWorldEpoch(); }

    void RefreshRuntimeBindings() noexcept;
    void NotifySaveLoadReset() noexcept;

private:
    struct CoopCombatSnapshot : RfabCombatSnapshot
    {
    };

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
    void RefreshTacticalHudSnapshot() noexcept;
    void ClearTacticalHudSnapshot() noexcept;
    void SelfHealFreeRoamMenuControls() noexcept;
    void SendHostAllyProtectionRequest(bool aEnabled, bool aNotifyPeers) const noexcept;
    void PushAllyProtectionStateToOverlay() const noexcept;
    [[nodiscard]] uint32_t ResolveEffectiveLocalPlayerId() const noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    RfabFormResolver m_formResolver{};
    RfabXpAndBookSync m_xpAndBookSync{};
    RfabCombatRules m_combatRules{};
    RfabHudAndPings m_hudAndPings{};

    std::atomic<BGSListForm*> m_pNoAllyDamageAllowedSpellsList{nullptr};
    std::atomic<bool> m_allyProtectionEnabled{true};
    uint32_t m_localPlayerId{0};
    bool m_hasLocalPlayerId{false};
    bool m_isServerHost{false};
    double m_pollAccumulatorSec{0.0};
    double m_controlMapHealAccumulatorSec{0.0};

    mutable std::mutex m_coopSnapshotMutex{};
    CoopCombatSnapshot m_coopCombatSnapshot{};

    mutable std::mutex m_tacticalHudSnapshotMutex{};
    RfabTacticalHudRenderSnapshot m_tacticalHudSnapshot{};

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
