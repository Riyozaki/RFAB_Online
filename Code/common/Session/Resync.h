#pragma once

/**
 * Self-healing session state (architecture step 1).
 *
 * WHY THIS EXISTS
 * The transport (GameNetworkingSockets reliable channel) already delivers every reliable message
 * exactly once and in order, so transport-level sequence numbers would add nothing. State is lost
 * INSIDE the client instead: a spawn that arrives while the engine is loading a save is dropped, a
 * world reload discards the paused inbound queue, a string-table delta is rejected after a gap, a
 * spawn is refused because a plugin is missing. Before v4.1.0 nothing ever repaired such a loss -
 * the server believed the client had the entity (its interest registry was marked) and never sent
 * it again, so the actor stayed invisible / frozen / unnamed for the rest of the session.
 *
 * CONTRACT
 *  - The client records every gap it detects (`ClientLedger::NoteGap`) with a reason.
 *  - `ClientLedger::Poll` turns gaps into a small number of `RequestResync` messages: deduplicated,
 *    rate-limited, deferred while the world cannot accept state (loading / paused / offline),
 *    escalated to one whole-session resync when many entities are missing, and bounded per entity
 *    so a gap the server cannot fix (missing plugin on this client) never becomes a request loop.
 *  - The server answers every request with authoritative full-state snapshots it already knows how
 *    to build (`CharacterSpawnRequest` / `NotifyRemoveCharacter` / string table from id 0) and then
 *    `NotifyResyncComplete` carrying an `Outcome`. Snapshots are idempotent on the client
 *    (`CharacterService::OnCharacterSpawn` reconciles in place), so a duplicate resend is harmless.
 *  - `ServerBudget` bounds what one player can make the server do, whatever the client sends.
 *
 * THREADING: both classes are single-threaded by design. `ClientLedger` lives on the client main
 * thread (all callers are network handlers / UpdateEvent), `ServerBudget` on the server tick.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Session::Resync
{

enum class Scope : uint8_t
{
    kEntity = 0,       // one character, identified by server id
    kAllEntities = 1,  // every character the server considers in range of this observer
    kStringCache = 2,  // the shared CachedString table, resent from id 0
    kWorldItems = 3,   // the server-owned dropped items (common/Session/WorldItems.h), one snapshot
};
inline constexpr uint8_t kScopeCount = 4;

enum class Reason : uint8_t
{
    kUnknown = 0,
    kSpawnDroppedDuringWorldLoad = 1,
    kUnknownEntityInMovement = 2,
    kStringCacheRejected = 3,
    kWorldPacketsDiscarded = 4,
    kManual = 5,
    kDigestMismatch = 6,
    kWorldItemRevisionGap = 7,  // a dropped-item delta skipped a registry revision
};
inline constexpr uint8_t kReasonCount = 8;

enum class Outcome : uint8_t
{
    kApplied = 0,           // snapshots were sent
    kEntityGone = 1,        // the entity no longer exists / is out of range: a removal was sent
    kOwnedByRequester = 2,  // the server thinks the requester owns it: not fixable by a resend
    kThrottled = 3,         // server budget exhausted, retry later
    kDisabled = 4,          // feature switched off on the server
    kInvalid = 5,           // malformed request
};
inline constexpr uint8_t kOutcomeCount = 6;

// Server ids are entt entity values, and 0 is a perfectly valid one: the first character the server
// creates gets it. "No entity" is therefore the entt::null bit pattern of a 32-bit entity, which is
// never a live id. Whole-session and string-table requests carry it on the wire.
inline constexpr uint32_t kNoServerId = 0xFFFFFFFFu;

// Wire sanitizers: a corrupted byte must map to a harmless value, never index past a table.
[[nodiscard]] constexpr Scope SanitizeScope(uint8_t aRaw) noexcept
{
    return aRaw < kScopeCount ? static_cast<Scope>(aRaw) : Scope::kEntity;
}
[[nodiscard]] constexpr bool IsKnownScope(uint8_t aRaw) noexcept
{
    return aRaw < kScopeCount;
}
[[nodiscard]] constexpr Reason SanitizeReason(uint8_t aRaw) noexcept
{
    return aRaw < kReasonCount ? static_cast<Reason>(aRaw) : Reason::kUnknown;
}
[[nodiscard]] constexpr Outcome SanitizeOutcome(uint8_t aRaw) noexcept
{
    return aRaw < kOutcomeCount ? static_cast<Outcome>(aRaw) : Outcome::kInvalid;
}

[[nodiscard]] constexpr const char* ToString(Scope aScope) noexcept
{
    switch (aScope)
    {
    case Scope::kEntity: return "entity";
    case Scope::kAllEntities: return "all-entities";
    case Scope::kStringCache: return "string-cache";
    case Scope::kWorldItems: return "world-items";
    }
    return "?";
}

[[nodiscard]] constexpr const char* ToString(Reason aReason) noexcept
{
    switch (aReason)
    {
    case Reason::kUnknown: return "unknown";
    case Reason::kSpawnDroppedDuringWorldLoad: return "spawn-dropped-during-world-load";
    case Reason::kUnknownEntityInMovement: return "unknown-entity-in-movement";
    case Reason::kStringCacheRejected: return "string-cache-rejected";
    case Reason::kWorldPacketsDiscarded: return "world-packets-discarded";
    case Reason::kManual: return "manual";
    case Reason::kDigestMismatch: return "digest-mismatch";
    case Reason::kWorldItemRevisionGap: return "world-item-revision-gap";
    }
    return "?";
}

[[nodiscard]] constexpr const char* ToString(Outcome aOutcome) noexcept
{
    switch (aOutcome)
    {
    case Outcome::kApplied: return "applied";
    case Outcome::kEntityGone: return "entity-gone";
    case Outcome::kOwnedByRequester: return "owned-by-requester";
    case Outcome::kThrottled: return "throttled";
    case Outcome::kDisabled: return "disabled";
    case Outcome::kInvalid: return "invalid";
    }
    return "?";
}

struct Request
{
    uint32_t RequestId{0};
    Scope RequestScope{Scope::kEntity};
    uint32_t ServerId{kNoServerId};
    Reason GapReason{Reason::kUnknown};
};

struct ClientPolicy
{
    // An unknown id in a movement frame is only a gap once it persists: unreliable movement can
    // overtake the reliable spawn that introduces the entity.
    uint64_t EntityConfirmDelayMs{1000};
    // After an entity was resent, further gaps on it are expected echoes for this long.
    uint64_t EntityCooldownMs{3000};
    uint64_t AllEntitiesCooldownMs{5000};
    uint64_t StringCacheCooldownMs{5000};
    uint64_t WorldItemsCooldownMs{5000};
    // No NotifyResyncComplete within this window: the reply was lost (world reload discarded the
    // paused queue, ...) - the request is re-armed.
    uint64_t InFlightTimeoutMs{6000};
    // A server id is resent at most this many times per window, then quarantined for the rest of
    // the window. Covers spawns this client deliberately refuses, so they never loop.
    uint32_t MaxEntityAttemptsPerWindow{3};
    uint64_t EntityAttemptWindowMs{60000};
    // After NotifyRemoveCharacter, in-flight movement for the id is expected, not a gap.
    uint64_t RemovalTombstoneMs{4000};
    uint64_t IgnoredEntityMs{60000};
    // More pending entities than this: one whole-session resync is cheaper than N single ones.
    size_t EscalateToAllEntitiesThreshold{12};
    size_t MaxRequestsPerPoll{4};
    // Memory bounds for the bookkeeping maps.
    size_t MaxTrackedEntities{1024};
};

struct ClientStats
{
    std::array<uint32_t, kReasonCount> GapsAccepted{};
    uint32_t GapsAbsorbed{0};
    uint32_t RequestsSent{0};
    uint32_t Completed{0};
    uint32_t TimedOut{0};
    uint32_t Escalations{0};
    uint32_t Quarantined{0};
    uint32_t OwnershipConflicts{0};
    uint32_t Throttled{0};
    uint32_t StaleCompletions{0};
};

class ClientLedger
{
public:
    explicit ClientLedger(ClientPolicy aPolicy = {}) noexcept
        : m_policy(aPolicy)
    {
    }

    /**
     * Records a detected gap. Returns true when it is new information that will lead to a request,
     * false when it is already covered (pending, in flight, cooling down, tombstoned, quarantined).
     * Gaps are never dropped because the client cannot send right now: they wait for `Poll`.
     */
    bool NoteGap(Scope aScope, uint32_t aServerId, Reason aReason, uint64_t aNowMs) noexcept
    {
        switch (aScope)
        {
        case Scope::kAllEntities:
            if (m_pendingAll || HasInFlight(Scope::kAllEntities))
                return Absorb();
            m_pendingAll = true;
            m_pendingAllReason = aReason;
            m_pendingEntities.clear();  // subsumed
            return Accept(aReason);

        case Scope::kStringCache:
            if (m_pendingStrings || HasInFlight(Scope::kStringCache))
                return Absorb();
            m_pendingStrings = true;
            m_pendingStringsReason = aReason;
            return Accept(aReason);

        case Scope::kWorldItems:
            if (m_pendingWorldItems || HasInFlight(Scope::kWorldItems))
                return Absorb();
            m_pendingWorldItems = true;
            m_pendingWorldItemsReason = aReason;
            return Accept(aReason);

        case Scope::kEntity:
            return NoteEntityGap(aServerId, aReason, aNowMs);
        }
        return Absorb();
    }

    // A spawn for the id arrived: any suspicion about it is resolved.
    void NoteEntityKnown(uint32_t aServerId) noexcept
    {
        m_pendingEntities.erase(aServerId);
        m_tombstones.erase(aServerId);
        m_ignored.erase(aServerId);
    }

    // NotifyRemoveCharacter for the id: movement still in flight for it is expected.
    void NoteEntityRemoved(uint32_t aServerId, uint64_t aNowMs) noexcept
    {
        m_pendingEntities.erase(aServerId);
        BoundedInsert(m_tombstones, aServerId, aNowMs + m_policy.RemovalTombstoneMs, aNowMs);
    }

    // The client deliberately refused this entity (e.g. its plugin is not loaded here): a resend
    // would be refused the same way, so it is not a gap for a while.
    void NoteEntityIgnored(uint32_t aServerId, uint64_t aNowMs) noexcept
    {
        m_pendingEntities.erase(aServerId);
        BoundedInsert(m_ignored, aServerId, aNowMs + m_policy.IgnoredEntityMs, aNowMs);
    }

    // Replies to everything in flight were discarded (in-session world reload): re-arm them now
    // instead of waiting for the timeout.
    void AbandonInFlight(uint64_t aNowMs) noexcept
    {
        for (const auto& flight : m_inFlight)
            Rearm(flight, aNowMs);
        m_inFlight.clear();
    }

    /**
     * Emits the requests to send now. `aCanSend` is false while the client cannot accept state
     * (offline, engine loading, feature disabled): gaps keep accumulating and are sent later.
     */
    size_t Poll(uint64_t aNowMs, bool aCanSend, std::vector<Request>& aOut) noexcept
    {
        ExpireInFlight(aNowMs);
        PruneExpired(aNowMs);

        if (!aCanSend)
            return 0;

        size_t emitted = 0;
        const auto budgetLeft = [&] { return emitted < m_policy.MaxRequestsPerPoll; };

        if (m_pendingStrings && !HasInFlight(Scope::kStringCache) &&
            CooledDown(m_lastStringsSentMs, m_policy.StringCacheCooldownMs, aNowMs) && budgetLeft())
        {
            m_pendingStrings = false;
            m_lastStringsSentMs = aNowMs;
            Emit(Scope::kStringCache, kNoServerId, m_pendingStringsReason, aNowMs, aOut);
            ++emitted;
        }

        // Independent of the entity tables: a whole-session resync does not include dropped items.
        if (m_pendingWorldItems && !HasInFlight(Scope::kWorldItems) &&
            CooledDown(m_lastWorldItemsSentMs, m_policy.WorldItemsCooldownMs, aNowMs) && budgetLeft())
        {
            m_pendingWorldItems = false;
            m_lastWorldItemsSentMs = aNowMs;
            Emit(Scope::kWorldItems, kNoServerId, m_pendingWorldItemsReason, aNowMs, aOut);
            ++emitted;
        }

        if (m_pendingAll)
        {
            // Entity requests wait behind a pending whole-session resync: it covers them.
            if (!HasInFlight(Scope::kAllEntities) &&
                CooledDown(m_lastAllSentMs, m_policy.AllEntitiesCooldownMs, aNowMs) && budgetLeft())
            {
                m_pendingAll = false;
                m_pendingEntities.clear();
                m_lastAllSentMs = aNowMs;
                Emit(Scope::kAllEntities, kNoServerId, m_pendingAllReason, aNowMs, aOut);
                ++emitted;
            }
            return emitted;
        }

        if (HasInFlight(Scope::kAllEntities))
            return emitted;

        for (auto it = m_pendingEntities.begin(); it != m_pendingEntities.end() && budgetLeft();)
        {
            const uint32_t serverId = it->first;
            const PendingEntity pending = it->second;
            if (aNowMs < pending.FirstSeenMs + m_policy.EntityConfirmDelayMs)
            {
                ++it;
                continue;
            }

            it = m_pendingEntities.erase(it);

            EntityHistory& history = TouchHistory(serverId, aNowMs);
            if (history.QuarantineUntilMs > aNowMs)
            {
                ++m_stats.GapsAbsorbed;
                continue;
            }
            if (history.Attempts >= m_policy.MaxEntityAttemptsPerWindow)
            {
                history.QuarantineUntilMs = history.WindowStartMs + m_policy.EntityAttemptWindowMs;
                ++m_stats.Quarantined;
                continue;
            }

            ++history.Attempts;
            history.LastSentMs = aNowMs;
            Emit(Scope::kEntity, serverId, pending.GapReason, aNowMs, aOut);
            ++emitted;
        }

        return emitted;
    }

    // Returns false for a completion that matches nothing in flight (late reply after a timeout
    // or a reset) - such a reply is simply ignored.
    bool OnComplete(uint32_t aRequestId, Outcome aOutcome, uint64_t aNowMs) noexcept
    {
        const auto it = std::find_if(m_inFlight.begin(), m_inFlight.end(),
                                     [aRequestId](const InFlight& f) { return f.Id.RequestId == aRequestId; });
        if (it == m_inFlight.end())
        {
            ++m_stats.StaleCompletions;
            return false;
        }

        const InFlight flight = *it;
        m_inFlight.erase(it);
        ++m_stats.Completed;

        switch (aOutcome)
        {
        case Outcome::kApplied:
        case Outcome::kDisabled:
        case Outcome::kInvalid:
            break;
        case Outcome::kEntityGone:
            if (flight.Id.RequestScope == Scope::kEntity)
                NoteEntityRemoved(flight.Id.ServerId, aNowMs);
            break;
        case Outcome::kOwnedByRequester:
            ++m_stats.OwnershipConflicts;
            if (flight.Id.RequestScope == Scope::kEntity)
            {
                EntityHistory& history = TouchHistory(flight.Id.ServerId, aNowMs);
                history.QuarantineUntilMs = aNowMs + m_policy.EntityAttemptWindowMs;
            }
            break;
        case Outcome::kThrottled:
            ++m_stats.Throttled;
            Rearm(flight, aNowMs);
            break;
        }
        return true;
    }

    // Session boundary (connect / disconnect): nothing from the old session is meaningful.
    // Request ids keep increasing so a late reply can never match a new request.
    void Reset() noexcept
    {
        m_pendingAll = false;
        m_pendingStrings = false;
        m_pendingWorldItems = false;
        m_pendingEntities.clear();
        m_inFlight.clear();
        m_history.clear();
        m_tombstones.clear();
        m_ignored.clear();
        m_lastAllSentMs = kNever;
        m_lastStringsSentMs = kNever;
        m_lastWorldItemsSentMs = kNever;
    }

    [[nodiscard]] const ClientStats& GetStats() const noexcept { return m_stats; }
    [[nodiscard]] bool HasPendingAllEntities() const noexcept { return m_pendingAll; }
    [[nodiscard]] bool HasPendingStringCache() const noexcept { return m_pendingStrings; }
    [[nodiscard]] bool HasPendingWorldItems() const noexcept { return m_pendingWorldItems; }
    [[nodiscard]] size_t PendingEntityCount() const noexcept { return m_pendingEntities.size(); }
    [[nodiscard]] size_t InFlightCount() const noexcept { return m_inFlight.size(); }
    [[nodiscard]] size_t TrackedEntityCount() const noexcept { return m_history.size(); }
    [[nodiscard]] bool HasInFlight(Scope aScope) const noexcept
    {
        return std::any_of(m_inFlight.begin(), m_inFlight.end(),
                           [aScope](const InFlight& f) { return f.Id.RequestScope == aScope; });
    }

