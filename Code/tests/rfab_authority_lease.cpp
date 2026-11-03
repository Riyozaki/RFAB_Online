/*
 * Simulation authority — behavioural suite for the unified lease model.
 *
 * The project used to carry three overlapping mechanisms for "who simulates this actor":
 * the server's OwnerComponent (owner pointer + epoch + LastChangedAtMs + InvalidOwners),
 * the CoopOwnership policy helpers, and the client's ClientEpochTracker. Each one knew a
 * part of the rules, every call site re-implemented the transition by hand, and the gaps
 * between them produced the symptoms players reported: two peers animating the same NPC,
 * an NPC nobody simulates, a boss deleted by a late packet from its previous owner.
 *
 * Authority::Lease replaces all three. This suite pins the behaviour of every transition,
 * including the ones that used to be "obviously fine" and were not.
 */

#include <Authority/Lease.h>

#include <catch2/catch.hpp>

using namespace Authority;

namespace
{
constexpr uint32_t kHost = 1u;
constexpr uint32_t kGuest = 2u;
constexpr uint32_t kThird = 3u;

Lease OwnedBy(uint32_t aOwner, uint64_t aEpoch, uint64_t aGrantedAtMs, uint64_t aExpiresAtMs = 0ull)
{
    Lease lease{};
    lease.OwnerId = aOwner;
    lease.Epoch = aEpoch;
    lease.GrantedAtMs = aGrantedAtMs;
    lease.ExpiresAtMs = aExpiresAtMs;
    return lease;
}

Candidate Peer(uint32_t aId)
{
    Candidate candidate{};
    candidate.PlayerId = aId;
    return candidate;
}

constexpr Subject kWorldActor{};
constexpr OwnerState kOwnerPresent{true, true};
} // namespace

TEST_CASE("Authority: an unowned entity is granted to the first caller", "[rfab][authority][claim]")
{
    const Lease free{};
    REQUIRE_FALSE(free.HasOwner());

    const auto decision = Claim(free, kWorldActor, kOwnerPresent, Peer(kGuest), 1000ull);

    CHECK(decision.Result == Outcome::kGranted);
    CHECK(decision.Changed);
    CHECK(decision.Next.OwnerId == kGuest);
    CHECK(decision.Next.Epoch == 1ull);
    CHECK(decision.Next.GrantedAtMs == 1000ull);
    CHECK(decision.Next.ExpiresAtMs == 0ull);
}

TEST_CASE("Authority: every accepted transition advances the epoch exactly once",
          "[rfab][authority][epoch]")
{
    Lease lease{};
    uint64_t expected = 0ull;

    for (int i = 0; i < 8; ++i)
    {
        const uint64_t now = 10'000ull * static_cast<uint64_t>(i + 1);
        const auto grant = Claim(lease, kWorldActor, kOwnerPresent, Peer(kGuest), now);
        REQUIRE(grant.Result == Outcome::kGranted);
        lease = grant.Next;
        CHECK(lease.Epoch == ++expected);

        const auto release = Release(lease, kGuest, lease.Epoch, now + 1ull);
        REQUIRE(release.Result == Outcome::kReleased);
        lease = release.Next;
        CHECK(lease.Epoch == ++expected);
        CHECK_FALSE(lease.HasOwner());
    }
}

TEST_CASE("Authority: rejected requests never touch the lease", "[rfab][authority][claim]")
{
    const Lease held = OwnedBy(kHost, 7ull, 1000ull);

    const Outcome rejected[] = {
        Claim(held, Subject{true, false}, kOwnerPresent, Peer(kGuest), 9000ull).Result,
        Claim(held, Subject{false, true}, kOwnerPresent, Peer(kGuest), 9000ull).Result,
        Claim(held, kWorldActor, kOwnerPresent, Peer(kGuest), 1100ull).Result,
        Handoff(held, kGuest, 7ull, Peer(kThird), 9000ull).Result,
        Handoff(held, kHost, 6ull, Peer(kThird), 9000ull).Result,
        Release(held, kGuest, 7ull, 9000ull).Result,
    };

    for (const auto outcome : rejected)
    {
        CHECK(outcome != Outcome::kGranted);
        CHECK(outcome != Outcome::kReleased);
    }

    // The point of the table above: not one of those calls produced a new lease.
    CHECK(Claim(held, Subject{true, false}, kOwnerPresent, Peer(kGuest), 9000ull).Next == held);
    CHECK(Handoff(held, kGuest, 7ull, Peer(kThird), 9000ull).Next == held);
    CHECK(Release(held, kGuest, 7ull, 9000ull).Next == held);
}

