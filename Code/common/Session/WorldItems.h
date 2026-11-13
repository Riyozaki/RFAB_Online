#pragma once

/**
 * Server-owned dropped items (architecture step 4).
 *
 * WHY THIS EXISTS
 * Before v4.1.0 a dropped item had no identity. The dropper's engine created a temporary 0xFF
 * reference, the server only adjusted the dropper's inventory and told the players *in range* to
 * replay the drop on the dropper's puppet. Consequences, all reported in playtests:
 *   - a player who was not in range, joined later or reloaded never saw the item at all;
 *   - two players picking the same item at the same moment both got it (nobody arbitrated);
 *   - on pickup every peer *guessed* which ground reference to delete (common/DroppedItemMatch.h:
 *     same base form, closest, preferably same count) and could delete an unrelated stack.
 *
 * CONTRACT
 *  - The server owns every synced dropped item: `Registry` gives it an id, remembers where it lies
 *    and how many there are, and bounds how many exist (world, per cell, per-player drop rate).
 *  - Every change is stamped with a registry `Revision` and sent to EVERY joined player, not just to
 *    the players in range: `NotifyWorldItemSpawn` (upsert), `NotifyWorldItemDespawn`. A join, a
 *    resync request (Session::Resync::Scope::kWorldItems) and a checkpoint rollback send the whole
 *    `NotifyWorldItemSnapshot`. The client keeps a `ClientMirror` of the records and creates the
 *    engine reference only when the item's cell is loaded around the local player.
 *  - A pickup is a claim (`Registry::TryClaim`): the first claim the server processes wins, the
 *    loser's optimistic local pickup is rolled back by the server, and the reference is removed on
 *    every client *by id* - nobody guesses.
 *  - The client detects a lost delta from the revision sequence (`ClientMirror::AcceptRevision`)
 *    and asks for a snapshot through the step-1 resync ledger.
 *
 * THREADING: single-threaded by design. `Registry` lives on the server tick, `ClientMirror` on the
 * client main thread. Engine work (creating / deleting references) stays in the client service;
 * this header only decides.
 *
 * Everything here is engine-free and wire-free (templated on the payload) so the portable test
 * loop can run it: Code/tests/rfab_v410_world_items.cpp.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace WorldItems
{

using ItemId = uint32_t;

// Item ids are allocated by `Registry` starting at 1 and never reused within a server process, so 0
// can safely mean "no item" (unlike server entity ids, see Session::Resync::kNoServerId).
inline constexpr ItemId kNoItem = 0;

// Packs a (mod id, base id) game id into one map key. 0 means "none" (e.g. an interior has no
// worldspace).
[[nodiscard]] constexpr uint64_t PackKey(uint32_t aModId, uint32_t aBaseId) noexcept
{
    return (static_cast<uint64_t>(aModId) << 32) | static_cast<uint64_t>(aBaseId);
}

// Serial-number comparison (RFC 1982 style) so the revision may wrap around.
[[nodiscard]] constexpr bool IsNewerRevision(uint32_t aCandidate, uint32_t aCurrent) noexcept
{
    return static_cast<int32_t>(aCandidate - aCurrent) > 0;
}

struct Policy
{
    // Bounds what the world can hold. A full registry refuses new drops (the item then stays a
    // local, unsynced reference of the dropper - exactly the pre-4.1 behaviour for that item);
    // it never evicts an item another player may be walking towards.
    size_t MaxItems{1024};
    size_t MaxItemsPerCell{256};
    // Per-player token bucket: a burst of 32 drops, then 4 per second sustained.
    uint32_t DropBurst{32};
    uint64_t DropRefillIntervalMs{250};
    // Same bound CoopPacketSafety::SanitizeItemCountDelta applies to inventory deltas.
    int32_t MaxStackCount{1'000'000};
};

enum class DropResult : uint8_t
{
    kAccepted = 0,
    kInvalidCount = 1,
    kRateLimited = 2,
    kCellFull = 3,
    kWorldFull = 4,
};

enum class ClaimResult : uint8_t
{
    kGrantedAll = 0,      // the whole stack went to the claimer: the item is gone
    kGrantedPartial = 1,  // the claimer took part of the stack: the item stays with `Remaining`
    kUnknownItem = 2,     // never existed or somebody else's claim was processed first
    kInvalidCount = 3,    // requested <= 0: nothing granted, nothing changed
};

// Why an item left the world. Travels on the wire (NotifyWorldItemDespawn): append only.
enum class DespawnReason : uint8_t
{
    kPickedUp = 0,      // a claim took the whole stack
    kRemoved = 1,       // removed by the server (admin command)
    kClaimRejected = 2, // sent only to a claimer whose item the server does not have: the claimer's
                        // mirror was stale, the item is not (or no longer) in the world
};
inline constexpr uint8_t kDespawnReasonCount = 3;

[[nodiscard]] constexpr DespawnReason SanitizeDespawnReason(uint8_t aRaw) noexcept
{
    return aRaw < kDespawnReasonCount ? static_cast<DespawnReason>(aRaw) : DespawnReason::kRemoved;
}

[[nodiscard]] constexpr const char* ToString(DespawnReason aReason) noexcept
{
    switch (aReason)
    {
    case DespawnReason::kPickedUp: return "picked-up";
    case DespawnReason::kRemoved: return "removed";
    case DespawnReason::kClaimRejected: return "claim-rejected";
    }
    return "?";
}

// Snapshot size bound on the wire: the registry never holds more than Policy::MaxItems (1024).
inline constexpr size_t kMaxSnapshotItems = 4096;

[[nodiscard]] constexpr const char* ToString(DropResult aResult) noexcept
{
    switch (aResult)
    {
    case DropResult::kAccepted: return "accepted";
    case DropResult::kInvalidCount: return "invalid-count";
    case DropResult::kRateLimited: return "rate-limited";
    case DropResult::kCellFull: return "cell-full";
    case DropResult::kWorldFull: return "world-full";
    }
    return "?";
}

[[nodiscard]] constexpr const char* ToString(ClaimResult aResult) noexcept
{
    switch (aResult)
    {
    case ClaimResult::kGrantedAll: return "granted-all";
    case ClaimResult::kGrantedPartial: return "granted-partial";
    case ClaimResult::kUnknownItem: return "unknown-item";
    case ClaimResult::kInvalidCount: return "invalid-count";
    }
    return "?";
}

/**
 * The server's authoritative list of dropped items. `TPayload` is whatever the server wants to send
 * back (the inventory entry, placement ...); the registry only reads the fields passed explicitly.
 */
