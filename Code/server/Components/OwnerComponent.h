#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Authority/Lease.h>

struct Player;

// Defined in Components/OwnerComponent.cpp, where Player is a complete type.
// Returns Authority::kNoOwner for nullptr.
[[nodiscard]] uint32_t GetOwnerPlayerId(const Player* apPlayer) noexcept;

// Server-side binding between an entity and the peer that simulates it.
//
// The authoritative state is the Authority::Lease: who owns the entity, on which
// epoch, since when and until when. pOwner is only a resolved pointer to the peer
// named by the lease, kept because the rest of the server speaks in Player*.
//
// Nothing outside this component writes the lease. Call sites ask Authority for a
// Decision and hand it here, which is what keeps the epoch monotonic and makes
// "two owners" or "silently no owner" impossible to express.
struct OwnerComponent
{
    OwnerComponent(Player* apPlayer, uint64_t aNowMs = 0, const Authority::Policy& acPolicy = {})
    {
        ForceOwner(apPlayer, aNowMs, acPolicy);
    }

    [[nodiscard]] Player* GetOwner() const noexcept { return pOwner; }
    [[nodiscard]] const Authority::Lease& GetLease() const noexcept { return CurrentLease; }
    [[nodiscard]] uint64_t GetEpoch() const noexcept { return CurrentLease.Epoch; }

    // When the current owner took over; the anti-flap hold window is measured from here.
    [[nodiscard]] uint64_t GetGrantedAtMs() const noexcept { return CurrentLease.GrantedAtMs; }

    // Applies a decision produced by Authority::Claim/Handoff/Release/Revoke/Reassign.
    // apNewOwner must be the peer named by acDecision.Next (nullptr when it released).
    uint64_t Apply(const Authority::Decision& acDecision, Player* apNewOwner) noexcept
    {
        if (!acDecision.Changed)
            return CurrentLease.Epoch;

        CurrentLease = acDecision.Next;
        pOwner = apNewOwner;
        return CurrentLease.Epoch;
    }

    // Refreshes the lease window for the current unrevoked owner when simulation activity arrives.
    bool RefreshLease(uint64_t aNowMs, const Authority::Policy& acPolicy) noexcept
    {
        if (!pOwner)
            return false;

        const auto decision = Authority::RefreshUnrevoked(
            CurrentLease,
            GetOwnerPlayerId(pOwner),
            aNowMs,
            acPolicy);
        if (!decision.Changed)
            return false;

        CurrentLease = decision.Next;
        return true;
    }

    // Server-side override used when the server itself decides the owner (an observer
    // was picked, a reload reassigned the entity, the owner disconnected). Keeps the
    // epoch monotonic and never produces a transition when nothing actually changed.
    uint64_t ForceOwner(Player* apPlayer, uint64_t aNowMs = 0, const Authority::Policy& acPolicy = {}) noexcept
    {
        const uint32_t ownerId = GetOwnerPlayerId(apPlayer);
        const uint64_t expiresAt = (apPlayer && acPolicy.LeaseDurationMs != 0ull)
            ? (aNowMs + acPolicy.LeaseDurationMs)
            : 0ull;

        if (pOwner == apPlayer && CurrentLease.Epoch != Authority::kUnknownEpoch)
        {
            CurrentLease.ExpiresAtMs = expiresAt;
            return CurrentLease.Epoch;
        }

        pOwner = apPlayer;
        ++CurrentLease.Epoch;
        CurrentLease.OwnerId = ownerId;
        CurrentLease.GrantedAtMs = aNowMs;
        CurrentLease.ExpiresAtMs = expiresAt;
        return CurrentLease.Epoch;
    }

    // Historical name kept so call sites read the same; identical semantics.
    uint64_t SetOwner(Player* apPlayer, uint64_t aNowMs = 0, const Authority::Policy& acPolicy = {}) noexcept
    {
        return ForceOwner(apPlayer, aNowMs, acPolicy);
    }

    Authority::Lease CurrentLease{};
    Player* pOwner{nullptr};

    // Time-bounded zero-pointer decline set replacing raw Vector<const Player*>.
    Authority::DeclineList InvalidOwners{};
};
