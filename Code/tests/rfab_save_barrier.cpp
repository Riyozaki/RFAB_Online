// Behaviour of the save barrier (Code/common/Save/Barrier.h).
//
// Every case here is a thing that can go wrong in a session: a checkpoint written
// while a client refused it, a world reopened while someone is still loading, a
// session restored with a gate still armed. The barrier used to be welded inside
// SaveSnapshotService, so none of it could be exercised without a full Windows build.

#include <catch2/catch.hpp>

#include <Save/Barrier.h>

using Save::Coordinator;

namespace
{
constexpr uint32_t kHost = 1;
constexpr uint32_t kGuestA = 2;
constexpr uint32_t kGuestB = 3;

using Ack = Coordinator::AckResolution;
} // namespace

TEST_CASE("Save barrier: an epoch is committed only after every player wrote their save",
          "[rfab][save][barrier]")
{
    Coordinator coordinator{"S0042", 3};

    const auto started = coordinator.BeginBarrier({kHost, kGuestA, kGuestB}, false, 1000ull);
    REQUIRE(started.has_value());
    REQUIRE(coordinator.HasPendingBarrier());
    CHECK(started->SlotName == "RFAB_MP_S0042_Manual_0");

    const uint64_t epoch = started->EpochId;
    CHECK(coordinator.RecordClientAck(kHost, epoch, true, "") == Ack::WaitingForMorePlayers);
    CHECK(coordinator.RecordClientAck(kGuestA, epoch, true, "") == Ack::WaitingForMorePlayers);

    // Nothing is recorded while a single player is still writing.
    CHECK(coordinator.GetLastCommittedEpoch() == 0);

    CHECK(coordinator.RecordClientAck(kGuestB, epoch, true, "") == Ack::ReadyToCommit);

    const auto committed = coordinator.ConsumeAndCommit(2000ull);
    CHECK(committed.EpochId == epoch);
    CHECK_FALSE(coordinator.HasPendingBarrier());
    CHECK(coordinator.GetLastCommittedEpoch() == epoch);
    CHECK(coordinator.GetLastCommittedSlot() == "RFAB_MP_S0042_Manual_0");
    CHECK(coordinator.GetCurrentWorldEpoch() >= epoch);
}

TEST_CASE("Save barrier: acks that belong to nobody and nothing are ignored", "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    const auto started = coordinator.BeginBarrier({kHost, kGuestA}, false, 0ull);
    REQUIRE(started.has_value());

    CHECK(coordinator.RecordClientAck(kGuestB, started->EpochId, true, "") == Ack::Ignored);
    CHECK(coordinator.RecordClientAck(kHost, started->EpochId + 7ull, true, "") == Ack::Ignored);
    CHECK(coordinator.GetPendingBarrier()->AckedPlayerIds.empty());
}

TEST_CASE("Save barrier: a refusal kills the barrier and late acks cannot revive it",
          "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    const auto started = coordinator.BeginBarrier({kHost, kGuestA}, false, 500ull);
    REQUIRE(started.has_value());
    const uint64_t epoch = started->EpochId;

    CHECK(coordinator.RecordClientAck(kHost, epoch, false, "no space left on device") == Ack::Aborted);

    // The other client's ack was already on the wire. It must not complete a barrier
    // that someone refused, or the server records a checkpoint one player never wrote.
    CHECK(coordinator.RecordClientAck(kGuestA, epoch, true, "") == Ack::Aborted);
    CHECK(coordinator.HandlePlayerDisconnected(kGuestA) == Ack::Aborted);

    const auto aborted = coordinator.ConsumeAborted(900ull);
    CHECK(aborted.EpochId == epoch);
    CHECK(aborted.AbortReason == "no space left on device");
    CHECK_FALSE(coordinator.HasPendingBarrier());
    CHECK(coordinator.GetLastCommittedEpoch() == 0);
}

TEST_CASE("Save barrier: committing or aborting nothing is a no-op", "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    REQUIRE_FALSE(coordinator.HasPendingBarrier());

    // Both of these used to read an empty optional.
    CHECK(coordinator.ConsumeAndCommit(1000ull).EpochId == 0);
    CHECK(coordinator.ConsumeAborted(1000ull).EpochId == 0);
    CHECK(coordinator.GetLastCommittedEpoch() == 0);
    CHECK(coordinator.GetCurrentWorldEpoch() == 1);
}

