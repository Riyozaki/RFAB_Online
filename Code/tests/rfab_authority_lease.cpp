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
constexpr Subject kPlayerAvatar{true, false};
constexpr Subject kBoundSummon{false, true};
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

        const auto release = Release(lease, kWorldActor, kGuest, lease.Epoch, now + 1ull);
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
        Handoff(held, kWorldActor, kGuest, 7ull, Peer(kThird), 9000ull).Result,
        Handoff(held, kWorldActor, kHost, 6ull, Peer(kThird), 9000ull).Result,
        Release(held, kWorldActor, kGuest, 7ull, 9000ull).Result,
    };

    for (const auto outcome : rejected)
    {
        CHECK(outcome != Outcome::kGranted);
        CHECK(outcome != Outcome::kReleased);
    }

    // The point of the table above: not one of those calls produced a new lease.
    CHECK(Claim(held, Subject{true, false}, kOwnerPresent, Peer(kGuest), 9000ull).Next == held);
    CHECK(Handoff(held, kWorldActor, kGuest, 7ull, Peer(kThird), 9000ull).Next == held);
    CHECK(Release(held, kWorldActor, kGuest, 7ull, 9000ull).Next == held);
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

    const auto stale = Handoff(current, kWorldActor, kHost, 8ull, Peer(kGuest), 6000ull);
    CHECK(stale.Result == Outcome::kDeniedStaleEpoch);
    CHECK(stale.Next == current);

    const auto notOwner = Handoff(current, kWorldActor, kGuest, kUnknownEpoch, Peer(kHost), 6000ull);
    CHECK(notOwner.Result == Outcome::kDeniedNotOwner);
    CHECK(notOwner.Next == current);

    const auto staleRelease = Release(current, kWorldActor, kHost, 8ull, 6000ull);
    CHECK(staleRelease.Result == Outcome::kDeniedStaleEpoch);
    CHECK(staleRelease.Next == current);
}

TEST_CASE("Authority: a handoff from the current owner moves the lease", "[rfab][authority][handoff]")
{
    const Lease current = OwnedBy(kHost, 4ull, 1000ull);

    const auto decision = Handoff(current, kWorldActor, kHost, 4ull, Peer(kGuest), 2000ull);
    CHECK(decision.Result == Outcome::kGranted);
    CHECK(decision.Next.OwnerId == kGuest);
    CHECK(decision.Next.Epoch == 5ull);
    CHECK(decision.Next.GrantedAtMs == 2000ull);

    // A handoff to nobody is a release.
    const auto toServer = Handoff(current, kWorldActor, kHost, 4ull, Peer(kNoOwner), 2000ull);
    CHECK(toServer.Result == Outcome::kReleased);
    CHECK_FALSE(toServer.Next.HasOwner());
    CHECK(toServer.Next.Epoch == 5ull);
}