private:
    static constexpr uint64_t kNever = ~0ull;

    struct PendingEntity
    {
        uint64_t FirstSeenMs{0};
        Reason GapReason{Reason::kUnknown};
    };

    struct EntityHistory
    {
        uint64_t WindowStartMs{0};
        uint32_t Attempts{0};
        uint64_t LastSentMs{kNever};
        uint64_t QuarantineUntilMs{0};
    };

    struct InFlight
    {
        Request Id{};
        uint64_t SentMs{0};
    };

    bool Accept(Reason aReason) noexcept
    {
        ++m_stats.GapsAccepted[static_cast<uint8_t>(aReason) < kReasonCount ? static_cast<uint8_t>(aReason) : 0];
        return true;
    }

    bool Absorb() noexcept
    {
        ++m_stats.GapsAbsorbed;
        return false;
    }

    [[nodiscard]] static bool CooledDown(uint64_t aLastMs, uint64_t aCooldownMs, uint64_t aNowMs) noexcept
    {
        return aLastMs == kNever || aNowMs >= aLastMs + aCooldownMs || aNowMs < aLastMs;
    }

    [[nodiscard]] static bool IsActive(const std::unordered_map<uint32_t, uint64_t>& acMap, uint32_t aId, uint64_t aNowMs) noexcept
    {
        const auto it = acMap.find(aId);
        return it != acMap.end() && it->second > aNowMs;
    }

    bool NoteEntityGap(uint32_t aServerId, Reason aReason, uint64_t aNowMs) noexcept
    {
        if (aServerId == kNoServerId)
            return Absorb();
        if (m_pendingAll || HasInFlight(Scope::kAllEntities))
            return Absorb();
        if (IsActive(m_tombstones, aServerId, aNowMs) || IsActive(m_ignored, aServerId, aNowMs))
            return Absorb();
        if (m_pendingEntities.find(aServerId) != m_pendingEntities.end())
            return Absorb();
        if (std::any_of(m_inFlight.begin(), m_inFlight.end(), [aServerId](const InFlight& f) {
                return f.Id.RequestScope == Scope::kEntity && f.Id.ServerId == aServerId;
            }))
            return Absorb();

        if (const auto histIt = m_history.find(aServerId); histIt != m_history.end())
        {
            const EntityHistory& history = histIt->second;
            if (history.QuarantineUntilMs > aNowMs)
                return Absorb();
            if (history.LastSentMs != kNever && aNowMs < history.LastSentMs + m_policy.EntityCooldownMs)
                return Absorb();
        }

        m_pendingEntities[aServerId] = PendingEntity{aNowMs, aReason};
        if (m_pendingEntities.size() > m_policy.EscalateToAllEntitiesThreshold)
        {
            ++m_stats.Escalations;
            m_pendingEntities.clear();
            m_pendingAll = true;
            m_pendingAllReason = aReason;
        }
        return Accept(aReason);
    }

    void Emit(Scope aScope, uint32_t aServerId, Reason aReason, uint64_t aNowMs, std::vector<Request>& aOut) noexcept
    {
        if (++m_nextRequestId == 0)
            m_nextRequestId = 1;
        Request request{m_nextRequestId, aScope, aServerId, aReason};
        aOut.push_back(request);
        m_inFlight.push_back(InFlight{request, aNowMs});
        ++m_stats.RequestsSent;
    }

    // Puts an unanswered / throttled request back. Entity re-arms skip the confirm delay (the gap
    // was already confirmed once) but still count against the per-window attempt budget in Poll.
    void Rearm(const InFlight& acFlight, uint64_t aNowMs) noexcept
    {
        switch (acFlight.Id.RequestScope)
        {
        case Scope::kAllEntities:
            m_pendingAll = true;
            m_pendingAllReason = acFlight.Id.GapReason;
            m_pendingEntities.clear();
            break;
        case Scope::kStringCache:
            m_pendingStrings = true;
            m_pendingStringsReason = acFlight.Id.GapReason;
            break;
        case Scope::kWorldItems:
            m_pendingWorldItems = true;
            m_pendingWorldItemsReason = acFlight.Id.GapReason;
            break;
        case Scope::kEntity:
            if (!m_pendingAll)
            {
                const uint64_t confirmedAt = aNowMs >= m_policy.EntityConfirmDelayMs ? aNowMs - m_policy.EntityConfirmDelayMs : 0;
                m_pendingEntities[acFlight.Id.ServerId] = PendingEntity{confirmedAt, acFlight.Id.GapReason};
            }
            if (auto histIt = m_history.find(acFlight.Id.ServerId); histIt != m_history.end())
                histIt->second.LastSentMs = kNever;  // the cooldown protects echoes of a reply that never came
            break;
        }
    }

    void ExpireInFlight(uint64_t aNowMs) noexcept
    {
        for (auto it = m_inFlight.begin(); it != m_inFlight.end();)
        {
            if (aNowMs >= it->SentMs + m_policy.InFlightTimeoutMs)
            {
                ++m_stats.TimedOut;
                const InFlight flight = *it;
                it = m_inFlight.erase(it);
                Rearm(flight, aNowMs);
            }
            else
            {
                ++it;
            }
        }
    }

    EntityHistory& TouchHistory(uint32_t aServerId, uint64_t aNowMs) noexcept
    {
        auto it = m_history.find(aServerId);
        if (it == m_history.end())
        {
            if (m_history.size() >= m_policy.MaxTrackedEntities)
                EvictOldestHistory(aNowMs);
            it = m_history.emplace(aServerId, EntityHistory{aNowMs, 0, kNever, 0}).first;
        }
        else if (aNowMs >= it->second.WindowStartMs + m_policy.EntityAttemptWindowMs &&
                 it->second.QuarantineUntilMs <= aNowMs)
        {
            it->second = EntityHistory{aNowMs, 0, it->second.LastSentMs, 0};
        }
        return it->second;
    }

    void EvictOldestHistory(uint64_t aNowMs) noexcept
    {
        PruneExpired(aNowMs);
        if (m_history.size() < m_policy.MaxTrackedEntities || m_history.empty())
            return;
        auto oldest = m_history.begin();
        for (auto it = m_history.begin(); it != m_history.end(); ++it)
        {
            if (it->second.WindowStartMs < oldest->second.WindowStartMs)
                oldest = it;
        }
        m_history.erase(oldest);
    }

    void BoundedInsert(std::unordered_map<uint32_t, uint64_t>& aMap, uint32_t aId, uint64_t aUntilMs, uint64_t aNowMs) noexcept
    {
        if (aMap.size() >= m_policy.MaxTrackedEntities && aMap.find(aId) == aMap.end())
        {
            for (auto it = aMap.begin(); it != aMap.end();)
                it = it->second <= aNowMs ? aMap.erase(it) : std::next(it);
            if (aMap.size() >= m_policy.MaxTrackedEntities)
                aMap.clear();  // worst case: one spurious, rate-limited resync
        }
        aMap[aId] = aUntilMs;
    }

    void PruneExpired(uint64_t aNowMs) noexcept
    {
        for (auto it = m_tombstones.begin(); it != m_tombstones.end();)
            it = it->second <= aNowMs ? m_tombstones.erase(it) : std::next(it);
        for (auto it = m_ignored.begin(); it != m_ignored.end();)
            it = it->second <= aNowMs ? m_ignored.erase(it) : std::next(it);
        for (auto it = m_history.begin(); it != m_history.end();)
        {
            const EntityHistory& h = it->second;
            const bool windowOver = aNowMs >= h.WindowStartMs + m_policy.EntityAttemptWindowMs;
            const bool quarantineOver = h.QuarantineUntilMs <= aNowMs;
            const bool cooldownOver = h.LastSentMs == kNever || aNowMs >= h.LastSentMs + m_policy.EntityCooldownMs;
            it = (windowOver && quarantineOver && cooldownOver) ? m_history.erase(it) : std::next(it);
        }
    }

    ClientPolicy m_policy;
    ClientStats m_stats{};

    bool m_pendingAll{false};
    Reason m_pendingAllReason{Reason::kUnknown};
    bool m_pendingStrings{false};
    Reason m_pendingStringsReason{Reason::kUnknown};
    bool m_pendingWorldItems{false};
    Reason m_pendingWorldItemsReason{Reason::kUnknown};
    std::unordered_map<uint32_t, PendingEntity> m_pendingEntities{};
    std::vector<InFlight> m_inFlight{};
    std::unordered_map<uint32_t, EntityHistory> m_history{};
    std::unordered_map<uint32_t, uint64_t> m_tombstones{};
    std::unordered_map<uint32_t, uint64_t> m_ignored{};
    uint64_t m_lastAllSentMs{kNever};
    uint64_t m_lastStringsSentMs{kNever};
    uint64_t m_lastWorldItemsSentMs{kNever};
    uint32_t m_nextRequestId{0};
};

