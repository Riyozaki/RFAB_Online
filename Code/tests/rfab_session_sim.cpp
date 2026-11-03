// Multi-peer session scenarios.
//
// These run the shipping decision code (Authority::Lease, Save::Coordinator) through the
// deterministic session model in SessionSim.h: several peers, a clock, and a reliable but
// delayed and occasionally duplicating channel. Unit tests pin one decision; these pin
// what happens when decisions race each other, which is where sessions actually broke.
//
// Every scenario ends by letting the session settle and then asserting the state a player
// would see: exactly one simulator per entity, nothing stranded, no checkpoint written
// behind someone's back.

#include <catch2/catch.hpp>

#include "SessionSim.h"

using SessionSim::Session;

namespace
{
// Prints the seed and the session log next to a failure, so a soak failure is a recipe
// rather than a mystery.
void RequireNoViolations(const Session& acSession, uint32_t aSeed)
{
    if (acSession.Problems().Empty())
        return;

    UNSCOPED_INFO("seed " << aSeed);
    for (const auto& line : acSession.Log())
        UNSCOPED_INFO(line);
    for (const auto& line : acSession.Problems().Entries)
        UNSCOPED_INFO("VIOLATION " << line);

    FAIL("the session broke an invariant");
}

// After everything has been delivered, every world entity someone can reach must have
// exactly one simulator, and the peers must agree with the server about who it is.
void RequireSettledOwnership(Session& aSession)
{
    bool someoneCanSimulate = false;
    for (const auto& peer : aSession.Peers())
    {
        if (peer.Connected && peer.InRange)
            someoneCanSimulate = true;
    }

    for (const auto& entity : aSession.Entities())
    {
        if (entity.IsPlayerAvatar)
            continue;

        const uint32_t serverOwner = aSession.ServerOwnerOf(entity.Id);
        const auto believers = aSession.BelieversFor(entity.Id);

        if (someoneCanSimulate)
        {
            INFO("entity " << entity.Id);
            CHECK(serverOwner != Authority::kNoOwner);
        }

        INFO("entity " << entity.Id << " is simulated by " << believers.size() << " peers");
        CHECK(believers.size() <= 1u);
        if (!believers.empty())
            CHECK(believers.front() == serverOwner);
    }
}
} // namespace

TEST_CASE("Session: three peers grabbing the same NPC end up with exactly one simulator",
          "[rfab][session][authority]")
{
    Session session{3, 3, 20261103u};
    const uint32_t entity = session.Entities().front().Id;

    // Everyone wants it, repeatedly, while grants are still in flight.
    for (uint32_t round = 0; round < 20; ++round)
    {
        for (auto& peer : session.Peers())
            session.Claim(peer.Id, entity);

        session.Tick();
    }

    session.Quiesce();
    RequireNoViolations(session, 20261103u);
    RequireSettledOwnership(session);
    CHECK(session.ServerOwnerOf(entity) != Authority::kNoOwner);
}

TEST_CASE("Session: an owner that disconnects does not take the entity with it",
          "[rfab][session][authority]")
{
    Session session{3, 4, 7u};
    const uint32_t entity = session.Entities().front().Id;

    session.Claim(2u, entity);
    for (uint32_t i = 0; i < 20; ++i)
        session.Tick();

    REQUIRE(session.ServerOwnerOf(entity) == 2u);

    session.SetConnected(2u, false);
    session.Quiesce();

    RequireNoViolations(session, 7u);
    const uint32_t owner = session.ServerOwnerOf(entity);
    CHECK((owner == 1u || owner == 3u));
    RequireSettledOwnership(session);
}

TEST_CASE("Session: an owner who walks out of the cell loses it to someone who is there",
          "[rfab][session][authority]")
{
    Session session{2, 2, 11u};
    const uint32_t entity = session.Entities().front().Id;

    session.Claim(1u, entity);
    for (uint32_t i = 0; i < 20; ++i)
        session.Tick();
    REQUIRE(session.ServerOwnerOf(entity) == 1u);

    session.SetInRange(1u, false);
    session.Quiesce();

    RequireNoViolations(session, 11u);
    CHECK(session.ServerOwnerOf(entity) == 2u);
}

