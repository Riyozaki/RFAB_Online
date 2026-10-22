#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct World;
struct PlayerJoinEvent;
struct SyncRfabProgressionEvent;

/**
 * Pure deterministic ledger backing RFAB shared progression, XP deduplication,
 * read XP books (XPBooksReaded), synced TESGlobals, and dynamic ambush spawn deduplication.
 * Exposed cleanly so unit tests and SaveSnapshotService operate on the exact same logic.
 */
class RfabProgressionLedger
{
public:
    struct AmbushRecord
    {
        uint32_t ServerEntityId{};
        uint32_t OwnerPlayerId{};
        GameId CellId{};
        GameId BaseActorId{};
        float PosX{};
        float PosY{};
        float PosZ{};
        uint64_t SpawnedAtMs{};
        std::unordered_set<uint32_t> MatchedPeerPlayerIds{};
    };

    struct Snapshot
    {
        int64_t TotalSharedXp{};
        std::unordered_map<std::string, int32_t> GrantedXpBySource{};
        std::vector<GameId> ReadBooks{};
        std::unordered_map<std::string, float> SyncedGlobals{};
        std::vector<AmbushRecord> RecentAmbushes{};
    };

    struct XpGrantOutcome
    {
        bool Accepted{false};
        int32_t AppliedDelta{0};
        int32_t DuplicateRebateDelta{0};
        int64_t AuthoritativeTotalXp{0};
    };

    XpGrantOutcome TryGrantXp(const std::string& acSourceKey, int32_t aXpDelta)
    {
        XpGrantOutcome out{};
        if (aXpDelta <= 0)
        {
            out.AuthoritativeTotalXp = m_totalSharedXp;
            return out;
        }

        if (!acSourceKey.empty())
        {
            if (const auto it = m_grantedXpBySource.find(acSourceKey); it != m_grantedXpBySource.end())
            {
                out.Accepted = false;
                out.DuplicateRebateDelta = -aXpDelta;
                out.AuthoritativeTotalXp = m_totalSharedXp;
                return out;
            }
            m_grantedXpBySource.emplace(acSourceKey, aXpDelta);
        }

        m_totalSharedXp += aXpDelta;
        out.Accepted = true;
        out.AppliedDelta = aXpDelta;
        out.AuthoritativeTotalXp = m_totalSharedXp;
        return out;
    }

    bool RecordBookRead(const GameId& acBookId)
    {
        if (!acBookId)
            return false;

        for (const auto& existing : m_readBooks)
        {
            if (existing == acBookId)
                return false;
        }

        m_readBooks.push_back(acBookId);
        return true;
    }

    bool UpdateGlobal(const std::string& acEditorId, float aValue)
    {
        if (acEditorId.empty() || !std::isfinite(aValue))
            return false;

        const auto it = m_syncedGlobals.find(acEditorId);
        if (it != m_syncedGlobals.end() && std::fabs(it->second - aValue) < 1e-4f)
            return false;

        m_syncedGlobals[acEditorId] = aValue;
        return true;
    }

    // Matches a second client's duplicate dynamic ambush spawn (0xFF000000 PlaceActorAtMe) 1-to-1
    // against an entity spawned by ANOTHER player within [aWindowMs] and [aRadiusUnits].
    // Preserves multi-actor ambushes (e.g. PlaceActorAtMe(Wolf, 3)): Owner spawns 3 entities,
    // and each peer binds its 3 local spawns 1-to-1 to those exact 3 entities instead of collapsing to 1 or duplicating to 6!
    [[nodiscard]] uint32_t FindMatchingRecentAmbush(
        uint32_t aRequestingPlayerId,
        const GameId& acCellId,
        const GameId& acBaseActorId,
        float aPosX,
        float aPosY,
        float aPosZ,
        uint64_t aNowMs,
        uint64_t aWindowMs,
        float aRadiusUnits) noexcept
    {
        PruneExpiredAmbushes(aNowMs, aWindowMs);

        const float maxDistSq = aRadiusUnits * aRadiusUnits;
        for (auto& rec : m_recentAmbushes)
        {
            if (rec.OwnerPlayerId == aRequestingPlayerId)
                continue;

            if (rec.MatchedPeerPlayerIds.contains(aRequestingPlayerId))
                continue;

            if (rec.CellId != acCellId || rec.BaseActorId != acBaseActorId)
                continue;

            const float dx = rec.PosX - aPosX;
            const float dy = rec.PosY - aPosY;
            const float dz = rec.PosZ - aPosZ;
            const float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq <= maxDistSq)
            {
                rec.MatchedPeerPlayerIds.insert(aRequestingPlayerId);
                return rec.ServerEntityId;
            }
        }

