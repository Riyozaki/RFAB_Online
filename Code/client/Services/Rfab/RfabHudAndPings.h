#pragma once

#include <common/RfabModpackCompat.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct World;
struct TransportService;
struct ImDrawList;
struct NotifyPlayerJoined;
struct NotifyPlayerLeft;
struct NotifyPlayerLevel;
struct NotifyPlayerCellChanged;
struct NotifyPlayerHealthUpdate;
struct NotifyPlayerList;
struct NotifyRfabProgressionEvent;
class RfabFormResolver;
class RfabCombatRules;

struct RfabDownedBeaconSnapshotEntry
{
    std::string AllyName{};
    float DistMeters{0.0f};
    float NormScreenX{0.5f};
    float NormScreenY{0.5f};
    float NormScreenZ{1.0f};
    bool BeingRevived{false};
};

struct RfabTacticalPingSnapshotEntry
{
    RfabCompat::TacticalPingKind Kind{RfabCompat::TacticalPingKind::Rally};
    std::string TargetLabel{};
    float DistMeters{0.0f};
    float NormScreenX{0.5f};
    float NormScreenY{0.5f};
    float NormScreenZ{1.0f};
    double RemainingSec{0.0};
    double TotalDurationSec{12.0};
};

struct RfabBossHudSnapshotEntry
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

struct RfabPartyMemberHudSnapshotEntry
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

struct RfabTacticalHudRenderSnapshot
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
    std::vector<RfabDownedBeaconSnapshotEntry> DownedBeacons{};
    std::vector<RfabTacticalPingSnapshotEntry> ActivePings{};
    RfabBossHudSnapshotEntry Boss{};
    std::vector<RfabPartyMemberHudSnapshotEntry> PartyRows{};
};

/**
 * Owns the co-op tactical ping system, party peer roster state, ControlMap self-healing,
 * Main-Game-Thread TacticalHudRenderSnapshot assembly, and D3D11 Render-Thread pure ImDrawList drawing.
 */
class RfabHudAndPings
{
public:
    RfabHudAndPings() noexcept = default;
    ~RfabHudAndPings() noexcept = default;

    TP_NOCOPYMOVE(RfabHudAndPings);

    void ApplyDefaultModeFromBindings(const RfabCompat::RfabBindingsConfig& acBindings) noexcept;
    void SetMode(RfabCompat::TacticalHudMode aMode, bool aShowNotice) noexcept;
    [[nodiscard]] RfabCompat::TacticalHudMode GetMode() const noexcept { return m_tacticalHudMode; }

    void ResetOnDisconnect() noexcept;

    void OnPlayerJoined(World& aWorld, const NotifyPlayerJoined& acMessage) noexcept;
    void OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept;
    void OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept;
    void OnPlayerCellChanged(World& aWorld, const NotifyPlayerCellChanged& acMessage) noexcept;
    void OnPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept;
    void OnPlayerList(const NotifyPlayerList& acMessage) noexcept;

    void UpsertActivePing(
        const RfabCompat::TacticalPingPayload& acPayload,
        const RfabCompat::RfabBindingsConfig& acBindings) noexcept;

    void ApplyIncomingPingSyncEvent(
        const NotifyRfabProgressionEvent& acMessage,
        const RfabCompat::RfabBindingsConfig& acBindings) noexcept;

    void ClearLocalPings(
        TransportService& aTransport,
        uint32_t aLocalPlayerId,
        uint64_t aWorldEpoch) noexcept;

    void TriggerContextualPing(
        World& aWorld,
        TransportService& aTransport,
        const RfabFormResolver& acResolver,
        const RfabCombatRules& acCombatRules,
        uint32_t aLocalPlayerId,
        uint64_t aWorldEpoch,
        bool aForceDanger,
        const std::string& acCustomLabel) noexcept;

    void TriggerExplicitPing(
        World& aWorld,
        TransportService& aTransport,
        const RfabFormResolver& acResolver,
        const RfabCombatRules& acCombatRules,
        uint32_t aLocalPlayerId,
        uint64_t aWorldEpoch,
        RfabCompat::TacticalPingKind aKind,
        const std::string& acCustomLabel) noexcept;

    bool ExecuteChatCommand(
        World& aWorld,
        TransportService& aTransport,
        const RfabFormResolver& acResolver,
        const RfabCombatRules& acCombatRules,
        uint32_t aLocalPlayerId,
        uint64_t aWorldEpoch,
        const RfabCompat::TacticalChatCommand& acCmd,
        bool& aOutShouldClearSnapshot) noexcept;

    bool UpdatePingsAndInput(
        World& aWorld,
        TransportService& aTransport,
        const RfabFormResolver& acResolver,
        const RfabCombatRules& acCombatRules,
        uint32_t aLocalPlayerId,
        uint64_t aWorldEpoch,
        double aDeltaSec) noexcept;

    static void SelfHealFreeRoamMenuControls(World& aWorld) noexcept;

    void BuildHudSnapshot(
        World& aWorld,
        TransportService& aTransport,
        const RfabFormResolver& acResolver,
        const RfabCombatRules& acCombatRules,
        uint32_t aLocalPlayerId,
        bool aAllyProtectionEnabled,
        bool aLocalIsAllyProtectionHost,
        RfabTacticalHudRenderSnapshot& aOutSnapshot) const noexcept;

    // Render-thread-safe ImDrawList helpers (zero engine/UI/registry calls)
    static void DrawDownedBeacon(
        ImDrawList* apDrawList,
        const RfabDownedBeaconSnapshotEntry& acBeacon,
        const RfabCompat::ScreenMarkerProjection& acProj) noexcept;

    static void DrawTacticalPing(
        ImDrawList* apDrawList,
        const RfabTacticalPingSnapshotEntry& acPing,
        const RfabCompat::ScreenMarkerProjection& acProj) noexcept;

    static void DrawOverlayPanels(
        ImDrawList* apDrawList,
        const RfabTacticalHudRenderSnapshot& acSnapshot,
        float aViewW,
        float aViewH) noexcept;

private:
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
};
