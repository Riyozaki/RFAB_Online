#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// =====================================================================================
//  Simulation authority: one model, one transition point.
// =====================================================================================
//
//  Who simulates an entity is described by exactly one value, a Lease:
//
//      { OwnerId, Epoch, GrantedAtMs, ExpiresAtMs }
//
//  * OwnerId     - the peer running the AI, physics and animation for that entity.
//                  kNoOwner means the server holds it and nobody simulates it.
//  * Epoch       - monotonic counter, incremented by every accepted transition.
//                  It is the only tie-breaker on the wire: a packet carrying an older
//                  epoch describes a world that no longer exists and is dropped.
//  * GrantedAtMs - when the current owner got the lease; the anti-flap hold window
//                  is measured from here.
//  * ExpiresAtMs - when the lease lapses on its own (0 = never). A lapsed lease is
//                  equivalent to "no owner", which is what replaces ad-hoc timers:
//                  the owner does not need to be detected as gone, the lease simply
//                  stops being valid.
//
//  Every transition is a pure function of (current lease, request, clock, policy).
//  Nothing in here reads a game object, allocates, or touches the network, so the
//  whole authority model is exercised by the Linux test loop in milliseconds.
//
//  Callers never mutate a lease by hand and never bump an epoch themselves: they ask
//  for a Decision and store Decision::Next when Decision::Changed is true. That is
//  what makes "two owners at once" and "zero owners" unrepresentable instead of
//  merely unlikely.
//
namespace Authority
{
inline constexpr uint32_t kNoOwner = 0u;

// An epoch of 0 on the wire means "sender does not know the epoch" (older clients,
// or a first contact). Such packets are accepted on their other merits rather than
// being rejected outright, so a mixed-version session still converges.
inline constexpr uint64_t kUnknownEpoch = 0ull;

// Owner id a client uses for "this entity is simulated here". Clients do not need the
// server's player ids to order their own grants and relinquishes.
inline constexpr uint32_t kLocalOwner = 0xFFFFFFFFu;

struct Lease
{
    uint32_t OwnerId{kNoOwner};
    uint64_t Epoch{kUnknownEpoch};
    uint64_t GrantedAtMs{0};
    uint64_t ExpiresAtMs{0};

    [[nodiscard]] bool HasOwner() const noexcept { return OwnerId != kNoOwner; }

    [[nodiscard]] bool IsExpired(uint64_t aNowMs) const noexcept
    {
        return ExpiresAtMs != 0 && aNowMs >= ExpiresAtMs;
    }

    // Live = somebody owns it and the lease has not lapsed.
    [[nodiscard]] bool IsLive(uint64_t aNowMs) const noexcept { return HasOwner() && !IsExpired(aNowMs); }

    [[nodiscard]] uint64_t HeldForMs(uint64_t aNowMs) const noexcept
    {
        return aNowMs > GrantedAtMs ? aNowMs - GrantedAtMs : 0ull;
    }

    friend bool operator==(const Lease& acLhs, const Lease& acRhs) noexcept
    {
        return acLhs.OwnerId == acRhs.OwnerId && acLhs.Epoch == acRhs.Epoch &&
               acLhs.GrantedAtMs == acRhs.GrantedAtMs && acLhs.ExpiresAtMs == acRhs.ExpiresAtMs;
    }
    friend bool operator!=(const Lease& acLhs, const Lease& acRhs) noexcept { return !(acLhs == acRhs); }
};

struct Policy
{
    // Anti-flap: how long a fresh owner is protected from voluntary takeovers.
    uint64_t MinHoldMs{1500};

    // Lease lifetime. 0 keeps the historical behaviour (a lease is held until the
    // owner disconnects or walks out of range); a non-zero value makes authority
    // self-healing: a peer that stops renewing simply loses the entity.
    uint64_t LeaseDurationMs{0};

    // How long an entity that just became unowned is left alone before the server
    // offers it to an observer. Without it a dormant NPC ping-pongs between peers
    // on every reconcile tick. This used to be a bare 1000ULL repeated at the call
    // sites that happened to remember it.
    uint64_t DormantSettleMs{1000};
};

// What the entity is. Some entities are structurally pinned to one peer and can
// never change hands: a player's own avatar, and a summon bound to its summoner.
struct Subject
{
    bool IsPlayerAvatar{false};
    bool IsSummonPinnedToOwner{false};
};

// What the server currently knows about the peer holding the lease.
struct OwnerState
{
    bool Connected{true};
    bool InRange{true};

    [[nodiscard]] bool CanKeepSimulating() const noexcept { return Connected && InRange; }
};

// The peer asking for the lease.
struct Candidate
{
    uint32_t PlayerId{kNoOwner};
    bool IsDowned{false};
    bool InRange{true};

