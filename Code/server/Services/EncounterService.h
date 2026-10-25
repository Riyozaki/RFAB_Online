#pragma once

#include <common/RfabModpackCompat.h>
#include <Events/PacketEvent.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

struct World;
struct UpdateEvent;
struct PlayerLeaveEvent;
struct SyncPlayerDownedState;
struct ReviveActionRequest;
struct CombatEncounterStateReport;

/**
 * Pure deterministic encounter, downed-on-knee, ally E-revive, strict RFAB combat leash,
 * post-combat victory auto-revive, and Total Party Wipe state machine.
 */
class EncounterCoordinator
{
public:
    struct Config
    {
        float ReviveDurationSec{4.0f};
        float ReviveMaxDistanceMeters{4.5f}; // 3.0m client prompt + 1.5m network jitter tolerance
        float ReviveHealthFraction{0.30f};
        float PostReviveGraceSec{3.0f};
        float LeashRadiusMeters{120.0f};
        float CombatClearHysteresisSec{4.5f};
        float PartyWipeDelaySec{3.0f};
    };

    struct PlayerCombatRecord
    {
        uint32_t PlayerId{};
        bool IsDowned{false};
        bool HadHostilesWhenDowned{false};
        bool ParticipatedInEncounter{false};
        bool InCombat{false};
        uint32_t NearbyHostileCount{0};
        float NearestHostileDistanceMeters{9999.0f};
        float PosX{};
        float PosY{};
        float PosZ{};
    };

    struct ActiveReviveChannel
    {
        uint32_t ReviverPlayerId{};
        uint32_t TargetPlayerId{};
        uint64_t StartedAtMs{};
        uint64_t CompleteAtMs{};
    };

    struct TickEvents
    {
        std::vector<std::pair<uint32_t, uint32_t>> CompletedAllyRevives{}; // (reviverId, targetId)
        std::vector<std::pair<uint32_t, uint32_t>> CancelledAllyRevives{}; // (reviverId, targetId)
        std::vector<uint32_t> PostCombatRevivedPlayers{};
        bool EncounterVictoryResolved{false};
        bool BossDefeatedInResolvedEncounter{false};
        bool PartyWipeWarningStarted{false};
        bool PartyWipeTriggered{false};
    };

    EncounterCoordinator() noexcept = default;
    explicit EncounterCoordinator(Config aConfig) noexcept
        : m_config(aConfig)
    {
    }

    void SetConfig(const Config& acConfig) noexcept { m_config = acConfig; }
    [[nodiscard]] const Config& GetConfig() const noexcept { return m_config; }

    void UpdatePlayerCombatReport(
        uint32_t aPlayerId,
        bool aInCombat,
        uint32_t aNearbyHostileCount,
        float aNearestHostileMeters,
        bool aBossDefeated,
        float aPosX,
        float aPosY,
        float aPosZ,
        uint64_t aNowMs)
    {
        auto& rec = m_players[aPlayerId];
        rec.PlayerId = aPlayerId;
        rec.InCombat = aInCombat;
        rec.NearbyHostileCount = aNearbyHostileCount;
        rec.NearestHostileDistanceMeters = aNearestHostileMeters;
        rec.PosX = aPosX;
        rec.PosY = aPosY;
        rec.PosZ = aPosZ;

        if (aBossDefeated)
            m_bossDefeatedInCurrentEncounter = true;

        if (aInCombat || aNearbyHostileCount > 0)
        {
            rec.ParticipatedInEncounter = true;
            m_encounterActive = true;
            m_combatClearSinceMs.reset();
        }
        else if (m_encounterActive)
        {
            if (IsWithinLeashOfAnyParticipant(rec))
                rec.ParticipatedInEncounter = true;
            if (!m_combatClearSinceMs.has_value())
                m_combatClearSinceMs = aNowMs;
        }
    }