template <class TPayload> class Registry
{
public:
    struct Record
    {
        ItemId Id{kNoItem};
        int32_t Count{0};
        uint64_t CellKey{0};
        uint32_t DropperPlayerId{0};
        uint32_t DropperCookie{0};
        uint64_t CreatedMs{0};
        TPayload Payload{};
    };

    struct Claim
    {
        ClaimResult Result{ClaimResult::kUnknownItem};
        int32_t Granted{0};
        int32_t Remaining{0};
    };

    explicit Registry(Policy aPolicy = {}) noexcept
        : m_policy(aPolicy)
    {
    }

    [[nodiscard]] const Policy& GetPolicy() const noexcept { return m_policy; }

    /**
     * A player dropped `aCount` items in `aCellKey`. On kAccepted `aOutId` names the new item and
     * the revision moved; on any refusal nothing changed (the rate-limit token is only spent on
     * acceptance, so a full cell does not also lock the player out of other cells).
     */
    DropResult TryDrop(uint32_t aPlayerId, uint64_t aCellKey, int32_t aCount, uint32_t aCookie, TPayload aPayload,
                       uint64_t aNowMs, ItemId& aOutId)
    {
        aOutId = kNoItem;
        if (aCount <= 0 || aCount > m_policy.MaxStackCount)
            return DropResult::kInvalidCount;
        if (m_items.size() >= m_policy.MaxItems)
            return DropResult::kWorldFull;
        if (CountInCell(aCellKey) >= m_policy.MaxItemsPerCell)
            return DropResult::kCellFull;

        DropBucket& bucket = m_buckets.try_emplace(aPlayerId, DropBucket{m_policy.DropBurst, aNowMs}).first->second;
        Refill(bucket, aNowMs);
        if (bucket.Tokens == 0)
            return DropResult::kRateLimited;
        --bucket.Tokens;

        Record record{};
        record.Id = AllocateId();
        record.Count = aCount;
        record.CellKey = aCellKey;
        record.DropperPlayerId = aPlayerId;
        record.DropperCookie = aCookie;
        record.CreatedMs = aNowMs;
        record.Payload = std::move(aPayload);

        aOutId = record.Id;
        ++m_perCell[aCellKey];
        m_items.emplace(record.Id, std::move(record));
        ++m_revision;
        return DropResult::kAccepted;
    }

    /**
     * Arbitrates a pickup. Processing order is the arbitration: the first claim wins the stack (or
     * the part it asked for), later claims see kUnknownItem / a smaller stack.
     */
    Claim TryClaim(ItemId aId, int32_t aRequested)
    {
        Claim claim{};
        if (aRequested <= 0)
        {
            claim.Result = ClaimResult::kInvalidCount;
            return claim;
        }

        const auto it = m_items.find(aId);
        if (it == m_items.end())
        {
            claim.Result = ClaimResult::kUnknownItem;
            return claim;
        }

        Record& record = it->second;
        claim.Granted = std::min(aRequested, record.Count);
        claim.Remaining = record.Count - claim.Granted;
        ++m_revision;
        if (claim.Remaining > 0)
        {
            record.Count = claim.Remaining;
            claim.Result = ClaimResult::kGrantedPartial;
            return claim;
        }

        EraseIterator(it);
        claim.Result = ClaimResult::kGrantedAll;
        return claim;
    }

    // Removes an item without granting it to anybody (admin command). True when it existed.
    bool Remove(ItemId aId)
    {
        const auto it = m_items.find(aId);
        if (it == m_items.end())
            return false;
        EraseIterator(it);
        ++m_revision;
        return true;
    }

    [[nodiscard]] const Record* Find(ItemId aId) const noexcept
    {
        const auto it = m_items.find(aId);
        return it == m_items.end() ? nullptr : &it->second;
    }

    [[nodiscard]] Record* FindMutable(ItemId aId) noexcept
    {
        const auto it = m_items.find(aId);
        return it == m_items.end() ? nullptr : &it->second;
    }

    // Records sorted by id: deterministic snapshots and checkpoint files.
    [[nodiscard]] std::vector<Record> Export() const
    {
        std::vector<Record> out;
        out.reserve(m_items.size());
        for (const auto& [id, record] : m_items)
            out.push_back(record);
        std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) { return a.Id < b.Id; });
        return out;
    }

    /**
     * Replaces the whole content (checkpoint restore). Records with id 0, a duplicate id or an
     * invalid count are skipped, and the world / cell bounds still hold. Ids keep their value - the
     * id counter moves past them so a later drop can never reuse one. Drop cookies are cleared: a
     * cookie only pairs the live echo with the dropper's pending reference, and after a restore (or
     * a server restart, where player ids are handed out again) it could pair a stranger's drop.
     * Always moves the revision: the caller sends the result as a snapshot. Returns how many records
     * were imported.
     */
    size_t Import(std::vector<Record> aRecords)
    {
        m_items.clear();
        m_perCell.clear();
        std::sort(aRecords.begin(), aRecords.end(), [](const Record& a, const Record& b) { return a.Id < b.Id; });
        for (auto& record : aRecords)
        {
            if (record.Id == kNoItem || record.Count <= 0 || record.Count > m_policy.MaxStackCount)
                continue;
            if (m_items.size() >= m_policy.MaxItems || CountInCell(record.CellKey) >= m_policy.MaxItemsPerCell)
                continue;
            if (m_items.find(record.Id) != m_items.end())
                continue;
            if (record.Id >= m_nextId)
                m_nextId = record.Id + 1 == kNoItem ? 1 : record.Id + 1;
            ++m_perCell[record.CellKey];
            record.DropperCookie = 0;
            const ItemId id = record.Id;
            m_items.emplace(id, std::move(record));
        }
        ++m_revision;
        return m_items.size();
    }

    // Empties the registry; moves the revision only when something was removed.
    void Clear() noexcept
    {
        if (m_items.empty())
            return;
        m_items.clear();
        m_perCell.clear();
        ++m_revision;
    }

    // The player left: their drop budget goes with them (a reconnect starts with a full bucket).
    // Their items stay in the world.
    void ForgetPlayer(uint32_t aPlayerId) noexcept { m_buckets.erase(aPlayerId); }

    [[nodiscard]] uint32_t GetRevision() const noexcept { return m_revision; }
    [[nodiscard]] size_t Size() const noexcept { return m_items.size(); }
    [[nodiscard]] size_t CountInCell(uint64_t aCellKey) const noexcept
    {
        const auto it = m_perCell.find(aCellKey);
        return it == m_perCell.end() ? 0 : it->second;
    }