TEST_CASE("Session: nobody in range means nobody is pretending to simulate",
          "[rfab][session][authority]")
{
    Session session{2, 2, 13u};
    const uint32_t entity = session.Entities().front().Id;

    session.Claim(1u, entity);
    for (uint32_t i = 0; i < 20; ++i)
        session.Tick();

    session.SetInRange(1u, false);
    session.SetInRange(2u, false);
    session.Quiesce();

    RequireNoViolations(session, 13u);
    CHECK(session.ServerOwnerOf(entity) == Authority::kNoOwner);
    CHECK(session.BelieversFor(entity).empty());
}

TEST_CASE("Session: a player's own character is never handed to anyone else",
          "[rfab][session][authority]")
{
    Session session{3, 1, 29u};

    uint32_t avatar = 0;
    for (const auto& entity : session.Entities())
    {
        if (entity.IsPlayerAvatar && entity.PinnedTo == 1u)
            avatar = entity.Id;
    }
    REQUIRE(avatar != 0);

    // Two peers try to take someone else's body, while its owner is downed and
    // out of range - every excuse the old code used to move an entity.
    session.SetDowned(1u, true);
    session.SetInRange(1u, false);
    for (uint32_t round = 0; round < 10; ++round)
    {
        session.Claim(2u, avatar);
        session.Claim(3u, avatar);
        session.Tick();
    }

    session.Quiesce();
    RequireNoViolations(session, 29u);
    CHECK(session.ServerOwnerOf(avatar) == 1u);
}

TEST_CASE("Session: a client that refuses an entity is not offered it again",
          "[rfab][session][authority]")
{
    Session session{2, 1, 31u};
    const uint32_t entity = session.Entities().front().Id;

    // Peer 1 cannot load this actor; peer 2 is standing right there.
    session.RecordDecline(1u, entity);
    session.Quiesce();

    RequireNoViolations(session, 31u);
    CHECK(session.ServerOwnerOf(entity) == 2u);
}

TEST_CASE("Session: a checkpoint everyone refuses is never written", "[rfab][session][save]")
{
    Session session{3, 2, 41u};
    session.Peers()[1].RefusesSave = true;

    REQUIRE(session.RequestSave(false));
    session.Quiesce();

    RequireNoViolations(session, 41u);
    CHECK(session.CommitCount() == 0u);
    CHECK(session.AbortCount() >= 1u);
    CHECK(session.Saves().GetLastCommittedEpoch() == 0u);
}

TEST_CASE("Session: a save completes when everyone writes it, even with the acks reordered",
          "[rfab][session][save]")
{
    Session session{4, 2, 43u};

    REQUIRE(session.RequestSave(false));
    session.Quiesce();

    RequireNoViolations(session, 43u);
    CHECK(session.CommitCount() == 1u);
    CHECK(session.Saves().GetLastCommittedEpoch() > 0u);
}

TEST_CASE("Session: a player leaving mid-save does not hang the barrier", "[rfab][session][save]")
{
    Session session{3, 2, 47u};
    REQUIRE(session.RequestSave(false));

    // Drop someone while their save is still being written.
    session.Tick();
    session.SetConnected(3u, false);
    session.Quiesce();

    RequireNoViolations(session, 47u);
    CHECK_FALSE(session.Saves().HasPendingBarrier());
    CHECK((session.CommitCount() + session.AbortCount()) >= 1u);
}

TEST_CASE("Session: guests are held out of the world until the host has loaded",
          "[rfab][session][save][reload]")
{
    Session session{3, 3, 53u};
    const uint32_t entity = session.Entities().front().Id;

    session.Peers()[0].StuckLoading = true; // the host never reports back
    session.RollBack(5ull);

    // Guests keep trying to claim while the host is loading.
    for (uint32_t round = 0; round < 20; ++round)
    {
        session.Claim(2u, entity);
        session.Claim(3u, entity);
        session.Tick();
    }

    // The gate exists so the host - and only the host - decides the world it just
    // loaded. Guests are refused; the host may still be given dormant entities.
    CHECK(session.ClaimsHeldByGate() > 0u);
    CHECK(session.ServerOwnerOf(entity) != 2u);
    CHECK(session.ServerOwnerOf(entity) != 3u);

    // ... but a host that never finishes must not freeze the session for everyone else.
    session.Quiesce();
    RequireNoViolations(session, 53u);
    CHECK_FALSE(session.Saves().HasActiveReloadBarrier());
    RequireSettledOwnership(session);
}