TEST_CASE("Authority: player avatars and bound summons never change hands", "[rfab][authority][pinned]")
{
    const Lease held = OwnedBy(kHost, 3ull, 0ull);

    const auto avatar = Claim(held, Subject{true, false}, kOwnerPresent, Peer(kGuest), 100'000ull);
    CHECK(avatar.Result == Outcome::kDeniedPinnedEntity);
    CHECK_FALSE(avatar.Changed);

    const auto summon = Claim(held, Subject{false, true}, kOwnerPresent, Peer(kGuest), 100'000ull);
    CHECK(summon.Result == Outcome::kDeniedPinnedEntity);
    CHECK_FALSE(summon.Changed);

    const auto reassign = Reassign(held, Subject{true, false}, {Peer(kGuest)}, {}, 100'000ull);
    CHECK(reassign.Result == Outcome::kDeniedPinnedEntity);
    CHECK_FALSE(reassign.Changed);
}

TEST_CASE("Authority: the hold window stops ownership ping-pong", "[rfab][authority][holdwindow]")
{
    const Policy policy{1500ull, 0ull};
    const Lease held = OwnedBy(kHost, 2ull, 1000ull);

    CHECK(Claim(held, kWorldActor, kOwnerPresent, Peer(kGuest), 1100ull, policy).Result ==
          Outcome::kDeniedHoldWindow);
    CHECK(Claim(held, kWorldActor, kOwnerPresent, Peer(kGuest), 2499ull, policy).Result ==
          Outcome::kDeniedHoldWindow);
    CHECK(Claim(held, kWorldActor, kOwnerPresent, Peer(kGuest), 2500ull, policy).Result == Outcome::kGranted);
    CHECK(Claim(held, kWorldActor, kOwnerPresent, Peer(kGuest), 9000ull, policy).Result == Outcome::kGranted);
}

TEST_CASE("Authority: the party leader takes a calm actor from a faster-loading guest",
          "[rfab][authority][holdwindow]")
{
    const Lease held = OwnedBy(kGuest, 4ull, 1000ull);

    Candidate leader = Peer(kHost);
    leader.IsPartyLeader = true;

    const auto decision = Claim(held, kWorldActor, kOwnerPresent, leader, 1050ull);
    CHECK(decision.Result == Outcome::kGranted);
    CHECK(decision.Next.OwnerId == kHost);

    // ...and the guest cannot immediately take it back: only the leader bypasses the window.
    CHECK(Claim(decision.Next, kWorldActor, kOwnerPresent, Peer(kGuest), 1100ull).Result ==
          Outcome::kDeniedHoldWindow);
}

TEST_CASE("Authority: a downed peer cannot take a live owner's entity", "[rfab][authority][downed]")
{
    const Lease held = OwnedBy(kHost, 5ull, 1000ull);

    Candidate downed = Peer(kGuest);
    downed.IsDowned = true;

    CHECK(Claim(held, kWorldActor, kOwnerPresent, downed, 50'000ull).Result ==
          Outcome::kDeniedChallengerDowned);

    // A downed owner keeps simulating: bleeding out is not losing authority, which is what
    // keeps a boss fight running while one player is on the floor.
    Candidate healthy = Peer(kThird);
    CHECK(Claim(held, kWorldActor, kOwnerPresent, healthy, 1100ull).Result == Outcome::kDeniedHoldWindow);

    // But an unowned entity may go to a downed peer rather than to nobody.
    CHECK(Claim(Lease{}, kWorldActor, kOwnerPresent, downed, 50'000ull).Result == Outcome::kGranted);
}

TEST_CASE("Authority: a disconnected or out-of-range owner holds nothing", "[rfab][authority][claim]")
{
    const Lease held = OwnedBy(kHost, 6ull, 1000ull);

    const OwnerState disconnected{false, true};
    const OwnerState outOfRange{true, false};

    CHECK(Claim(held, kWorldActor, disconnected, Peer(kGuest), 1010ull).Result == Outcome::kGranted);
    CHECK(Claim(held, kWorldActor, outOfRange, Peer(kGuest), 1010ull).Result == Outcome::kGranted);

    // Even inside the hold window, because there is nothing left to protect.
    CHECK(Claim(held, kWorldActor, disconnected, Peer(kGuest), 1000ull).Changed);
}

TEST_CASE("Authority: a late packet from a former owner cannot move a live entity",
          "[rfab][authority][handoff][regression]")
{
    // Reproduces the crash pattern: a client unloads a cell and sends a transfer request,
    // the server has already reassigned the boss, and the late packet used to be applied
    // anyway - wiping the owner list and deleting a living boss.
    const Lease current = OwnedBy(kThird, 9ull, 5000ull);

    const auto stale = Handoff(current, kHost, 8ull, Peer(kGuest), 6000ull);
    CHECK(stale.Result == Outcome::kDeniedStaleEpoch);
    CHECK(stale.Next == current);

    const auto notOwner = Handoff(current, kGuest, kUnknownEpoch, Peer(kHost), 6000ull);
    CHECK(notOwner.Result == Outcome::kDeniedNotOwner);
    CHECK(notOwner.Next == current);

    const auto staleRelease = Release(current, kHost, 8ull, 6000ull);
    CHECK(staleRelease.Result == Outcome::kDeniedStaleEpoch);
    CHECK(staleRelease.Next == current);
}

TEST_CASE("Authority: a handoff from the current owner moves the lease", "[rfab][authority][handoff]")
{
    const Lease current = OwnedBy(kHost, 4ull, 1000ull);

    const auto decision = Handoff(current, kHost, 4ull, Peer(kGuest), 2000ull);
    CHECK(decision.Result == Outcome::kGranted);
    CHECK(decision.Next.OwnerId == kGuest);
    CHECK(decision.Next.Epoch == 5ull);
    CHECK(decision.Next.GrantedAtMs == 2000ull);

    // A handoff to nobody is a release.
    const auto toServer = Handoff(current, kHost, 4ull, Peer(kNoOwner), 2000ull);
    CHECK(toServer.Result == Outcome::kReleased);
    CHECK_FALSE(toServer.Next.HasOwner());
    CHECK(toServer.Next.Epoch == 5ull);
}

TEST_CASE("Authority: peers that do not track epochs still interoperate", "[rfab][authority][compat]")
{
    const Lease current = OwnedBy(kHost, 11ull, 1000ull);

    const auto decision = Handoff(current, kHost, kUnknownEpoch, Peer(kGuest), 2000ull);
    CHECK(decision.Result == Outcome::kGranted);
    CHECK(decision.Next.Epoch == 12ull);

    CHECK(Release(current, kHost, kUnknownEpoch, 2000ull).Result == Outcome::kReleased);
}

TEST_CASE("Authority: a lease that lapses frees the entity without any timer",
          "[rfab][authority][lease]")
{
    const Policy policy{1500ull, 5000ull};

    const auto granted = Claim(Lease{}, kWorldActor, kOwnerPresent, Peer(kHost), 1000ull, policy);
    REQUIRE(granted.Result == Outcome::kGranted);
    const Lease lease = granted.Next;
    CHECK(lease.ExpiresAtMs == 6000ull);
    CHECK(lease.IsLive(5999ull));
    CHECK_FALSE(lease.IsLive(6000ull));

    // Before expiry the hold window still protects the owner.
    CHECK(Claim(lease, kWorldActor, kOwnerPresent, Peer(kGuest), 1200ull, policy).Result ==
          Outcome::kDeniedHoldWindow);

    // After expiry the entity is free even though the owner is still connected and in range.
    const auto afterExpiry = Claim(lease, kWorldActor, kOwnerPresent, Peer(kGuest), 6001ull, policy);
    CHECK(afterExpiry.Result == Outcome::kGranted);
    CHECK(afterExpiry.Next.OwnerId == kGuest);

    // The owner keeps it by renewing, which is one message instead of a bespoke timer.
    const auto renewed = Renew(lease, kHost, 5500ull, policy);
    CHECK(renewed.Result == Outcome::kGranted);
    CHECK(renewed.Next.ExpiresAtMs == 10'500ull);
    CHECK(renewed.Next.Epoch == lease.Epoch);
    CHECK(renewed.Next.OwnerId == kHost);

    CHECK(Renew(lease, kGuest, 5500ull, policy).Result == Outcome::kDeniedNotOwner);
    CHECK(Renew(lease, kHost, 6001ull, policy).Result == Outcome::kDeniedStaleEpoch);
}

TEST_CASE("Authority: an expired owner does not count as owner for its own claim",
          "[rfab][authority][lease]")
{
    const Policy policy{1500ull, 5000ull};
    const Lease lapsed = OwnedBy(kHost, 2ull, 1000ull, 6000ull);

    const auto decision = Claim(lapsed, kWorldActor, kOwnerPresent, Peer(kHost), 7000ull, policy);
    CHECK(decision.Result == Outcome::kGranted);
    CHECK(decision.Next.Epoch == 3ull);
    CHECK(decision.Next.ExpiresAtMs == 12'000ull);
}

TEST_CASE("Authority: revoking on disconnect leaves the entity to the server", "[rfab][authority][revoke]")
{
    const Lease held = OwnedBy(kGuest, 3ull, 1000ull);

    const auto decision = Revoke(held, 4000ull);
    CHECK(decision.Result == Outcome::kReleased);
    CHECK(decision.Changed);
    CHECK(decision.Next.OwnerId == kNoOwner);
    CHECK(decision.Next.Epoch == 4ull);

    // Revoking twice is a no-op, so a double disconnect notification cannot skip an epoch.
    const auto again = Revoke(decision.Next, 5000ull);
    CHECK_FALSE(again.Changed);
    CHECK(again.Next.Epoch == 4ull);
}

TEST_CASE("Authority: reassignment prefers peers that can actually simulate",
          "[rfab][authority][reassign]")
{
    const Lease orphan = OwnedBy(kNoOwner, 10ull, 1000ull);

    Candidate farLeader = Peer(kHost);
    farLeader.IsPartyLeader = true;
    farLeader.InRange = false;

    Candidate nearGuest = Peer(kGuest);
    Candidate nearDowned = Peer(kThird);
    nearDowned.IsDowned = true;

    const auto decision = Reassign(orphan, kWorldActor, {farLeader, nearDowned, nearGuest}, {}, 2000ull);
    CHECK(decision.Result == Outcome::kGranted);
    CHECK(decision.Next.OwnerId == kGuest);
    CHECK(decision.Next.Epoch == 11ull);

    // A downed peer in range still beats nobody at all.
    const auto onlyDowned = Reassign(orphan, kWorldActor, {farLeader, nearDowned}, {}, 2000ull);
    CHECK(onlyDowned.Result == Outcome::kGranted);
    CHECK(onlyDowned.Next.OwnerId == kThird);
}

TEST_CASE("Authority: peers that already refused an entity are skipped", "[rfab][authority][reassign]")
{
    const Lease orphan = OwnedBy(kNoOwner, 1ull, 0ull);

    const auto decision = Reassign(orphan, kWorldActor, {Peer(kGuest), Peer(kThird)}, {kGuest}, 500ull);
    CHECK(decision.Result == Outcome::kGranted);
    CHECK(decision.Next.OwnerId == kThird);

    // Everyone refused: the server keeps the entity instead of forcing it on a peer.
    const auto nobody = Reassign(orphan, kWorldActor, {Peer(kGuest), Peer(kThird)}, {kGuest, kThird}, 500ull);
    CHECK(nobody.Next.OwnerId == kNoOwner);
    CHECK_FALSE(nobody.Next.HasOwner());
}

TEST_CASE("Authority: out-of-order lease updates can never produce two owners",
          "[rfab][authority][table][regression]")
{
    LeaseTable table;

    CHECK(table.Apply(100u, OwnedBy(kHost, 5ull, 1000ull)));
    CHECK(table.OwnerOf(100u) == kHost);

    // The grant for epoch 7 arrives before the relinquish for epoch 6.
    CHECK(table.Apply(100u, OwnedBy(kGuest, 7ull, 1200ull)));
    CHECK_FALSE(table.Apply(100u, OwnedBy(kNoOwner, 6ull, 1100ull)));
    CHECK(table.OwnerOf(100u) == kGuest);
    CHECK(table.EpochOf(100u) == 7ull);

    // A duplicate of the current epoch changes nothing.
    CHECK_FALSE(table.Apply(100u, OwnedBy(kHost, 7ull, 1300ull)));
    CHECK(table.OwnerOf(100u) == kGuest);
    CHECK(table.IsOwnedBy(100u, kGuest));
    CHECK_FALSE(table.IsOwnedBy(100u, kHost));
    CHECK_FALSE(table.IsOwnedBy(100u, kNoOwner));
}

TEST_CASE("Authority: a replayed packet storm leaves the table on the newest epoch",
          "[rfab][authority][table]")
{
    LeaseTable table;
    const uint32_t entity = 42u;

    table.Apply(entity, OwnedBy(kHost, 1ull, 0ull));

    // Feed the same five updates in a deterministic but scrambled order, twice.
    const Lease updates[] = {
        OwnedBy(kGuest, 2ull, 10ull), OwnedBy(kThird, 5ull, 40ull), OwnedBy(kHost, 3ull, 20ull),
        OwnedBy(kNoOwner, 4ull, 30ull), OwnedBy(kGuest, 6ull, 50ull),
    };

    for (int pass = 0; pass < 2; ++pass)
        for (const auto& update : updates)
            table.Apply(entity, update);

    CHECK(table.EpochOf(entity) == 6ull);
    CHECK(table.OwnerOf(entity) == kGuest);
    CHECK(table.Size() == 1u);
}

TEST_CASE("Authority: the table forgets lapsed and removed entities", "[rfab][authority][table]")
{
    LeaseTable table;
    table.Apply(1u, OwnedBy(kHost, 1ull, 0ull, 1000ull));
    table.Apply(2u, OwnedBy(kGuest, 1ull, 0ull, 3000ull));
    table.Apply(3u, OwnedBy(kThird, 1ull, 0ull));

    CHECK(table.Size() == 3u);
    CHECK(table.ExpireBefore(1500ull) == 1u);
    CHECK(table.OwnerOf(1u) == kNoOwner);
    CHECK(table.Size() == 2u);

    table.Forget(2u);
    CHECK(table.Size() == 1u);
    CHECK(table.Find(2u) == nullptr);

    table.Clear();
    CHECK(table.Size() == 0u);
    CHECK(table.EpochOf(3u) == kUnknownEpoch);
}

TEST_CASE("Authority: a full session of handovers stays consistent", "[rfab][authority][session]")
{
    // One server-side lease, one client-side mirror, driven through the sequence that used
    // to break: join, claim, contested claim, downed challenger, disconnect, reassign.
    const Policy policy{1500ull, 0ull};
    Lease server{};
    LeaseTable client;
    uint64_t now = 0ull;

    auto commit = [&](const Decision& acDecision) {
        if (acDecision.Changed)
        {
            server = acDecision.Next;
            CHECK(client.Apply(777u, server));
        }
        return acDecision.Result;
    };

    CHECK(commit(Claim(server, kWorldActor, kOwnerPresent, Peer(kHost), now += 100, policy)) ==
          Outcome::kGranted);
    CHECK(client.OwnerOf(777u) == kHost);

    CHECK(commit(Claim(server, kWorldActor, kOwnerPresent, Peer(kGuest), now += 100, policy)) ==
          Outcome::kDeniedHoldWindow);
    CHECK(client.OwnerOf(777u) == kHost);

    CHECK(commit(Claim(server, kWorldActor, kOwnerPresent, Peer(kGuest), now += 5000, policy)) ==
          Outcome::kGranted);
    CHECK(client.OwnerOf(777u) == kGuest);

    // The host disconnects; its late transfer request must not disturb the guest.
    CHECK(commit(Handoff(server, kHost, 1ull, Peer(kThird), now += 10, policy)) == Outcome::kDeniedStaleEpoch);
    CHECK(client.OwnerOf(777u) == kGuest);

    // The guest disconnects, and the entity goes to the only peer left in range.
    CHECK(commit(Revoke(server, now += 10)) == Outcome::kReleased);
    CHECK(commit(Reassign(server, kWorldActor, {Peer(kThird)}, {}, now += 10, policy)) == Outcome::kGranted);
    CHECK(client.OwnerOf(777u) == kThird);
    CHECK(client.EpochOf(777u) == server.Epoch);
    CHECK(server.Epoch == 4ull);
}

TEST_CASE("Authority: the client-side epoch gate keeps grants and relinquishes ordered",
          "[rfab][authority][table][client]")
{
    LeaseTable client;
    const uint32_t entity = 7u;

    // Grant at epoch 4, then a relinquish for epoch 3 that overtook it on the wire.
    CHECK(client.AcceptEpoch(entity, 4ull, true));
    CHECK(client.IsOwnedBy(entity, kLocalOwner));
    CHECK_FALSE(client.AcceptEpoch(entity, 3ull, false));
    CHECK(client.IsOwnedBy(entity, kLocalOwner));

    // A re-sent grant for the same epoch must still be applied: the server resends it
    // after a cell reload, and dropping it used to leave the client simulating nothing.
    CHECK(client.AcceptEpoch(entity, 4ull, true));

    // The real relinquish, one epoch later, takes effect.
    CHECK(client.AcceptEpoch(entity, 5ull, false));
    CHECK(client.OwnerOf(entity) == kNoOwner);
    CHECK(client.EpochOf(entity) == 5ull);

    // A peer that does not send epochs at all still gets applied, without rewriting
    // the epoch we already know.
    CHECK(client.AcceptEpoch(entity, kUnknownEpoch, true));
    CHECK(client.IsOwnedBy(entity, kLocalOwner));
    CHECK(client.EpochOf(entity) == 5ull);

    client.Forget(entity);
    CHECK(client.AcceptEpoch(entity, kUnknownEpoch, false));
    CHECK(client.OwnerOf(entity) == kNoOwner);
}