private:
    struct DropBucket
    {
        uint32_t Tokens{0};
        uint64_t LastRefillMs{0};
    };

    void Refill(DropBucket& aBucket, uint64_t aNowMs) const noexcept
    {
        // A clock that went backwards refills nothing (and does not move the reference point, which
        // would credit the same time twice once it moves forward again).
        if (aNowMs < aBucket.LastRefillMs)
            return;
        if (m_policy.DropRefillIntervalMs == 0)
        {
            aBucket.Tokens = m_policy.DropBurst;
            return;
        }
        const uint64_t steps = (aNowMs - aBucket.LastRefillMs) / m_policy.DropRefillIntervalMs;
        if (steps == 0)
            return;
        aBucket.LastRefillMs += steps * m_policy.DropRefillIntervalMs;
        aBucket.Tokens = static_cast<uint32_t>(std::min<uint64_t>(m_policy.DropBurst, aBucket.Tokens + steps));
    }

    ItemId AllocateId() noexcept
    {
        // 2^32 drops in one process before a wrap; skipping live ids keeps even that safe.
        for (;;)
        {
            const ItemId id = m_nextId;
            m_nextId = m_nextId + 1 == kNoItem ? 1 : m_nextId + 1;
            if (id != kNoItem && m_items.find(id) == m_items.end())
                return id;
        }
    }

    void EraseIterator(typename std::unordered_map<ItemId, Record>::iterator aIt) noexcept
    {
        const uint64_t cellKey = aIt->second.CellKey;
        m_items.erase(aIt);
        if (const auto cellIt = m_perCell.find(cellKey); cellIt != m_perCell.end())
        {
            if (cellIt->second <= 1)
                m_perCell.erase(cellIt);
            else
                --cellIt->second;
        }
    }

    Policy m_policy;
    std::unordered_map<ItemId, Record> m_items{};
    std::unordered_map<uint64_t, size_t> m_perCell{};
    std::unordered_map<uint32_t, DropBucket> m_buckets{};
    ItemId m_nextId{1};
    uint32_t m_revision{0};
};