struct ServerPolicy
{
    uint64_t AllEntitiesMinIntervalMs{2000};
    uint64_t StringCacheMinIntervalMs{2000};
    uint64_t WorldItemsMinIntervalMs{2000};
    uint32_t EntityBurst{24};
    uint64_t EntityRefillIntervalMs{100};  // 10 single-entity resends per second sustained
};

/**
 * Per-player server-side admission. Independent of anything the client claims: a buggy or hostile
 * client looping on RequestResync costs at most one whole-session rebuild per interval.
 */
class ServerBudget
{
public:
    explicit ServerBudget(ServerPolicy aPolicy = {}) noexcept
        : m_policy(aPolicy)
        , m_entityTokens(aPolicy.EntityBurst)
    {
    }

    [[nodiscard]] bool Admit(Scope aScope, uint64_t aNowMs) noexcept
    {
        switch (aScope)
        {
        case Scope::kAllEntities:
            return AdmitInterval(m_lastAllMs, m_policy.AllEntitiesMinIntervalMs, aNowMs);
        case Scope::kStringCache:
            return AdmitInterval(m_lastStringsMs, m_policy.StringCacheMinIntervalMs, aNowMs);
        case Scope::kWorldItems:
            return AdmitInterval(m_lastWorldItemsMs, m_policy.WorldItemsMinIntervalMs, aNowMs);
        case Scope::kEntity:
            Refill(aNowMs);
            if (m_entityTokens == 0)
                return false;
            --m_entityTokens;
            return true;
        }
        return false;
    }

private:
    static constexpr uint64_t kNever = ~0ull;