TEST_CASE("Save barrier: the session does not wait for players who left", "[rfab][save][barrier]")
{
    SECTION("the last outstanding player leaving completes the barrier")
    {
        Coordinator coordinator{};
        const auto started = coordinator.BeginBarrier({kHost, kGuestA}, false, 0ull);
        REQUIRE(coordinator.RecordClientAck(kHost, started->EpochId, true, "") == Ack::WaitingForMorePlayers);
        CHECK(coordinator.HandlePlayerDisconnected(kGuestA) == Ack::ReadyToCommit);
    }

    SECTION("everyone leaving aborts it")
    {
        Coordinator coordinator{};
        const auto started = coordinator.BeginBarrier({kHost, kGuestA}, false, 0ull);
        REQUIRE(started.has_value());
        CHECK(coordinator.HandlePlayerDisconnected(kHost) == Ack::WaitingForMorePlayers);
        CHECK(coordinator.HandlePlayerDisconnected(kGuestA) == Ack::Aborted);
        CHECK(coordinator.ConsumeAborted().AbortReason == "All players disconnected during save barrier");
    }
}

TEST_CASE("Save barrier: one barrier at a time, and never with an empty session",
          "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    REQUIRE(coordinator.BeginBarrier({kHost}, false, 0ull).has_value());
    CHECK_FALSE(coordinator.BeginBarrier({kHost}, false, 10ull).has_value());

    Coordinator empty{};
    CHECK_FALSE(empty.BeginBarrier({}, false, 0ull).has_value());
    CHECK_FALSE(empty.HasPendingBarrier());
}

TEST_CASE("Save barrier: slots rotate through the ring and manual never overwrites auto",
          "[rfab][save][barrier]")
{
    Coordinator coordinator{"S0007", 3};

    auto runBarrier = [&coordinator](bool aIsAuto, uint64_t aNowMs) {
        const auto started = coordinator.BeginBarrier({kHost}, aIsAuto, aNowMs);
        REQUIRE(started.has_value());
        REQUIRE(coordinator.RecordClientAck(kHost, started->EpochId, true, "") == Ack::ReadyToCommit);
        return coordinator.ConsumeAndCommit(aNowMs).SlotName;
    };

    CHECK(runBarrier(false, 0ull) == "RFAB_MP_S0007_Manual_0");
    CHECK(runBarrier(false, 1ull) == "RFAB_MP_S0007_Manual_1");
    CHECK(runBarrier(false, 2ull) == "RFAB_MP_S0007_Manual_2");
    CHECK(runBarrier(false, 3ull) == "RFAB_MP_S0007_Manual_0"); // wraps, keeping 3 generations
    CHECK(runBarrier(true, 4ull) == "RFAB_MP_S0007_Auto_0");    // its own cursor
    CHECK(runBarrier(true, 5ull) == "RFAB_MP_S0007_Auto_1");
    CHECK(runBarrier(false, 6ull) == "RFAB_MP_S0007_Manual_1");
}

TEST_CASE("Save barrier: epoch ids are never reused", "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    std::vector<uint64_t> seen;

    const auto first = coordinator.BeginBarrier({kHost}, false, 0ull);
    seen.push_back(first->EpochId);
    coordinator.RecordClientAck(kHost, first->EpochId, false, "cancelled");
    coordinator.ConsumeAborted();

    const auto second = coordinator.BeginBarrier({kHost}, false, 10ull);
    seen.push_back(second->EpochId);
    coordinator.RecordClientAck(kHost, second->EpochId, true, "");
    coordinator.ConsumeAndCommit(20ull);

    coordinator.NotifyRolledBackTo(second->EpochId, "RFAB_MP_S0001_Manual_0", 30ull);

    const auto third = coordinator.BeginBarrier({kHost}, false, 40ull);
    seen.push_back(third->EpochId);

    CHECK(seen[0] < seen[1]);
    CHECK(seen[1] < seen[2]);
}