    // Returns true if the player's downed state actually changed.
    bool SetPlayerDowned(
        uint32_t aPlayerId,
        bool aIsDowned,
        float aPosX,
        float aPosY,
        float aPosZ,
        uint32_t aNearbyHostiles,
        uint64_t aNowMs)
    {
        if (aIsDowned && m_ignoreDownedReportsUntilMs.has_value() && aNowMs < *m_ignoreDownedReportsUntilMs)
            return false;

        auto& rec = m_players[aPlayerId];
        rec.PlayerId = aPlayerId;
        rec.PosX = aPosX;
        rec.PosY = aPosY;
        rec.PosZ = aPosZ;
        rec.NearbyHostileCount = aNearbyHostiles;

        const bool changed = (rec.IsDowned != aIsDowned);
        rec.IsDowned = aIsDowned;

        if (aIsDowned)
        {
            rec.ParticipatedInEncounter = true;
            rec.HadHostilesWhenDowned = (aNearbyHostiles > 0) || m_encounterActive;
            m_encounterActive = true;
            m_combatClearSinceMs.reset();
        }
        else
        {
            rec.HadHostilesWhenDowned = false;
            m_channelsByTarget.erase(aPlayerId);
            if (AreAnyEncounterParticipantsAlive())
                m_wipeStartedAtMs.reset();
        }

        (void)aNowMs;
        return changed;
    }

    void RemovePlayer(uint32_t aPlayerId)
    {
        m_players.erase(aPlayerId);
        m_channelsByTarget.erase(aPlayerId);
        for (auto it = m_channelsByTarget.begin(); it != m_channelsByTarget.end();)
        {
            if (it->second.ReviverPlayerId == aPlayerId)
                it = m_channelsByTarget.erase(it);
            else
                ++it;
        }
    }

    bool TryStartAllyRevive(uint32_t aReviverId, uint32_t aTargetId, uint64_t aNowMs)
    {
        if (aReviverId == aTargetId)
            return false;

        const auto reviverIt = m_players.find(aReviverId);
        const auto targetIt = m_players.find(aTargetId);
        if (reviverIt == m_players.end() || targetIt == m_players.end())
            return false;

        if (reviverIt->second.IsDowned || !targetIt->second.IsDowned)
            return false;

        if (DistanceMeters(reviverIt->second, targetIt->second) > m_config.ReviveMaxDistanceMeters)
            return false;

        const uint64_t durationMs = static_cast<uint64_t>(std::max(0.25f, m_config.ReviveDurationSec) * 1000.0f);
        m_channelsByTarget[aTargetId] = ActiveReviveChannel{
            aReviverId,
            aTargetId,
            aNowMs,
            aNowMs + durationMs,
        };
        return true;
    }

    bool CancelAllyRevive(uint32_t aReviverId, uint32_t aTargetId)
    {
        const auto it = m_channelsByTarget.find(aTargetId);
        if (it == m_channelsByTarget.end() || it->second.ReviverPlayerId != aReviverId)
            return false;

        m_channelsByTarget.erase(it);
        return true;
    }