TEST_CASE("Session: a rollback moves the world timeline forward for everyone",
          "[rfab][session][save][reload]")
{
    Session session{3, 2, 59u};
    const uint64_t before = session.Saves().GetCurrentWorldEpoch();

    session.RollBack(9ull);
    session.Quiesce();

    RequireNoViolations(session, 59u);
    CHECK(session.Saves().GetCurrentWorldEpoch() > before);
    CHECK_FALSE(session.Saves().HasActiveReloadBarrier());
}

TEST_CASE("Session: everyone ends up agreeing how hurt the dragon is",
          "[rfab][session][damage]")
{
    Session session{3, 2, 61u};
    const uint32_t dragon = session.Entities().front().Id;

    session.Claim(1u, dragon);
    for (uint32_t i = 0; i < 20; ++i)
        session.Tick();
    REQUIRE(session.ServerOwnerOf(dragon) == 1u);

    // Three players hitting the same target, their reports crossing on the wire.
    for (uint32_t round = 0; round < 10; ++round)
    {
        session.Damage(1u, dragon, -3.0f);
        session.Damage(2u, dragon, -2.0f);
        session.Damage(3u, dragon, -1.0f);
        session.Tick();
    }

    session.Quiesce();
    RequireNoViolations(session, 61u);
    CHECK(session.HealthAgreedOn(dragon));
    CHECK(session.ServerHealthOf(dragon) < 100.0f);
    CHECK(session.ServerHealthOf(dragon) >= 0.0f);
}

TEST_CASE("Session: a target cannot be hurt below zero or healed by damage",
          "[rfab][session][damage]")
{
    Session session{2, 1, 67u};
    const uint32_t target = session.Entities().front().Id;

    session.Claim(1u, target);
    for (uint32_t i = 0; i < 20; ++i)
        session.Tick();

    for (uint32_t round = 0; round < 40; ++round)
    {
        session.Damage(1u, target, -25.0f);
        session.Tick();
    }

    session.Quiesce();
    RequireNoViolations(session, 67u);
    CHECK(session.ServerHealthOf(target) == 0.0f);
    CHECK(session.HealthAgreedOn(target));
}

TEST_CASE("Session: the same swing sent twice costs the target once",
          "[rfab][session][damage]")
{
    Session session{2, 1, 73u};
    const uint32_t target = session.Entities().front().Id;

    session.Claim(1u, target);
    for (uint32_t i = 0; i < 20; ++i)
        session.Tick();

    // Peer 2 swings once and, believing the packet was lost, sends the same one again.
    session.Damage(2u, target, -10.0f);
    session.ReplayLastDamage(2u);
    session.ReplayLastDamage(2u);
    session.Quiesce();

    RequireNoViolations(session, 73u);
    CHECK(session.DuplicateHits() >= 2u);
    CHECK(session.ServerHealthOf(target) == 90.0f);
    CHECK(session.HealthAgreedOn(target));
}

TEST_CASE("Session: a refused hit does not leave the attacker's health bar wrong",
          "[rfab][session][damage]")
{
    Session session{2, 1, 79u};
    const uint32_t target = session.Entities().front().Id;

    session.Claim(1u, target);
    for (uint32_t i = 0; i < 20; ++i)
        session.Tick();

    // Peer 2 is down on its knees, where the server refuses its damage - but its own game
    // already took the health off locally. The owner keeps restating what it sees, and
    // that stream is what has to pull the mistaken player back to the truth.
    session.SetDowned(2u, true);
    session.Damage(2u, target, -40.0f);
    session.Quiesce();

    RequireNoViolations(session, 79u);
    CHECK(session.RejectedHits() >= 1u);
    CHECK(session.ServerHealthOf(target) == 100.0f);
    CHECK(session.KnownHealthOf(2u, target) == 100.0f);
}

TEST_CASE("Session: an older echo does not bounce a swing that already landed",
          "[rfab][session][damage]")
{
    Session session{2, 1, 89u};
    const uint32_t target = session.Entities().front().Id;

    session.Claim(1u, target);
    for (uint32_t i = 0; i < 20; ++i)
        session.Tick();

    // Peer 2 swings again and again. The server's answer to one swing is still on the wire
    // when the next lands locally: believing it would show the player its own damage being
    // handed back.
    for (uint32_t round = 0; round < 8; ++round)
    {
        session.Damage(2u, target, -5.0f);
        session.Tick();
    }

    session.Quiesce();
    RequireNoViolations(session, 89u);
    CHECK(session.HeldEchoes() >= 1u);
    CHECK(session.ServerHealthOf(target) == 60.0f);
    CHECK(session.HealthAgreedOn(target));
}