// ---------------------------------------------------------------------------------------------
// Client side
// ---------------------------------------------------------------------------------------------

// Where an item lies, or where the local player stands, in server game-id space. An interior has
// WorldSpaceKey 0 and is matched by cell; exteriors are matched by worldspace and distance because
// the loaded grid spans several cells.
struct Placement
{
    uint64_t CellKey{0};
    uint64_t WorldSpaceKey{0};
    float X{0.0f};
    float Y{0.0f};
    float Z{0.0f};
};

// uGridsToLoad=5 keeps +-2 cells (4096 units each) loaded around the player: 6000 stays inside the
// loaded grid in every direction, so a materialized reference always lands in an attached cell.
inline constexpr float kExteriorMaterializeRadius = 6000.0f;

[[nodiscard]] inline bool IsWithinMaterializeRange(const Placement& acItem, const Placement& acViewer,
                                                   float aRadius = kExteriorMaterializeRadius) noexcept
{
    if (acItem.WorldSpaceKey == 0 || acViewer.WorldSpaceKey == 0)
        return acItem.WorldSpaceKey == acViewer.WorldSpaceKey && acItem.CellKey != 0 && acItem.CellKey == acViewer.CellKey;
    if (acItem.WorldSpaceKey != acViewer.WorldSpaceKey)
        return false;
    const float dx = acItem.X - acViewer.X;
    const float dy = acItem.Y - acViewer.Y;
    return dx * dx + dy * dy <= aRadius * aRadius;
}

enum class BindingOrigin : uint8_t
{
    kNone = 0,
    kDroppedHere = 1,       // the local engine created it when the local player/NPC dropped it
    kMaterializedHere = 2,  // the client service created it from a server record
};

enum class RevisionCheck : uint8_t
{
    kInOrder = 0,    // exactly the next revision
    kDuplicate = 1,  // not newer than what the mirror already has: ignore the delta
    kGap = 2,        // newer than the next one, or no snapshot yet: apply it AND ask for a snapshot
};