TEST_CASE("Save barrier: a barrier nobody answers times out", "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    REQUIRE(coordinator.BeginBarrier({kHost, kGuestA}, false, 10'000ull).has_value());

    CHECK_FALSE(coordinator.HasBarrierTimedOut(10'000ull, 12'000ull));
    CHECK_FALSE(coordinator.HasBarrierTimedOut(21'999ull, 12'000ull));
    CHECK(coordinator.HasBarrierTimedOut(22'000ull, 12'000ull));

    Coordinator idle{};
    CHECK_FALSE(idle.HasBarrierTimedOut(1'000'000ull, 12'000ull));
}

TEST_CASE("Save barrier: autosave waits for its interval unless a boss just died",
          "[rfab][save][barrier]")
{
    Coordinator coordinator{};

    // Nothing has been checkpointed yet, so the first opportunity is taken.
    CHECK(coordinator.ShouldTriggerPostCombatAutosave(5'000ull, 300u, false));

    const auto started = coordinator.BeginBarrier({kHost}, true, 5'000ull);
    REQUIRE(coordinator.RecordClientAck(kHost, started->EpochId, true, "") == Ack::ReadyToCommit);
    coordinator.ConsumeAndCommit(5'000ull);

    CHECK_FALSE(coordinator.ShouldTriggerPostCombatAutosave(100'000ull, 300u, false));
    CHECK(coordinator.ShouldTriggerPostCombatAutosave(100'000ull, 300u, true)); // boss kill
    CHECK(coordinator.ShouldTriggerPostCombatAutosave(305'000ull, 300u, false));

    // While a barrier is running, no second one is proposed.
    REQUIRE(coordinator.BeginBarrier({kHost}, true, 305'000ull).has_value());
    CHECK_FALSE(coordinator.ShouldTriggerPostCombatAutosave(400'000ull, 300u, true));
}

TEST_CASE("Save barrier: a rollback advances the world timeline", "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    const uint64_t before = coordinator.GetCurrentWorldEpoch();

    coordinator.NotifyRolledBackTo(9ull, "RFAB_MP_S0001_Manual_2", 1'000ull);

    // Everything sent before the load is now from an older world and can be dropped.
    CHECK(coordinator.GetCurrentWorldEpoch() == before + 1);
    CHECK(coordinator.GetLastCommittedEpoch() == 9ull);
    CHECK(coordinator.GetLastCommittedSlot() == "RFAB_MP_S0001_Manual_2");
    CHECK(coordinator.GetNextEpochId() >= coordinator.GetCurrentWorldEpoch());
    CHECK_FALSE(coordinator.HasPendingBarrier());
}

TEST_CASE("Save barrier: guests are held until the host has loaded its save",
          "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    const auto barrier = coordinator.BeginReloadBarrier({kHost, kGuestA}, kHost, 4ull, "slot", 1'000ull);
    REQUIRE(coordinator.HasActiveReloadBarrier());
    CHECK_FALSE(barrier.HostGateReleased);

    // While the host loads, a guest must not claim dormant NPCs out from under it.
    CHECK(coordinator.ShouldHoldNonHostWorldClaims(kGuestA));
    CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(kHost));

    CHECK(coordinator.RecordReloadPlayerReady(kHost, 4ull));        // releases the gate
    CHECK_FALSE(coordinator.RecordReloadPlayerReady(kHost, 4ull));  // and only reports it once
    CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(kGuestA));

    REQUIRE(coordinator.HasActiveReloadBarrier()); // still waiting for the guest
    CHECK_FALSE(coordinator.RecordReloadPlayerReady(kGuestA, 4ull));
    CHECK_FALSE(coordinator.HasActiveReloadBarrier());
}

TEST_CASE("Save barrier: a stranger reporting ready cannot reopen the world",
          "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    coordinator.BeginReloadBarrier({kHost, kGuestA}, kHost, 4ull, "slot", 0ull);

    // kGuestB is not in this barrier: a stale or spoofed ready used to count towards
    // the total and could release the world while kGuestA was still loading.
    CHECK_FALSE(coordinator.RecordReloadPlayerReady(kGuestB, 4ull));
    CHECK(coordinator.RecordReloadPlayerReady(kHost, 4ull));
    REQUIRE(coordinator.HasActiveReloadBarrier());
    REQUIRE(coordinator.GetReloadBarrier() != nullptr);
    CHECK(coordinator.GetReloadBarrier()->ReadyPlayerIds.count(kGuestB) == 0);

    CHECK_FALSE(coordinator.RecordReloadPlayerReady(kGuestA, 4ull));
    CHECK_FALSE(coordinator.HasActiveReloadBarrier());
}

TEST_CASE("Save barrier: a host that never finishes loading cannot freeze the session",
          "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    coordinator.BeginReloadBarrier({kHost, kGuestA}, kHost, 4ull, "slot", 1'000ull);

    CHECK_FALSE(coordinator.TickReloadBarrier(10'000ull, 15'000ull, 45'000ull));
    CHECK(coordinator.ShouldHoldNonHostWorldClaims(kGuestA));

    CHECK(coordinator.TickReloadBarrier(16'000ull, 15'000ull, 45'000ull)); // hold expired
    CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(kGuestA));
    CHECK(coordinator.HasActiveReloadBarrier());

    CHECK_FALSE(coordinator.TickReloadBarrier(46'001ull, 15'000ull, 45'000ull)); // full timeout
    CHECK_FALSE(coordinator.HasActiveReloadBarrier());
}

TEST_CASE("Save barrier: the host leaving releases everyone else", "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    coordinator.BeginReloadBarrier({kHost, kGuestA, kGuestB}, kHost, 4ull, "slot", 0ull);
    REQUIRE(coordinator.ShouldHoldNonHostWorldClaims(kGuestA));

    CHECK(coordinator.HandlePlayerDisconnectedFromReload(kHost));
    CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(kGuestA));
    CHECK(coordinator.HasActiveReloadBarrier());

    coordinator.RecordReloadPlayerReady(kGuestA, 4ull);
    CHECK(coordinator.HasActiveReloadBarrier());
    coordinator.RecordReloadPlayerReady(kGuestB, 4ull);
    CHECK_FALSE(coordinator.HasActiveReloadBarrier());
}

TEST_CASE("Save barrier: a reload barrier with no tracked host does not gate anyone",
          "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    const auto barrier = coordinator.BeginReloadBarrier({kGuestA, kGuestB}, 0u, 4ull, "slot", 0ull);
    CHECK(barrier.HostReady);
    CHECK(barrier.HostGateReleased);
    CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(kGuestA));
}

TEST_CASE("Save barrier: restoring a session leaves no gate armed", "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    coordinator.BeginBarrier({kHost, kGuestA}, false, 0ull);
    coordinator.BeginReloadBarrier({kHost, kGuestA}, kHost, 4ull, "slot", 0ull);
    REQUIRE(coordinator.ShouldHoldNonHostWorldClaims(kGuestA));

    coordinator.RestorePersistedState("S0099", 12ull, 7ull, 1u, 2u, 11ull, "RFAB_MP_S0099_Manual_1");

    // A gate left armed by the previous process would hold every non-host claim for
    // the rest of the session.
    CHECK_FALSE(coordinator.HasActiveReloadBarrier());
    CHECK_FALSE(coordinator.ShouldHoldNonHostWorldClaims(kGuestA));
    CHECK_FALSE(coordinator.HasPendingBarrier());

    CHECK(coordinator.GetSessionTag() == "S0099");
    CHECK(coordinator.GetCurrentWorldEpoch() == 7ull);
    CHECK(coordinator.GetLastCommittedEpoch() == 11ull);
    CHECK(coordinator.GetManualRingCursor() == 1u);
    CHECK(coordinator.GetAutoRingCursor() == 2u);

    // The next epoch can never land on or below one that is already on disk.
    const auto next = coordinator.BeginBarrier({kHost}, false, 100ull);
    REQUIRE(next.has_value());
    CHECK(next->EpochId > 11ull);
}

TEST_CASE("Save barrier: the flight ring records what happened to each epoch",
          "[rfab][save][barrier]")
{
    Coordinator coordinator{};
    const auto started = coordinator.BeginBarrier({kHost}, false, 1'000ull);
    REQUIRE(started.has_value());
    coordinator.RecordClientAck(kHost, started->EpochId, true, "");
    coordinator.ConsumeAndCommit(2'000ull);
    coordinator.NotifyRolledBackTo(started->EpochId, "slot", 3'000ull);

    const auto records = coordinator.GetFlightRing().SnapshotRecent(8);
    REQUIRE(records.size() == 3);
    CHECK(records[0].Kind == CoopFlight::FlightEventKind::kSavePrepared);
    CHECK(records[1].Kind == CoopFlight::FlightEventKind::kSaveCommitted);
    CHECK(records[2].Kind == CoopFlight::FlightEventKind::kWorldRollback);
    CHECK(records[1].AuxValue == started->EpochId);
}