TEST_CASE("Session: the owner's own reading cannot undo another player's hit",
          "[rfab][session][damage]")
{
    Session session{2, 1, 83u};
    const uint32_t target = session.Entities().front().Id;

    session.Claim(1u, target);
    for (uint32_t i = 0; i < 20; ++i)
        session.Tick();
    REQUIRE(session.ServerOwnerOf(target) == 1u);

    // Peer 1 simulates the target and keeps restating its health. Peer 2 hits it while
    // those readings are in flight: every one of them is now out of date.
    for (uint32_t round = 0; round < 6; ++round)
    {
        session.Damage(2u, target, -5.0f);
        for (uint32_t i = 0; i < 4; ++i)
            session.Tick();
    }

    session.Quiesce();
    RequireNoViolations(session, 83u);
    CHECK(session.SuppressedSnapshots() >= 1u);
    CHECK(session.ServerHealthOf(target) == 70.0f);
    CHECK(session.HealthAgreedOn(target));
}

TEST_CASE("Session: a hit reported from outside the cell is not believed",
          "[rfab][session][damage]")
{
    Session session{2, 1, 71u};
    const uint32_t target = session.Entities().front().Id;

    session.Claim(1u, target);
    for (uint32_t i = 0; i < 20; ++i)
        session.Tick();

    // Peer 2 does not simulate the target and is nowhere near it.
    session.SetInRange(2u, false);
    const float before = session.ServerHealthOf(target);
    for (uint32_t round = 0; round < 5; ++round)
    {
        session.Damage(2u, target, -10.0f);
        session.Tick();
    }

    session.Quiesce();
    RequireNoViolations(session, 71u);
    CHECK(session.RejectedHits() >= 5u);
    CHECK(session.ServerHealthOf(target) == before);
}

TEST_CASE("Session: soak - random play never produces two simulators or a lost checkpoint",
          "[rfab][session][soak]")
{
    // Each seed is a different session: who claims what, who drops, who refuses a save,
    // when the host rolls back. The invariants are checked on every tick inside the model.
    for (uint32_t seed = 1; seed <= 32u; ++seed)
    {
        Session session{4, 5, seed * 2654435761u};

        for (uint32_t tick = 0; tick < 300u; ++tick)
        {
            const uint32_t roll = session.Random(100u);
            auto& peers = session.Peers();
            const uint32_t peerId = peers[session.Random(static_cast<uint32_t>(peers.size()))].Id;
            const auto& entities = session.Entities();
            const uint32_t entityId = entities[session.Random(static_cast<uint32_t>(entities.size()))].Id;

            if (roll < 30)
                session.Claim(peerId, entityId);
            else if (roll < 35)
                session.Damage(peerId, entityId, -1.0f * static_cast<float>(1u + session.Random(20u)));
            else if (roll < 45)
                session.Release(peerId, entityId);
            else if (roll < 52)
                session.SetInRange(peerId, session.Random(2u) == 0u);
            else if (roll < 58)
                session.SetDowned(peerId, session.Random(2u) == 0u);
            else if (roll < 62)
                session.SetConnected(peerId, session.Random(2u) == 0u);
            else if (roll < 65)
                session.RequestSave(session.Random(2u) == 0u);
            else if (roll < 67)
                session.Peers()[session.Random(static_cast<uint32_t>(peers.size()))].RefusesSave =
                    session.Random(2u) == 0u;
            else if (roll < 68)
                session.RollBack(session.Saves().GetLastCommittedEpoch());

            session.Tick();
        }

        // Put the session back in a playable shape and let it settle.
        for (auto& peer : session.Peers())
        {
            peer.RefusesSave = false;
            peer.StuckLoading = false;
            session.SetConnected(peer.Id, true);
            session.SetInRange(peer.Id, true);
            session.SetDowned(peer.Id, false);
        }

        session.Quiesce();
        RequireNoViolations(session, seed);
        RequireSettledOwnership(session);

        for (const auto& entity : session.Entities())
        {
            INFO("entity " << entity.Id << " health " << session.ServerHealthOf(entity.Id));
            CHECK(session.HealthAgreedOn(entity.Id));
        }
    }
}
