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
    OwnerComponent(Player* apPlayer, uint64_t aNowMs = 0) { ForceOwner(apPlayer, aNowMs); }

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
        InvalidOwners.clear();
        return CurrentLease.Epoch;
    }

    // Server-side override used when the server itself decides the owner (an observer
    // was picked, a reload reassigned the entity, the owner disconnected). Keeps the
    // epoch monotonic and never produces a transition when nothing actually changed.
    uint64_t ForceOwner(Player* apPlayer, uint64_t aNowMs = 0) noexcept
    {
        if (pOwner == apPlayer && CurrentLease.Epoch != Authority::kUnknownEpoch)
            return CurrentLease.Epoch;

        pOwner = apPlayer;
        ++CurrentLease.Epoch;
        CurrentLease.OwnerId = GetOwnerPlayerId(apPlayer);
        CurrentLease.GrantedAtMs = aNowMs;
        CurrentLease.ExpiresAtMs = 0ull;
        return CurrentLease.Epoch;
    }

    // Historical name kept so call sites read the same; identical semantics.
    uint64_t SetOwner(Player* apPlayer, uint64_t aNowMs = 0) noexcept { return ForceOwner(apPlayer, aNowMs); }

    Authority::Lease CurrentLease{};
    Player* pOwner{nullptr};

    // Peers that cannot take this entity right now (failed to load it, refused it).
    // Authority::Reassign consumes this as its declined list.
    Vector<const Player*> InvalidOwners{};
};