    TickEvents Tick(uint64_t aNowMs)
    {
        TickEvents events{};

        // 1. Progress active E-revive channels (validate distance & alive status continuously)
        for (auto it = m_channelsByTarget.begin(); it != m_channelsByTarget.end();)
        {
            const auto revIt = m_players.find(it->second.ReviverPlayerId);
            const auto tgtIt = m_players.find(it->second.TargetPlayerId);

            if (revIt == m_players.end() || tgtIt == m_players.end() ||
                revIt->second.IsDowned ||
                DistanceMeters(revIt->second, tgtIt->second) > m_config.ReviveMaxDistanceMeters)
            {
                events.CancelledAllyRevives.emplace_back(it->second.ReviverPlayerId, it->second.TargetPlayerId);
                it = m_channelsByTarget.erase(it);
                continue;
            }

            if (aNowMs >= it->second.CompleteAtMs)
            {
                const uint32_t revId = it->second.ReviverPlayerId;
                const uint32_t tgtId = it->second.TargetPlayerId;
                tgtIt->second.IsDowned = false;
                tgtIt->second.HadHostilesWhenDowned = false;
                m_wipeStartedAtMs.reset();
                events.CompletedAllyRevives.emplace_back(revId, tgtId);
                it = m_channelsByTarget.erase(it);
                continue;
            }

            ++it;
        }

        // 2. Check Total Encounter Wipe: triggers when all combatants in the active encounter have fallen
        // (even if another party member is peacefully crafting 5 km away in another city and never entered the fight!).
        if (!m_players.empty() && !AreAnyEncounterParticipantsAlive() && CountDownedPlayers() > 0)
        {
            if (m_wipeCooldownUntilMs.has_value() && aNowMs < *m_wipeCooldownUntilMs)
                return events;

            if (!m_wipeStartedAtMs.has_value())
            {
                m_wipeStartedAtMs = aNowMs;
                events.PartyWipeWarningStarted = true;
            }
            else
            {
                const uint64_t wipeDelayMs = static_cast<uint64_t>(std::max(0.5f, m_config.PartyWipeDelaySec) * 1000.0f);
                if (aNowMs >= *m_wipeStartedAtMs && (aNowMs - *m_wipeStartedAtMs) >= wipeDelayMs)
                {
                    events.PartyWipeTriggered = true;
                    m_wipeStartedAtMs.reset();
                    m_wipeCooldownUntilMs = aNowMs + 15000;
                    return events;
                }
            }
            return events;
        }
        else
        {
            m_wipeStartedAtMs.reset();
        }

        // 3. Evaluate honest RFAB encounter completion (hysteresis + strict anti-flee leash)
        if (!m_encounterActive)
            return events;

        const bool anyHostilesActive = AreAnyPlayersInCombatOrNearHostiles();
        if (anyHostilesActive)
        {
            m_combatClearSinceMs.reset();
            return events;
        }

        if (!m_combatClearSinceMs.has_value())
        {
            m_combatClearSinceMs = aNowMs;
            return events;
        }

        const uint64_t hysteresisMs = static_cast<uint64_t>(std::max(0.5f, m_config.CombatClearHysteresisSec) * 1000.0f);
        if (aNowMs < *m_combatClearSinceMs || (aNowMs - *m_combatClearSinceMs) < hysteresisMs)
            return events;

        // Verify Strict RFAB Leash: every downed player must have at least one living party member
        // within LeashRadiusMeters (120m) so running away across the map never auto-revives fallen allies!
        for (const auto& [playerId, rec] : m_players)
        {
            if (!rec.IsDowned)
                continue;

            if (rec.NearbyHostileCount > 0)
                return events;

            if (!HasLivingAllyWithinLeash(rec))
                return events;
        }

        // Genuine victory confirmed! Auto-revive all fallen allies & emit encounter victory checkpoint signal.
        for (auto& [playerId, rec] : m_players)
        {
            if (rec.IsDowned)
            {
                rec.IsDowned = false;
                rec.HadHostilesWhenDowned = false;
                events.PostCombatRevivedPlayers.push_back(playerId);
            }
            rec.ParticipatedInEncounter = false;
        }

        m_channelsByTarget.clear();
        m_encounterActive = false;
        m_combatClearSinceMs.reset();
        events.EncounterVictoryResolved = true;
        events.BossDefeatedInResolvedEncounter = m_bossDefeatedInCurrentEncounter;
        m_bossDefeatedInCurrentEncounter = false;
        return events;
    }

    void ResetAll(uint64_t aNowMs = 0) noexcept
    {
        for (auto& [_, rec] : m_players)
        {
            rec.IsDowned = false;
            rec.HadHostilesWhenDowned = false;
            rec.ParticipatedInEncounter = false;
            rec.InCombat = false;
            rec.NearbyHostileCount = 0;
        }
        m_channelsByTarget.clear();
        m_encounterActive = false;
        m_bossDefeatedInCurrentEncounter = false;
        m_combatClearSinceMs.reset();
        m_wipeStartedAtMs.reset();
        if (aNowMs > 0)
        {
            m_ignoreDownedReportsUntilMs = aNowMs + 6000;
            m_wipeCooldownUntilMs = aNowMs + 15000;
        }
        else
        {
            m_ignoreDownedReportsUntilMs.reset();
            m_wipeCooldownUntilMs.reset();
        }
    }