struct ClientPolicy
{
    // An own drop whose server echo never came (refused, or the session ended) stops waiting.
    uint64_t PendingDropTimeoutMs{30000};
    size_t MaxPendingDrops{256};
    // A reference that could not be created is retried with back-off, then left alone for a while.
    uint32_t MaxMaterializeAttempts{3};
    uint64_t MaterializeRetryMs{2000};
    uint64_t MaterializeQuarantineMs{30000};
    // After a whole-stack pickup the item is not shown again while the claim is in flight. The answer
    // (despawn, remainder upsert, or the despawn the server sends a losing claimer) normally comes
    // within a round trip; if it never does, the item is shown again rather than lost from view.
    uint64_t ClaimInFlightMs{15000};
};

/**
 * The client's copy of the registry plus the binding of each item to the engine reference that shows
 * it here. It never touches the engine: methods return what the service must create or delete.
 */
template <class TPayload> class ClientMirror
{
public:
    struct Record
    {
        ItemId Id{kNoItem};
        int32_t Count{0};
        uint32_t DropperPlayerId{0};
        uint32_t DropperCookie{0};
        Placement Where{};
        TPayload Payload{};
        // Binding to the engine reference (0 = nothing shown here yet).
        uint32_t BoundFormId{0};
        BindingOrigin Origin{BindingOrigin::kNone};
        // Materialization back-off.
        uint32_t FailedAttempts{0};
        uint64_t RetryAfterMs{0};
    };

    struct Binding
    {
        ItemId Id{kNoItem};
        uint32_t FormId{0};
        BindingOrigin Origin{BindingOrigin::kNone};
    };

    // What an upsert/snapshot means for the engine.
    struct UpsertEffect
    {
        bool Inserted{false};
        // The item was matched with a reference this client dropped (cookie echo): adopt it.
        uint32_t AdoptedFormId{0};
        // The shown reference no longer matches (stack size changed): delete it, it is recreated.
        uint32_t StaleFormId{0};
        BindingOrigin StaleOrigin{BindingOrigin::kNone};
    };

    explicit ClientMirror(ClientPolicy aPolicy = {}) noexcept
        : m_policy(aPolicy)
    {
    }

    [[nodiscard]] const ClientPolicy& GetPolicy() const noexcept { return m_policy; }

    // --- revision sequence --------------------------------------------------------------------

    /**
     * Classifies a delta's revision and, unless it is a duplicate, advances the mirror to it. A gap
     * is applied anyway (spawn/despawn are idempotent) and the caller asks for a snapshot.
     */
    RevisionCheck AcceptRevision(uint32_t aRevision) noexcept
    {
        if (!m_hasBaseline)
        {
            m_hasBaseline = true;  // a snapshot is requested; until it arrives deltas still apply
            m_revision = aRevision;
            ++m_gaps;
            return RevisionCheck::kGap;
        }
        if (!IsNewerRevision(aRevision, m_revision))
        {
            ++m_duplicates;
            return RevisionCheck::kDuplicate;
        }
        const bool inOrder = aRevision == m_revision + 1;
        m_revision = aRevision;
        if (inOrder)
            return RevisionCheck::kInOrder;
        ++m_gaps;
        return RevisionCheck::kGap;
    }

    [[nodiscard]] bool HasBaseline() const noexcept { return m_hasBaseline; }
    [[nodiscard]] uint32_t GetRevision() const noexcept { return m_revision; }
    [[nodiscard]] uint32_t GetGapCount() const noexcept { return m_gaps; }
    [[nodiscard]] uint32_t GetDuplicateCount() const noexcept { return m_duplicates; }

    // --- own drops ----------------------------------------------------------------------------

    // The local engine created `aFormId` for a drop announced to the server with `aCookie`.
    void NotePendingDrop(uint32_t aCookie, uint32_t aFormId, uint64_t aNowMs)
    {
        if (aCookie == 0 || aFormId == 0)
            return;
        ExpirePendingDrops(aNowMs);
        if (m_pendingDrops.size() >= m_policy.MaxPendingDrops)
        {
            // Oldest first: the one least likely to still get an echo.
            auto oldest = m_pendingDrops.begin();
            for (auto it = m_pendingDrops.begin(); it != m_pendingDrops.end(); ++it)
                if (it->second.CreatedMs < oldest->second.CreatedMs)
                    oldest = it;
            m_pendingDrops.erase(oldest);
        }
        m_pendingDrops[aCookie] = PendingDrop{aFormId, aNowMs};
    }

    // Returns how many pending drops timed out (the items stay local, unsynced references).
    size_t ExpirePendingDrops(uint64_t aNowMs) noexcept
    {
        size_t expired = 0;
        for (auto it = m_pendingDrops.begin(); it != m_pendingDrops.end();)
        {
            if (aNowMs >= it->second.CreatedMs + m_policy.PendingDropTimeoutMs || aNowMs < it->second.CreatedMs)
            {
                it = m_pendingDrops.erase(it);
                ++expired;
            }
            else
            {
                ++it;
            }
        }
        m_expiredDrops += static_cast<uint32_t>(expired);
        return expired;
    }

    [[nodiscard]] size_t PendingDropCount() const noexcept { return m_pendingDrops.size(); }
    [[nodiscard]] uint32_t GetExpiredDropCount() const noexcept { return m_expiredDrops; }

    // --- deltas -------------------------------------------------------------------------------

    /**
     * Server upsert. `aShownCount` reports the stack size of the reference currently bound to this
     * id (0 when unknown/none): a different server count means the reference must be rebuilt.
     */
    UpsertEffect Upsert(ItemId aId, int32_t aCount, uint32_t aDropperPlayerId, uint32_t aDropperCookie, const Placement& acWhere,
                        TPayload aPayload, uint32_t aLocalPlayerId, int32_t aShownCount = 0)
    {
        UpsertEffect effect{};
        if (aId == kNoItem || aCount <= 0)
            return effect;

        auto [it, inserted] = m_records.try_emplace(aId);
        Record& record = it->second;
        effect.Inserted = inserted;
        const int32_t previousCount = record.Count;
        record.Id = aId;
        record.Count = aCount;
        record.DropperPlayerId = aDropperPlayerId;
        record.DropperCookie = aDropperCookie;
        record.Where = acWhere;
        record.Payload = std::move(aPayload);

        if (record.BoundFormId == 0)
        {
            // New server state answers any claim this client had in flight for the item: it may be
            // shown again right away (a remainder after a pickup that took less than the server had).
            if (!inserted && previousCount != aCount)
                record.RetryAfterMs = 0;
            TryAdoptPendingDrop(record, aLocalPlayerId, effect);
            return effect;
        }

        // Already shown: keep the reference unless the stack it shows is now wrong.
        const int32_t shown = aShownCount > 0 ? aShownCount : previousCount;
        if (shown != aCount)
        {
            effect.StaleFormId = record.BoundFormId;
            effect.StaleOrigin = record.Origin;
            Unbind(record);
        }
        return effect;
    }

    /**
     * Server despawn: returns the reference to delete here (FormId 0 when nothing was shown). Unlike an
     * upsert, a despawn is applied even when its revision is not newer: ids are never reused, so a
     * removal is final and can never undo newer state. The server relies on that when it answers a
     * losing claimer with a despawn stamped with the current revision.
     */
    Binding Remove(ItemId aId) noexcept
    {
        Binding binding{};
        const auto it = m_records.find(aId);
        if (it == m_records.end())
            return binding;
        binding = Binding{aId, it->second.BoundFormId, it->second.Origin};
        if (it->second.BoundFormId != 0)
            m_byFormId.erase(it->second.BoundFormId);
        m_records.erase(it);
        return binding;
    }

    struct SnapshotEntry
    {
        ItemId Id{kNoItem};
        int32_t Count{0};
        uint32_t DropperPlayerId{0};
        uint32_t DropperCookie{0};
        Placement Where{};
        TPayload Payload{};
        int32_t ShownCount{0};  // filled by the service for already-bound ids (0: unknown)
    };

    struct SnapshotEffect
    {
        std::vector<Binding> Vanished;   // bound references whose item no longer exists: delete them
        std::vector<Binding> Stale;      // bound references showing a wrong stack: delete, recreated later
        std::vector<Binding> Adopted;    // own drops matched by cookie: mark them as synced
    };

    /**
     * Authoritative replacement (join, resync, rollback). Ids absent from the snapshot are removed and
     * their references reported for deletion; present ids keep their binding; own pending drops are
     * adopted by cookie.
     */
    SnapshotEffect ApplySnapshot(uint32_t aRevision, std::vector<SnapshotEntry> aEntries, uint32_t aLocalPlayerId)
    {
        SnapshotEffect effect{};
        std::unordered_map<ItemId, size_t> incoming;
        incoming.reserve(aEntries.size());
        for (size_t i = 0; i < aEntries.size(); ++i)
            if (aEntries[i].Id != kNoItem && aEntries[i].Count > 0)
                incoming[aEntries[i].Id] = i;

        for (auto it = m_records.begin(); it != m_records.end();)
        {
            if (incoming.find(it->first) == incoming.end())
            {
                if (it->second.BoundFormId != 0)
                {
                    effect.Vanished.push_back(Binding{it->first, it->second.BoundFormId, it->second.Origin});
                    m_byFormId.erase(it->second.BoundFormId);
                }
                it = m_records.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (const auto& [id, index] : incoming)
        {
            SnapshotEntry& entry = aEntries[index];
            const UpsertEffect upsert = Upsert(entry.Id, entry.Count, entry.DropperPlayerId, entry.DropperCookie, entry.Where,
                                               std::move(entry.Payload), aLocalPlayerId, entry.ShownCount);
            if (upsert.StaleFormId != 0)
                effect.Stale.push_back(Binding{id, upsert.StaleFormId, upsert.StaleOrigin});
            if (upsert.AdoptedFormId != 0)
                effect.Adopted.push_back(Binding{id, upsert.AdoptedFormId, BindingOrigin::kDroppedHere});
        }

        m_hasBaseline = true;
        m_revision = aRevision;
        return effect;
    }

    // --- pickups ------------------------------------------------------------------------------

    // Which item a picked-up reference shows (kNoItem: an unsynced reference).
    [[nodiscard]] ItemId FindByFormId(uint32_t aFormId) const noexcept
    {
        if (aFormId == 0)
            return kNoItem;
        const auto it = m_byFormId.find(aFormId);
        return it == m_byFormId.end() ? kNoItem : it->second;
    }

    /**
     * The local engine consumed the reference (whole-stack pickup): forget the binding right away, so
     * neither the server's despawn nor a reused form id can make this client delete another
     * reference. The record stays until the server decides.
     */
    void ReleaseConsumedReference(ItemId aId, uint64_t aNowMs) noexcept
    {
        if (Record* pRecord = FindMutable(aId))
        {
            Unbind(*pRecord);
            // Do not recreate it while the claim is in flight: the despawn (or the remainder upsert)
            // is on its way.
            pRecord->RetryAfterMs = aNowMs + m_policy.ClaimInFlightMs;
        }
    }

    // --- engine bindings ----------------------------------------------------------------------

    bool Bind(ItemId aId, uint32_t aFormId, BindingOrigin aOrigin) noexcept
    {
        Record* pRecord = FindMutable(aId);
        if (!pRecord || aFormId == 0)
            return false;
        if (const auto other = m_byFormId.find(aFormId); other != m_byFormId.end() && other->second != aId)
        {
            // A form id can only show one item; the older binding is the stale one (reused id).
            if (Record* pOther = FindMutable(other->second))
                Unbind(*pOther);
        }
        if (pRecord->BoundFormId != 0)
            m_byFormId.erase(pRecord->BoundFormId);
        pRecord->BoundFormId = aFormId;
        pRecord->Origin = aOrigin;
        pRecord->FailedAttempts = 0;
        pRecord->RetryAfterMs = 0;
        m_byFormId[aFormId] = aId;
        return true;
    }

    // The reference vanished under us (cell unloaded, engine deleted it): show it again later.
    void NoteReferenceLost(ItemId aId) noexcept
    {
        if (Record* pRecord = FindMutable(aId))
            Unbind(*pRecord);
    }

    // Creating the reference failed: retry with back-off, quarantine after repeated failures.
    void NoteMaterializeFailed(ItemId aId, uint64_t aNowMs) noexcept
    {
        Record* pRecord = FindMutable(aId);
        if (!pRecord)
            return;
        ++pRecord->FailedAttempts;
        if (pRecord->FailedAttempts >= m_policy.MaxMaterializeAttempts)
        {
            pRecord->FailedAttempts = 0;
            pRecord->RetryAfterMs = aNowMs + m_policy.MaterializeQuarantineMs;
            ++m_quarantined;
        }
        else
        {
            pRecord->RetryAfterMs = aNowMs + m_policy.MaterializeRetryMs;
        }
    }

    // Ids that are not shown and may be (re)created now, in id order (oldest drops first).
    [[nodiscard]] std::vector<ItemId> CollectUnbound(uint64_t aNowMs) const
    {
        std::vector<ItemId> out;
        for (const auto& [id, record] : m_records)
            if (record.BoundFormId == 0 && record.RetryAfterMs <= aNowMs)
                out.push_back(id);
        std::sort(out.begin(), out.end());
        return out;
    }

    [[nodiscard]] std::vector<Binding> CollectBindings() const
    {
        std::vector<Binding> out;
        for (const auto& [id, record] : m_records)
            if (record.BoundFormId != 0)
                out.push_back(Binding{id, record.BoundFormId, record.Origin});
        std::sort(out.begin(), out.end(), [](const Binding& a, const Binding& b) { return a.Id < b.Id; });
        return out;
    }

    // --- lifecycle ----------------------------------------------------------------------------

    /**
     * The engine world is being torn down (save load): every reference this client knew is gone with
     * it. Records stay (they belong to the server) and are shown again once the new world is loaded;
     * own drops still waiting for their echo lost their reference.
     */
    void ForgetEngineReferences() noexcept
    {
        for (auto& [id, record] : m_records)
        {
            record.BoundFormId = 0;
            record.Origin = BindingOrigin::kNone;
            record.FailedAttempts = 0;
            record.RetryAfterMs = 0;
        }
        m_byFormId.clear();
        m_pendingDrops.clear();
    }

    /**
     * The session ended. Returns the bindings so the service can clean up (delete what it created,
     * hand own drops back to the save game), then forgets everything including the baseline.
     */
    std::vector<Binding> Reset()
    {
        std::vector<Binding> bindings = CollectBindings();
        m_records.clear();
        m_byFormId.clear();
        m_pendingDrops.clear();
        m_hasBaseline = false;
        m_revision = 0;
        return bindings;
    }

    [[nodiscard]] const Record* Find(ItemId aId) const noexcept
    {
        const auto it = m_records.find(aId);
        return it == m_records.end() ? nullptr : &it->second;
    }

    [[nodiscard]] size_t Size() const noexcept { return m_records.size(); }
    [[nodiscard]] size_t BoundCount() const noexcept { return m_byFormId.size(); }
    [[nodiscard]] uint32_t GetQuarantineCount() const noexcept { return m_quarantined; }

private:
    struct PendingDrop
    {
        uint32_t FormId{0};
        uint64_t CreatedMs{0};
    };

    Record* FindMutable(ItemId aId) noexcept
    {
        const auto it = m_records.find(aId);
        return it == m_records.end() ? nullptr : &it->second;
    }

    void Unbind(Record& arRecord) noexcept
    {
        if (arRecord.BoundFormId != 0)
        {
            const auto it = m_byFormId.find(arRecord.BoundFormId);
            if (it != m_byFormId.end() && it->second == arRecord.Id)
                m_byFormId.erase(it);
        }
        arRecord.BoundFormId = 0;
        arRecord.Origin = BindingOrigin::kNone;
        arRecord.FailedAttempts = 0;
        arRecord.RetryAfterMs = 0;
    }

    void TryAdoptPendingDrop(Record& arRecord, uint32_t aLocalPlayerId, UpsertEffect& arEffect)
    {
        if (aLocalPlayerId == 0 || arRecord.DropperPlayerId != aLocalPlayerId || arRecord.DropperCookie == 0)
            return;
        const auto it = m_pendingDrops.find(arRecord.DropperCookie);
        if (it == m_pendingDrops.end())
            return;
        const uint32_t formId = it->second.FormId;
        m_pendingDrops.erase(it);
        if (Bind(arRecord.Id, formId, BindingOrigin::kDroppedHere))
            arEffect.AdoptedFormId = formId;
    }

    ClientPolicy m_policy;
    std::unordered_map<ItemId, Record> m_records{};
    std::unordered_map<uint32_t, ItemId> m_byFormId{};
    std::unordered_map<uint32_t, PendingDrop> m_pendingDrops{};
    bool m_hasBaseline{false};
    uint32_t m_revision{0};
    uint32_t m_gaps{0};
    uint32_t m_duplicates{0};
    uint32_t m_expiredDrops{0};
    uint32_t m_quarantined{0};
};

} // namespace WorldItems