        return 0;
    }

    void RegisterAmbushSpawn(
        uint32_t aServerEntityId,
        uint32_t aOwnerPlayerId,
        const GameId& acCellId,
        const GameId& acBaseActorId,
        float aPosX,
        float aPosY,
        float aPosZ,
        uint64_t aNowMs,
        uint64_t aWindowMs)
    {
        PruneExpiredAmbushes(aNowMs, aWindowMs);
        m_recentAmbushes.push_back(AmbushRecord{
            aServerEntityId,
            aOwnerPlayerId,
            acCellId,
            acBaseActorId,
            aPosX,
            aPosY,
            aPosZ,
            aNowMs,
            {},
        });
    }

    [[nodiscard]] Snapshot CaptureSnapshot() const
    {
        return Snapshot{
            m_totalSharedXp,
            m_grantedXpBySource,
            m_readBooks,
            m_syncedGlobals,
            m_recentAmbushes,
        };
    }

    void RestoreSnapshot(const Snapshot& acSnapshot)
    {
        m_totalSharedXp = acSnapshot.TotalSharedXp;
        m_grantedXpBySource = acSnapshot.GrantedXpBySource;
        m_readBooks = acSnapshot.ReadBooks;
        m_syncedGlobals = acSnapshot.SyncedGlobals;
        m_recentAmbushes = acSnapshot.RecentAmbushes;
    }

    [[nodiscard]] int64_t GetTotalSharedXp() const noexcept { return m_totalSharedXp; }
    [[nodiscard]] const std::vector<GameId>& GetReadBooks() const noexcept { return m_readBooks; }
    [[nodiscard]] const std::unordered_map<std::string, float>& GetSyncedGlobals() const noexcept { return m_syncedGlobals; }

private:
    void PruneExpiredAmbushes(uint64_t aNowMs, uint64_t aWindowMs) noexcept
    {
        m_recentAmbushes.erase(
            std::remove_if(
                m_recentAmbushes.begin(),
                m_recentAmbushes.end(),
                [aNowMs, aWindowMs](const AmbushRecord& r) { return aNowMs > r.SpawnedAtMs && (aNowMs - r.SpawnedAtMs) > aWindowMs; }),
            m_recentAmbushes.end());
    }

    int64_t m_totalSharedXp{0};
    std::unordered_map<std::string, int32_t> m_grantedXpBySource{};
    std::vector<GameId> m_readBooks{};
    std::unordered_map<std::string, float> m_syncedGlobals{};
    std::vector<AmbushRecord> m_recentAmbushes{};
};

class RfabProgressionService
{
public:
    RfabProgressionService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

    [[nodiscard]] RfabProgressionLedger& GetLedger() noexcept { return m_ledger; }
    [[nodiscard]] const RfabProgressionLedger& GetLedger() const noexcept { return m_ledger; }

private:
    void OnSyncProgressionEvent(const PacketEvent<SyncRfabProgressionEvent>& acMessage) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;

    World& m_world;
    RfabProgressionLedger m_ledger;

    entt::scoped_connection m_progressionConnection;
    entt::scoped_connection m_playerJoinConnection;
};