TEST_CASE("Authority: peers that do not track epochs still interoperate", "[rfab][authority][compat]")
{
    const Lease current = OwnedBy(kHost, 11ull, 1000ull);

    const auto decision = Handoff(current, kWorldActor, kHost, kUnknownEpoch, Peer(kGuest), 2000ull);
    CHECK(decision.Result == Outcome::kGranted);
    CHECK(decision.Next.Epoch == 12ull);

    CHECK(Release(current, kWorldActor, kHost, kUnknownEpoch, 2000ull).Result == Outcome::kReleased);
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

    // A standing guest outranks a downed host: authority follows who can act.
    Candidate downedLeader = Peer(kHost);
    downedLeader.IsDowned = true;
    downedLeader.IsPartyLeader = true;
    CHECK(Reassign(orphan, kWorldActor, {downedLeader, nearGuest}, {}, 2000ull).Next.OwnerId == kGuest);

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
    CHECK(commit(Handoff(server, kWorldActor, kHost, 1ull, Peer(kThird), now += 10, policy)) == Outcome::kDeniedStaleEpoch);
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

TEST_CASE("Authority: a freshly orphaned entity settles before it is adopted",
          "[rfab][authority][dormant]")
{
    const Policy policy{1500ull, 0ull, 1000ull};

    // The owner just disconnected: the entity is unowned as of now.
    const auto revoked = Revoke(OwnedBy(kHost, 3ull, 1000ull), 5000ull);
    REQUIRE(revoked.Result == Outcome::kReleased);
    const Lease orphan = revoked.Next;

    CHECK_FALSE(CanAdoptDormant(orphan, 5000ull, policy));
    CHECK_FALSE(CanAdoptDormant(orphan, 5999ull, policy));
    CHECK(CanAdoptDormant(orphan, 6000ull, policy));

    // An entity that still has a live owner is never up for adoption.
    CHECK_FALSE(CanAdoptDormant(OwnedBy(kGuest, 4ull, 0ull), 100'000ull, policy));

    // A lapsed lease is as good as unowned.
    CHECK(CanAdoptDormant(OwnedBy(kGuest, 4ull, 0ull, 2000ull), 100'000ull, policy));
}

TEST_CASE("Authority: a player's own character cannot be given up either",
          "[rfab][authority][pinned]")
{
    const Lease avatarLease = OwnedBy(kHost, 4ull, 1000ull);

    // Claim and Reassign already refused pinned entities; Release and Handoff did not,
    // and the server takes the entity id for a handoff straight from the packet. A
    // client could therefore ask the server to let go of a player's own character, and
    // the reassignment that followed left it with no owner at all.
    const auto released = Release(avatarLease, kPlayerAvatar, kHost, 4ull, 5000ull);
    CHECK(released.Result == Outcome::kDeniedPinnedEntity);
    CHECK_FALSE(released.Changed);
    CHECK(released.Next.OwnerId == kHost);
    CHECK(released.Next.Epoch == 4ull);

    const auto handed = Handoff(avatarLease, kPlayerAvatar, kHost, 4ull, Peer(kGuest), 5000ull);
    CHECK(handed.Result == Outcome::kDeniedPinnedEntity);
    CHECK_FALSE(handed.Changed);
    CHECK(handed.Next.OwnerId == kHost);

    // A summon follows its summoner for the same reason.
    const auto summon = Release(avatarLease, kBoundSummon, kHost, 4ull, 5000ull);
    CHECK(summon.Result == Outcome::kDeniedPinnedEntity);
}


TEST_CASE("Lease table: a packet without an epoch cannot rewrite an epoch the table knows",
          "[authority][lease][table]")
{
    // A peer that does not track epochs sends no ordering information. Before this rule
    // both table paths applied such an update unconditionally, so that peer could take an
    // entity from its owner - or hand it to itself - regardless of how much newer the real
    // grant was. Two owners and zero owners both start exactly here.
    LeaseTable table;
    CHECK(table.Apply(10u, OwnedBy(kGuest, 7ull, 1000ull)));
    CHECK(table.OwnerOf(10u) == kGuest);

    // "It is not yours" arriving with no epoch must not undo epoch 7.
    CHECK_FALSE(table.AcceptEpoch(10u, kUnknownEpoch, false));
    CHECK(table.OwnerOf(10u) == kGuest);
    CHECK(table.EpochOf(10u) == 7ull);

    // Nor may it hand the entity to this peer.
    CHECK_FALSE(table.AcceptEpoch(10u, kUnknownEpoch, true));
    CHECK(table.OwnerOf(10u) == kGuest);

    // The same through the replication path.
    CHECK_FALSE(table.Apply(10u, OwnedBy(kNoOwner, kUnknownEpoch, 9000ull)));
    CHECK(table.OwnerOf(10u) == kGuest);
    CHECK(table.EpochOf(10u) == 7ull);
}

TEST_CASE("Lease table: first contact without an epoch is still accepted",
          "[authority][lease][table]")
{
    // Nothing is lost by believing the first thing a peer says about an entity the table
    // has never seen - that is what keeps a session working with an older client.
    LeaseTable table;
    CHECK(table.AcceptEpoch(11u, kUnknownEpoch, true));
    CHECK(table.OwnerOf(11u) == kLocalOwner);
    CHECK(table.EpochOf(11u) == kUnknownEpoch);

    CHECK(table.Apply(12u, OwnedBy(kHost, kUnknownEpoch, 0ull)));
    CHECK(table.OwnerOf(12u) == kHost);
}

TEST_CASE("Lease table: an equal epoch corrects a belief but cannot duplicate a decision",
          "[authority][lease][table]")
{
    LeaseTable table;
    CHECK(table.AcceptEpoch(20u, 4ull, true));
    CHECK(table.OwnerOf(20u) == kLocalOwner);

    // The server rejects this peer's handoff and answers at the epoch the peer already
    // holds ("you are not the owner, still epoch 4"). Dropping it would leave the client
    // simulating an actor nobody else does, so equal epochs are applied on this path.
    CHECK(table.AcceptEpoch(20u, 4ull, false));
    CHECK(table.OwnerOf(20u) == kNoOwner);
    CHECK(table.EpochOf(20u) == 4ull);

    // A correction only moves ownership: the timestamps it was never about stay put.
    LeaseTable expiring;
    CHECK(expiring.Apply(21u, OwnedBy(kHost, 2ull, 500ull, 9000ull)));
    CHECK(expiring.AcceptEpoch(21u, 2ull, false));
    CHECK(expiring.OwnerOf(21u) == kNoOwner);
    CHECK(expiring.Find(21u)->GrantedAtMs == 500ull);
    CHECK(expiring.Find(21u)->ExpiresAtMs == 9000ull);

    // Replicating a decision is the other case: the same epoch says nothing new, and a
    // duplicated packet must not resurrect a stale owner.
    LeaseTable mirror;
    CHECK(mirror.Apply(22u, OwnedBy(kHost, 5ull, 100ull)));
    CHECK_FALSE(mirror.Apply(22u, OwnedBy(kGuest, 5ull, 200ull)));
    CHECK(mirror.OwnerOf(22u) == kHost);
}

TEST_CASE("Authority v4.0.0: out-of-range challengers never get unowned or expired entities",
          "[rfab][authority][v400]")
{
    Candidate farAway = Peer(kGuest);
    farAway.InRange = false;

    CHECK(Claim(Lease{}, kWorldActor, kOwnerPresent, farAway, 1000ull).Result == Outcome::kDeniedNoCandidate);

    const Lease expired = OwnedBy(kHost, 3ull, 1000ull, 9000ull);
    CHECK(Claim(expired, kWorldActor, kOwnerPresent, farAway, 9500ull, WorldNpcLeasePolicy()).Result ==
          Outcome::kDeniedNoCandidate);
}

TEST_CASE("Authority v4.0.0: WorldNpcLeasePolicy expires after 8s and RefreshUnrevoked resumes cleanly",
          "[rfab][authority][v400]")
{
    const Policy npcPolicy = WorldNpcLeasePolicy();
    REQUIRE(npcPolicy.LeaseDurationMs == kWorldNpcLeaseDurationMs);

    const auto initial = Claim(Lease{}, kWorldActor, kOwnerPresent, Peer(kHost), 1000ull, npcPolicy);
    REQUIRE(initial.Result == Outcome::kGranted);
    CHECK(initial.Next.ExpiresAtMs == 1000ull + kWorldNpcLeaseDurationMs);
    CHECK_FALSE(initial.Next.IsExpired(8999ull));
    CHECK(initial.Next.IsExpired(9000ull));

    // Solo pause menu: Renew() refuses a lapsed lease, whereas RefreshUnrevoked() extends the
    // uncontested owner's lease window without churning Epoch when the player closes the menu.
    CHECK(Renew(initial.Next, kHost, 12000ull, npcPolicy).Result == Outcome::kDeniedStaleEpoch);

    const auto refreshed = RefreshUnrevoked(initial.Next, kHost, 12000ull, npcPolicy);
    CHECK(refreshed.Result == Outcome::kGranted);
    CHECK(refreshed.Changed);
    CHECK(refreshed.Next.OwnerId == kHost);
    CHECK(refreshed.Next.Epoch == initial.Next.Epoch);
    CHECK(refreshed.Next.ExpiresAtMs == 12000ull + kWorldNpcLeaseDurationMs);

    // Non-owner cannot refresh someone else's lease.
    CHECK(RefreshUnrevoked(refreshed.Next, kGuest, 13000ull, npcPolicy).Result == Outcome::kDeniedNotOwner);
}

TEST_CASE("Authority v4.0.0: DeclineList cascades across 3 peers and self-heals after TTL",
          "[rfab][authority][v400]")
{
    DeclineList declines;
    Lease current = OwnedBy(kHost, 1ull, 1000ull, 9000ull);
    const std::vector<Candidate> inRangePeers = {Peer(kHost), Peer(kGuest), Peer(kThird)};

    // Host relinquishes at t=2000; Host is added to DeclineList -> Guest wins epoch 2.
    declines.Add(kHost, 2000ull);
    const auto toGuest = Reassign(current, kWorldActor, inRangePeers, declines.CollectActive(2000ull), 2000ull,
                                  WorldNpcLeasePolicy());
    REQUIRE(toGuest.Result == Outcome::kGranted);
    REQUIRE(toGuest.Next.OwnerId == kGuest);
    REQUIRE(toGuest.Next.Epoch == 2ull);
    current = toGuest.Next;

    // Guest's cell is still streaming at t=2100, so Guest also declines. Because DeclineList was
    // NOT wiped on Apply(), BOTH Host and Guest remain declined -> Third wins epoch 3 (zero ping-pong!).
    declines.Add(kGuest, 2100ull);
    const auto toThird = Reassign(current, kWorldActor, inRangePeers, declines.CollectActive(2100ull), 2100ull,
                                  WorldNpcLeasePolicy());
    REQUIRE(toThird.Result == Outcome::kGranted);
    CHECK(toThird.Next.OwnerId == kThird);
    CHECK(toThird.Next.Epoch == 3ull);
    current = toThird.Next;

    // After kDefaultDeclineTtlMs (15s), Host and Guest automatically become eligible again.
    const uint64_t afterTtl = 2100ull + kDefaultDeclineTtlMs;
    CHECK(declines.CollectActive(afterTtl).empty());
    CHECK(declines.Empty());

    // ForgetPlayer removes a disconnecting peer immediately before TTL.
    declines.Add(kGuest, 20000ull);
    REQUIRE(declines.Contains(kGuest, 20001ull));
    declines.ForgetPlayer(kGuest);
    CHECK_FALSE(declines.Contains(kGuest, 20001ull));
}