    // The host/party leader may take a calm world actor from a faster-loading guest
    // without waiting out the hold window; it cannot ping-pong because every other
    // peer is still rejected while the leader keeps the entity in range.
    bool IsPartyLeader{false};
};

enum class Outcome : uint8_t
{
    kGranted = 0,              // lease moved to the requester
    kReleased,                 // lease returned to the server (kNoOwner)
    kAlreadyOwner,             // requester already holds it; nothing changed
    kDeniedPinnedEntity,       // player avatar or bound summon
    kDeniedChallengerDowned,   // a downed peer may not take over a live owner
    kDeniedHoldWindow,         // owner is fresh, in range and connected
    kDeniedNotOwner,           // handoff/release from a peer that does not hold the lease
    kDeniedStaleEpoch,         // request describes an older world state
    kDeniedNoCandidate,        // nobody is eligible to take the entity
};

struct Decision
{
    Outcome Result{Outcome::kDeniedNoCandidate};
    Lease Next{};
    bool Changed{false};

    [[nodiscard]] bool Accepted() const noexcept
    {
        return Result == Outcome::kGranted || Result == Outcome::kReleased;
    }
};

// A peer voluntarily asks to simulate an entity.
[[nodiscard]] Decision Claim(const Lease& acCurrent, const Subject& acSubject, const OwnerState& acOwnerState,
                             const Candidate& acChallenger, uint64_t aNowMs, const Policy& acPolicy = {}) noexcept;

// The current owner hands the entity to a specific peer (cell unload, scripted transfer).
// A late packet from a peer that already lost the lease must not move it: that is the
// bug that wiped live bosses when an unloading client's RequestOwnershipTransfer
// arrived after the entity had been reassigned.
[[nodiscard]] Decision Handoff(const Lease& acCurrent, const Subject& acSubject, uint32_t aSenderId,
                               uint64_t aSenderEpoch, const Candidate& acNewOwner, uint64_t aNowMs,
                               const Policy& acPolicy = {}) noexcept;

// The current owner gives the entity back to the server. Pinned subjects are refused
// here too: a player's own character has exactly one owner for its whole life, and a
// client asking to hand it back (by accident or on purpose) used to leave it ownerless.
[[nodiscard]] Decision Release(const Lease& acCurrent, const Subject& acSubject, uint32_t aSenderId,
                               uint64_t aSenderEpoch, uint64_t aNowMs) noexcept;

// The server takes the entity away: disconnect, out of range, or a lapsed lease.
[[nodiscard]] Decision Revoke(const Lease& acCurrent, uint64_t aNowMs) noexcept;

// Extends the current owner's lease without changing hands. Returns an unchanged
// decision when the sender is not the owner or the lease already lapsed.
[[nodiscard]] Decision Renew(const Lease& acCurrent, uint32_t aSenderId, uint64_t aNowMs,
                             const Policy& acPolicy = {}) noexcept;

// May the server hand this unowned entity to an observer yet? False while the
// entity is still settling, and for anything that already has an owner.
[[nodiscard]] bool CanAdoptDormant(const Lease& acCurrent, uint64_t aNowMs, const Policy& acPolicy = {}) noexcept;

// Picks the next owner after the previous one went away. Candidates that already
// refused the entity (or failed to load it) are passed in acDeclined, which replaces
// the hand-maintained InvalidOwners list: in-range peers win over out-of-range ones,
// party leaders win over regular peers, downed peers are a last resort.
[[nodiscard]] Decision Reassign(const Lease& acCurrent, const Subject& acSubject,
                                const std::vector<Candidate>& acCandidates,
                                const std::vector<uint32_t>& acDeclined, uint64_t aNowMs,
                                const Policy& acPolicy = {}) noexcept;

// =====================================================================================
//  LeaseTable - the receiving side of the same model.
// =====================================================================================
//
//  Both the client (for remote entities) and any mirror of server state apply leases
//  they are told about. Application is monotonic per entity: an update carrying an
//  older epoch is dropped, so Grant/Relinquish packets that arrive out of order can
//  never leave two owners or no owner behind.
//
class LeaseTable
{
public:
    // Returns true when the table changed. Epoch 0 means "unknown" and is accepted
    // once, for first contact with a peer that does not track epochs.
    bool Apply(uint32_t aEntityId, const Lease& acLease) noexcept;

    // Client-side shorthand. A client learns only "the entity is mine now" or "it is
    // not mine any more", plus the epoch that orders the two. Equal epochs are accepted
    // so a re-sent grant still applies; older ones are dropped, which is what stops a
    // late Relinquish from undoing a newer Grant.
    bool AcceptEpoch(uint32_t aEntityId, uint64_t aEpoch, bool aOwnedLocally) noexcept;

    [[nodiscard]] const Lease* Find(uint32_t aEntityId) const noexcept;
    [[nodiscard]] uint32_t OwnerOf(uint32_t aEntityId) const noexcept;
    [[nodiscard]] uint64_t EpochOf(uint32_t aEntityId) const noexcept;
    [[nodiscard]] bool IsOwnedBy(uint32_t aEntityId, uint32_t aPlayerId) const noexcept;

    // Drops entities whose lease lapsed; returns how many were removed.
    size_t ExpireBefore(uint64_t aNowMs) noexcept;

    void Forget(uint32_t aEntityId) noexcept;
    void Clear() noexcept;
    [[nodiscard]] size_t Size() const noexcept { return m_leases.size(); }

private:
    std::unordered_map<uint32_t, Lease> m_leases;
};
} // namespace Authority
