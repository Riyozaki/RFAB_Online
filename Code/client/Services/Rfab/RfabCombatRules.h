#pragma once

#include <common/RfabModpackCompat.h>
#include <Games/Primitives.h>
#include <Structs/GameId.h>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct World;
struct TransportService;
struct Actor;
struct NotifyRfabProgressionEvent;
class RfabFormResolver;

struct RfabStandingPlayerSnapshotEntry
{
    uint32_t FormId{0};
    uint32_t HandleBits{0};
    uint32_t WorldSpaceId{0};
    uint32_t CellId{0};
    NiPoint3 Position{};
};

struct RfabCombatSnapshot
{
    bool Connected{false};
    bool InParty{false};
    bool AllyProtectionEnabled{true};
    std::unordered_map<uint32_t, uint32_t> FormIdToPlayerId{};
    std::unordered_set<uint32_t> PartyMemberIds{};
    std::vector<RfabStandingPlayerSnapshotEntry> StandingPlayers{};
    std::vector<RfabStandingPlayerSnapshotEntry> StandingSummons{};
};

/**
 * Encapsulates RFAB co-op combat rules:
 * - Ally protection GMSTs (iAllyHitNonCombatAllowed / iAllyHitCombatAllowed)
 * - Main-thread CoopCombatSnapshot assembly and worker-thread-safe action/aggro evaluation
 * - RFAB boss detection (BossList.json + EditorID/ESP index), boss ExtraGhost ethereal sync,
 *   and co-op arena barrier door/lock synchronization.
 */
class RfabCombatRules
{
public:
    RfabCombatRules() noexcept = default;
    ~RfabCombatRules() noexcept = default;

    TP_NOCOPYMOVE(RfabCombatRules);

    void ApplyAllyHitGameSetting(bool aProtectAllies) noexcept;

    static void BuildCombatSnapshot(
        World& aWorld,
        TransportService& aTransport,
        bool aAllyProtectionEnabled,
        RfabCombatSnapshot& aOutSnapshot) noexcept;

    [[nodiscard]] static GamePtr<Actor> SelectBestStandingTarget(
        const RfabCombatSnapshot& acSnapshot,
        Actor* apAttacker) noexcept;

    [[nodiscard]] static RfabCompat::CoopAllyActionPolicy EvaluateSnapshotAction(
        const RfabCombatSnapshot& acSnapshot,
        Actor* apAttacker,
        Actor* apTarget,
        bool aIsBeneficialHealOrBuff) noexcept;

    static void CollectPartyPeerActorFormIds(
        const RfabCombatSnapshot& acSnapshot,
        TiltedPhoques::Vector<uint32_t>& aOutFormIds) noexcept;

    [[nodiscard]] bool IsBossActor(Actor* apActor, const RfabFormResolver& acResolver) const noexcept;
    [[nodiscard]] bool IsBossKnownEthereal(uint32_t aServerId) const noexcept;

    static void BroadcastArenaBarrierUnlock(
        TransportService& aTransport,
        const GameId& acObjectId,
        uint64_t aWorldEpoch) noexcept;

    void PollBossEncounterState(
        World& aWorld,
        TransportService& aTransport,
        const RfabFormResolver& acResolver,
        uint64_t aWorldEpoch) noexcept;

    void ApplyBossEncounterSyncEvent(
        World& aWorld,
        const NotifyRfabProgressionEvent& acMessage) noexcept;

    void ResetBossTracking() noexcept { m_lastBossGhostByServerId.clear(); }

private:
    int32_t m_savedAllyHitNonCombatAllowed{3};
    int32_t m_savedAllyHitCombatAllowed{1000};
    bool m_hasSavedAllyHitSettings{false};
    std::unordered_map<uint32_t, bool> m_lastBossGhostByServerId{};
};