    static bool AdmitInterval(uint64_t& aLastMs, uint64_t aIntervalMs, uint64_t aNowMs) noexcept
    {
        if (aLastMs != kNever && aNowMs >= aLastMs && aNowMs < aLastMs + aIntervalMs)
            return false;
        aLastMs = aNowMs;
        return true;
    }

    void Refill(uint64_t aNowMs) noexcept
    {
        if (m_lastRefillMs == kNever || aNowMs < m_lastRefillMs || m_policy.EntityRefillIntervalMs == 0)
        {
            m_lastRefillMs = aNowMs;
            return;
        }
        const uint64_t steps = (aNowMs - m_lastRefillMs) / m_policy.EntityRefillIntervalMs;
        if (steps == 0)
            return;
        m_lastRefillMs += steps * m_policy.EntityRefillIntervalMs;
        const uint64_t refilled = std::min<uint64_t>(m_policy.EntityBurst, m_entityTokens + steps);
        m_entityTokens = static_cast<uint32_t>(refilled);
    }

    ServerPolicy m_policy;
    uint64_t m_lastAllMs{kNever};
    uint64_t m_lastStringsMs{kNever};
    uint64_t m_lastWorldItemsMs{kNever};
    uint32_t m_entityTokens;
    uint64_t m_lastRefillMs{kNever};
};

} // namespace Session::Resync