    [[nodiscard]] bool IsPlayerDowned(uint32_t aPlayerId) const noexcept
    {
        const auto it = m_players.find(aPlayerId);
        return it != m_players.end() && it->second.IsDowned;
    }

    [[nodiscard]] bool IsEncounterActiveWithLivingParticipants() const noexcept
    {
        return m_encounterActive && AreAnyEncounterParticipantsAlive();
    }

private:
    [[nodiscard]] static float DistanceMeters(const PlayerCombatRecord& aA, const PlayerCombatRecord& aB) noexcept
    {
        const float dx = aA.PosX - aB.PosX;
        const float dy = aA.PosY - aB.PosY;
        const float dz = aA.PosZ - aB.PosZ;
        const float skyrimUnits = std::sqrt(dx * dx + dy * dy + dz * dz);
        return RfabCompat::SkyrimUnitsToMeters(skyrimUnits);
    }

    [[nodiscard]] bool AreAnyEncounterParticipantsAlive() const noexcept
    {
        for (const auto& [_, rec] : m_players)
        {
            if (rec.ParticipatedInEncounter && !rec.IsDowned)
                return true;
            if (!rec.IsDowned && IsWithinLeashOfAnyParticipant(rec))
                return true;
        }
        return false;
    }

    [[nodiscard]] bool IsWithinLeashOfAnyParticipant(const PlayerCombatRecord& acCandidate) const noexcept
    {
        for (const auto& [otherId, otherRec] : m_players)
        {
            if (otherId == acCandidate.PlayerId || !otherRec.ParticipatedInEncounter)
                continue;
            if (DistanceMeters(acCandidate, otherRec) <= m_config.LeashRadiusMeters)
                return true;
        }
        return false;
    }

    [[nodiscard]] size_t CountDownedPlayers() const noexcept
    {
        size_t count = 0;
        for (const auto& [_, rec] : m_players)
        {
            if (rec.IsDowned)
                ++count;
        }
        return count;
    }

    [[nodiscard]] bool AreAnyPlayersInCombatOrNearHostiles() const noexcept
    {
        for (const auto& [_, rec] : m_players)
        {
            if (rec.InCombat || rec.NearbyHostileCount > 0)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool HasLivingAllyWithinLeash(const PlayerCombatRecord& acDowned) const noexcept
    {
        for (const auto& [otherId, otherRec] : m_players)
        {
            if (otherId == acDowned.PlayerId || otherRec.IsDowned)
                continue;

            if (DistanceMeters(acDowned, otherRec) <= m_config.LeashRadiusMeters)
                return true;
        }
        return false;
    }

    Config m_config{};
    std::unordered_map<uint32_t, PlayerCombatRecord> m_players{};
    std::unordered_map<uint32_t, ActiveReviveChannel> m_channelsByTarget{};
    bool m_encounterActive{false};
    bool m_bossDefeatedInCurrentEncounter{false};
    std::optional<uint64_t> m_combatClearSinceMs{};
    std::optional<uint64_t> m_wipeStartedAtMs{};
    std::optional<uint64_t> m_ignoreDownedReportsUntilMs{};
    std::optional<uint64_t> m_wipeCooldownUntilMs{};
};

class EncounterService
{
public:
    EncounterService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

    void ResetAllStatesAfterRollback() noexcept;
    [[nodiscard]] EncounterCoordinator& GetCoordinator() noexcept { return m_coordinator; }

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnSyncPlayerDownedState(const PacketEvent<SyncPlayerDownedState>& acMessage) noexcept;
    void OnReviveActionRequest(const PacketEvent<ReviveActionRequest>& acMessage) noexcept;
    void OnCombatEncounterStateReport(const PacketEvent<CombatEncounterStateReport>& acMessage) noexcept;

    void BroadcastPlayerDownedState(Player* apPlayer, bool aIsDowned, float aX, float aY, float aZ) noexcept;

    World& m_world;
    EncounterCoordinator m_coordinator{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_downedStateConnection;
    entt::scoped_connection m_reviveRequestConnection;
    entt::scoped_connection m_combatReportConnection;
};
