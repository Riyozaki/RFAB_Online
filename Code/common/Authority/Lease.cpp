#include <Authority/Lease.h>

#include <algorithm>

namespace Authority
{
namespace
{
Lease Grant(const Lease& acCurrent, uint32_t aOwnerId, uint64_t aNowMs, const Policy& acPolicy) noexcept
{
    Lease next{};
    next.OwnerId = aOwnerId;
    next.Epoch = acCurrent.Epoch + 1ull;
    next.GrantedAtMs = aNowMs;
    next.ExpiresAtMs = acPolicy.LeaseDurationMs ? aNowMs + acPolicy.LeaseDurationMs : 0ull;
    return next;
}

Decision Unchanged(const Lease& acCurrent, Outcome aOutcome) noexcept
{
    Decision decision{};
    decision.Result = aOutcome;
    decision.Next = acCurrent;
    decision.Changed = false;
    return decision;
}

Decision Changed(const Lease& acNext, Outcome aOutcome) noexcept
{
    Decision decision{};
    decision.Result = aOutcome;
    decision.Next = acNext;
    decision.Changed = true;
    return decision;
}

// A sender is speaking about the current world when it holds the lease and either
// knows the current epoch or does not track epochs at all.
bool SenderIsCurrent(const Lease& acCurrent, uint32_t aSenderId, uint64_t aSenderEpoch) noexcept
{
    if (aSenderId == kNoOwner || acCurrent.OwnerId != aSenderId)
        return false;
    if (aSenderEpoch != kUnknownEpoch && aSenderEpoch != acCurrent.Epoch)
        return false;
    return true;
}
} // namespace

Decision Claim(const Lease& acCurrent, const Subject& acSubject, const OwnerState& acOwnerState,
               const Candidate& acChallenger, uint64_t aNowMs, const Policy& acPolicy) noexcept
{
    if (acSubject.IsPlayerAvatar || acSubject.IsSummonPinnedToOwner)
        return Unchanged(acCurrent, Outcome::kDeniedPinnedEntity);

    if (acChallenger.PlayerId == kNoOwner)
        return Unchanged(acCurrent, Outcome::kDeniedNoCandidate);

    if (acCurrent.OwnerId == acChallenger.PlayerId && !acCurrent.IsExpired(aNowMs))
        return Unchanged(acCurrent, Outcome::kAlreadyOwner);

    // An owner that lapsed, disconnected or left the area is not an owner any more:
    // the entity is free and the hold window does not apply.
    const bool ownerStillSimulating =
        acCurrent.HasOwner() && !acCurrent.IsExpired(aNowMs) && acOwnerState.CanKeepSimulating();

    if (!ownerStillSimulating)
        return Changed(Grant(acCurrent, acChallenger.PlayerId, aNowMs, acPolicy), Outcome::kGranted);

    // Simulation continuity beats convenience: never move a live entity to a peer
    // that cannot simulate it right now.
    if (acChallenger.IsDowned)
        return Unchanged(acCurrent, Outcome::kDeniedChallengerDowned);

    if (!acChallenger.InRange)
        return Unchanged(acCurrent, Outcome::kDeniedNoCandidate);

    if (!acChallenger.IsPartyLeader && acCurrent.HeldForMs(aNowMs) < acPolicy.MinHoldMs)
        return Unchanged(acCurrent, Outcome::kDeniedHoldWindow);

    return Changed(Grant(acCurrent, acChallenger.PlayerId, aNowMs, acPolicy), Outcome::kGranted);
}

Decision Handoff(const Lease& acCurrent, uint32_t aSenderId, uint64_t aSenderEpoch, const Candidate& acNewOwner,
                 uint64_t aNowMs, const Policy& acPolicy) noexcept
{
    // Epoch first: a sender that quotes an old epoch is describing a world that no
    // longer exists, and saying so is more useful than "you are not the owner" - it
    // usually *was* the owner one epoch ago.
    if (aSenderEpoch != kUnknownEpoch && aSenderEpoch != acCurrent.Epoch)
        return Unchanged(acCurrent, Outcome::kDeniedStaleEpoch);

    if (aSenderId == kNoOwner || acCurrent.OwnerId != aSenderId)
        return Unchanged(acCurrent, Outcome::kDeniedNotOwner);

    if (acNewOwner.PlayerId == kNoOwner)
        return Release(acCurrent, aSenderId, aSenderEpoch, aNowMs);

    if (acNewOwner.PlayerId == acCurrent.OwnerId)
        return Unchanged(acCurrent, Outcome::kAlreadyOwner);

    return Changed(Grant(acCurrent, acNewOwner.PlayerId, aNowMs, acPolicy), Outcome::kGranted);
}

Decision Release(const Lease& acCurrent, uint32_t aSenderId, uint64_t aSenderEpoch, uint64_t aNowMs) noexcept
{
    if (aSenderEpoch != kUnknownEpoch && aSenderEpoch != acCurrent.Epoch)
        return Unchanged(acCurrent, Outcome::kDeniedStaleEpoch);

    if (aSenderId == kNoOwner || acCurrent.OwnerId != aSenderId)
        return Unchanged(acCurrent, Outcome::kDeniedNotOwner);

    Lease next{};
    next.OwnerId = kNoOwner;
    next.Epoch = acCurrent.Epoch + 1ull;
    next.GrantedAtMs = aNowMs;
    next.ExpiresAtMs = 0ull;
    return Changed(next, Outcome::kReleased);
}

Decision Revoke(const Lease& acCurrent, uint64_t aNowMs) noexcept
{
    if (!acCurrent.HasOwner())
        return Unchanged(acCurrent, Outcome::kAlreadyOwner);

    Lease next{};
    next.OwnerId = kNoOwner;
    next.Epoch = acCurrent.Epoch + 1ull;
    next.GrantedAtMs = aNowMs;
    next.ExpiresAtMs = 0ull;
    return Changed(next, Outcome::kReleased);
}

Decision Renew(const Lease& acCurrent, uint32_t aSenderId, uint64_t aNowMs, const Policy& acPolicy) noexcept
{
    if (!SenderIsCurrent(acCurrent, aSenderId, kUnknownEpoch))
        return Unchanged(acCurrent, Outcome::kDeniedNotOwner);

    if (acCurrent.IsExpired(aNowMs))
        return Unchanged(acCurrent, Outcome::kDeniedStaleEpoch);

    if (acPolicy.LeaseDurationMs == 0ull)
        return Unchanged(acCurrent, Outcome::kAlreadyOwner);

    Lease next = acCurrent;
    next.ExpiresAtMs = aNowMs + acPolicy.LeaseDurationMs;
    return Changed(next, Outcome::kGranted);
}

Decision Reassign(const Lease& acCurrent, const Subject& acSubject, const std::vector<Candidate>& acCandidates,
                  const std::vector<uint32_t>& acDeclined, uint64_t aNowMs, const Policy& acPolicy) noexcept
{
    if (acSubject.IsPlayerAvatar || acSubject.IsSummonPinnedToOwner)
        return Unchanged(acCurrent, Outcome::kDeniedPinnedEntity);

    const Candidate* pBest = nullptr;
    int bestScore = -1;

    for (const auto& candidate : acCandidates)
    {
        if (candidate.PlayerId == kNoOwner)
            continue;
        if (std::find(acDeclined.begin(), acDeclined.end(), candidate.PlayerId) != acDeclined.end())
            continue;
        if (candidate.PlayerId == acCurrent.OwnerId)
            continue;

        // In range matters most (it is the only way to simulate at all), then being
        // able to act, then being the host. A standing guest beats a downed host:
        // handing a cell to someone bleeding out on the floor is how encounters stall.
        const int score = (candidate.InRange ? 4 : 0) + (candidate.IsDowned ? 0 : 2) + (candidate.IsPartyLeader ? 1 : 0);
        if (score > bestScore)
        {
            bestScore = score;
            pBest = &candidate;
        }
    }

    if (!pBest || !pBest->InRange)
        return Revoke(acCurrent, aNowMs);

    return Changed(Grant(acCurrent, pBest->PlayerId, aNowMs, acPolicy), Outcome::kGranted);
}

bool LeaseTable::Apply(uint32_t aEntityId, const Lease& acLease) noexcept
{
    const auto it = m_leases.find(aEntityId);
    if (it == m_leases.end())
    {
        m_leases.emplace(aEntityId, acLease);
        return true;
    }

    // Monotonic: an update that describes an older world is dropped. Equal epochs
    // are dropped as well, so a duplicated packet cannot resurrect a stale owner.
    if (acLease.Epoch != kUnknownEpoch && acLease.Epoch <= it->second.Epoch)
        return false;

    it->second = acLease;
    return true;
}

bool LeaseTable::AcceptEpoch(uint32_t aEntityId, uint64_t aEpoch, bool aOwnedLocally) noexcept
{
    const auto it = m_leases.find(aEntityId);
    if (aEpoch == kUnknownEpoch)
    {
        // A peer that does not track epochs: take its word, but do not let it rewrite
        // an epoch we already know.
        if (it == m_leases.end())
        {
            Lease lease{};
            lease.OwnerId = aOwnedLocally ? kLocalOwner : kNoOwner;
            m_leases.emplace(aEntityId, lease);
        }
        else
        {
            it->second.OwnerId = aOwnedLocally ? kLocalOwner : kNoOwner;
        }
        return true;
    }

    if (it != m_leases.end() && aEpoch < it->second.Epoch)
        return false;

    Lease lease = it != m_leases.end() ? it->second : Lease{};
    lease.OwnerId = aOwnedLocally ? kLocalOwner : kNoOwner;
    lease.Epoch = aEpoch;

    if (it == m_leases.end())
        m_leases.emplace(aEntityId, lease);
    else
        it->second = lease;

    return true;
}

const Lease* LeaseTable::Find(uint32_t aEntityId) const noexcept
{
    const auto it = m_leases.find(aEntityId);
    return it != m_leases.end() ? &it->second : nullptr;
}

uint32_t LeaseTable::OwnerOf(uint32_t aEntityId) const noexcept
{
    const auto* pLease = Find(aEntityId);
    return pLease ? pLease->OwnerId : kNoOwner;
}

uint64_t LeaseTable::EpochOf(uint32_t aEntityId) const noexcept
{
    const auto* pLease = Find(aEntityId);
    return pLease ? pLease->Epoch : kUnknownEpoch;
}

bool LeaseTable::IsOwnedBy(uint32_t aEntityId, uint32_t aPlayerId) const noexcept
{
    return aPlayerId != kNoOwner && OwnerOf(aEntityId) == aPlayerId;
}

size_t LeaseTable::ExpireBefore(uint64_t aNowMs) noexcept
{
    size_t removed = 0;
    for (auto it = m_leases.begin(); it != m_leases.end();)
    {
        if (it->second.IsExpired(aNowMs))
        {
            it = m_leases.erase(it);
            ++removed;
        }
        else
        {
            ++it;
        }
    }
    return removed;
}

void LeaseTable::Forget(uint32_t aEntityId) noexcept
{
    m_leases.erase(aEntityId);
}

void LeaseTable::Clear() noexcept
{
    m_leases.clear();
}
} // namespace Authority
