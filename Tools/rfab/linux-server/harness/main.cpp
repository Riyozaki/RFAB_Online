// The server, in process, driven through the same dispatcher a connection drives.
//
// This is the first slice of the integration harness the project was missing: `Code/server`
// now compiles and runs on a plain Linux box, so a server-side defect can be observed in
// seconds instead of after a playtest. Nothing here mocks the server - the handlers below
// are the shipping ones (`ActorValueService::OnHealthChangeBroadcast`, `OnDeathStateChange`,
// `OnActorValueChanges`); only the socket layer is absent.
//
// The scenarios are the ones players report as "rollbacks":
//   1. a hit from the owner lands and moves the health revision;
//   2. a killing blow fixes health at zero and moves it again;
//   3. an absolute reading sampled *before* the death is refused, and the sender is told
//      the truth - this is the defect that let dead NPCs stand back up;
//   4. a reading sampled after the death is accepted as already current (no correction
//      storm, no oscillation);
//   5. a claim carries the claimer's whole actor data - and health is still written through
//      the one writer that owns its revision, so a claim cannot move health silently.
//
// And the save barrier, which decides whether an hour of play was persisted:
//
//   6. a checkpoint is recorded only when every connected player reports its file written;
//   7. one refusal voids the barrier - no epoch is recorded, and the flight ring says "aborted";
//   8. an ack that arrives after the barrier is gone cannot commit anything;
//   9. a duplicate ack does not commit twice;
//  10. a player who leaves is no longer waited for;
//  11. the checkpoint file survives its own round trip - what the server writes is exactly what
//      it reads back, and a corrupted file is refused instead of becoming a world.
//
// The coordinator's own rules run in the fast loop (Code/tests/rfab_save_barrier.cpp). What
// was never observed is the wiring: who is told, and what the server records when a player
// refuses or answers too late.

#include <spdlog/sinks/stdout_color_sinks.h>

#include <ConsoleRegistry.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/PacketEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/CoordinatedSaveAck.h>
#include <Messages/RequestOwnershipClaim.h>

#include <Services/SaveSnapshotService.h>

#include "TransportSpy.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace
{
int g_failures = 0;

void Check(bool aCondition, const char* acWhat)
{
    if (aCondition)
    {
        std::printf("  ok   %s\n", acWhat);
        return;
    }
    std::printf("  FAIL %s\n", acWhat);
    ++g_failures;
}

template <class T> void Deliver(World& aWorld, T aPacket, Player* apSender)
{
    aWorld.GetDispatcher().enqueue<PacketEvent<T>>(&aPacket, apSender);
    aWorld.GetDispatcher().update();
}

// How many save events of one kind the server recorded for one epoch. The flight ring is the
// shipping record of what the save machinery did - it is what an operator reads after a lost
// session, so the harness reads it too instead of trusting private fields.
size_t CountSaveEvents(const SaveSnapshotCoordinator& acCoordinator, CoopFlight::FlightEventKind aKind,
                       uint64_t aEpochId)
{
    size_t count = 0;
    for (const auto& record : acCoordinator.GetFlightRing().SnapshotRecent())
    {
        if (record.Kind == aKind && record.AuxValue == aEpochId)
            ++count;
    }
    return count;
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& acPath)
{
    std::ifstream in(acPath, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteAllBytes(const std::filesystem::path& acPath, const std::vector<uint8_t>& acBytes)
{
    std::ofstream out(acPath, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(acBytes.data()), static_cast<std::streamsize>(acBytes.size()));
}

float HealthOf(const ActorValuesComponent& acValues)
{
    float hp = -1.0f;
    return acValues.TryGetHealth(hp) ? hp : -1.0f;
}
} // namespace

int main()
{
    // The console registry asserts on a logger named by its caller; the server logs through
    // "ConOut" as well.
    auto pConsoleLogger = spdlog::stdout_color_mt("ConOut");
    pConsoleLogger->set_level(spdlog::level::warn);
    spdlog::set_default_logger(pConsoleLogger);

    Console::ConsoleRegistry registry{"ConOut"};

    // The service restores persisted checkpoints from `cwd/Data` on construction. A file left by
    // a previous run would decide the starting epoch and session tag, so the scene would depend on
    // whatever ran before it: remove it before the server exists.
    std::error_code removeEc;
    std::filesystem::remove(std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin", removeEc);

    GameServer server{registry};

    World& world = server.GetWorld();
    const GameId kCell{0x0000003Cu, 0x00000000u}; // Whiterun, interior-free exterior cell
    Player* pOwner = world.GetPlayerManager().Create(1);
    Player* pAlly = world.GetPlayerManager().Create(2);
    if (pOwner == nullptr || pAlly == nullptr)
    {
        std::printf("FAIL could not create players\n");
        return 1;
    }

    // Both peers stand in the same cell as the NPC: range filtering is what decides
    // whether a report is even considered, and a player without a cell is nowhere.
    pOwner->SetCellComponent(CellIdComponent{kCell});
    pAlly->SetCellComponent(CellIdComponent{kCell});

    // An NPC in the world, simulated by pOwner.
    const entt::entity actor = world.create();
    world.emplace<FormIdComponent>(actor, 0x00012345u, 0u);
    world.emplace<CharacterComponent>(actor);
    world.emplace<OwnerComponent>(actor, pOwner);
    // A cell is what `SendToPlayersInRange` filters on; without it every broadcast is
    // dropped and the outbox assertions would test nothing.
    world.emplace<CellIdComponent>(actor, kCell);
    auto& values = world.emplace<ActorValuesComponent>(actor);
    values.CurrentActorValues.ActorMaxValuesList[24] = 100.0f;
    values.SetHealth(50.0f, 100.0f);

    const auto actorId = static_cast<uint32_t>(actor);
    const uint32_t revisionAfterSetup = values.HealthRevision;

    std::printf("health writes reachable from the wire\n");

    // 1. A hit from the owner.
    {
        RequestHealthChangeBroadcast hit{};
        hit.Id = actorId;
        hit.DeltaHealth = -20.0f;
        hit.HitId = 1;
        Deliver(world, hit, pOwner);

        Check(std::fabs(HealthOf(values) - 30.0f) < 0.01f, "an owner's hit lands on the server");
        Check(values.HealthRevision == revisionAfterSetup + 1, "a hit moves the health revision");
    }

    const uint32_t revisionAfterHit = values.HealthRevision;

    // 2. The killing blow.
    {
        RequestDeathStateChange death{};
        death.Id = actorId;
        death.IsDead = true;
        Deliver(world, death, pOwner);

        const auto& character = world.get<CharacterComponent>(actor);
        Check(character.IsDead(), "the killing blow is recorded");
        Check(std::fabs(HealthOf(values)) < 0.01f, "a dead actor has zero health");
        Check(values.HealthRevision == revisionAfterHit + 1, "death moves the health revision");
    }

    const uint32_t revisionAtDeath = values.HealthRevision;

    // 3. The reading that used to resurrect the corpse: sampled before the death, arriving after.
    {
        Harness::ClearOutbox();

        RequestActorValueChanges stale{};
        stale.Id = actorId;
        stale.HealthRevision = revisionAfterHit; // what the owner had applied when it read
        stale.Values[24] = 30.0f;
        Deliver(world, stale, pOwner);

        Check(std::fabs(HealthOf(values)) < 0.01f, "a pre-death reading cannot restore a corpse's health");
        Check(values.HealthRevision == revisionAtDeath, "a refused reading does not move the revision");
        Check(Harness::OutboxSize() > 0, "the sender is answered with the truth instead of silence");
    }

    // 4. A reading taken after the death is current, not stale.
    {
        RequestActorValueChanges fresh{};
        fresh.Id = actorId;
        fresh.HealthRevision = revisionAtDeath;
        fresh.Values[24] = 0.0f;
        Deliver(world, fresh, pOwner);

        Check(std::fabs(HealthOf(values)) < 0.01f, "a post-death reading agrees with the server");
        Check(values.HealthRevision == revisionAtDeath, "agreement does not churn the revision");
    }

    // 5. The claim path. `RequestOwnershipClaim` carries the claimer's whole actor data, and
    //    the server applies it wholesale - but health is the one number a revision describes,
    //    so it has to go through the single writer. Written straight into the map it changed
    //    health without moving the revision, and a reading sampled before the write still
    //    carried the current revision: the corpse case of Authority/Health.h §8E, still open
    //    on this path because the snapshot bypassed `SetHealth`.
    {
        // Unowned, so the claim is eligible (a fresh owner in range is protected by the hold
        // window), and unwounded, so a claimer reporting higher max health is believed - the
        // RFAB case where Fortify-Health armor lands on load and the peer's max grows.
        // Alive again: the scenarios above left it dead, and a claim cannot resurrect a corpse -
        // that is the protection 2. put there, not something to work around.
        world.get<CharacterComponent>(actor).SetDead(false);
        world.get<OwnerComponent>(actor).SetOwner(nullptr, 0);
        values.SetHealth(100.0f, 100.0f);
        const uint32_t revisionBeforeClaim = values.HealthRevision;

        ActorData claimerData{};
        claimerData.InitialActorValues.ActorValuesList[ActorValuesComponent::kHealthActorValueIndex] = 150.0f;
        claimerData.InitialActorValues.ActorMaxValuesList[ActorValuesComponent::kHealthActorValueIndex] = 150.0f;
        claimerData.IsDead = false;

        RequestOwnershipClaim claim{};
        claim.ServerId = actorId;
        claim.NewActorData = claimerData;
        Deliver(world, claim, pAlly);

        Check(world.get<OwnerComponent>(actor).GetOwner() == pAlly, "the claim is granted");
        Check(std::fabs(HealthOf(values) - 150.0f) < 0.01f, "a claim's actor data is applied");
        Check(values.HealthRevision == revisionBeforeClaim + 1, "a claim that moves health moves the revision");
    }

    // 6. And the reason the revision has to move: a claim reporting the actor dead must zero
    //    its health, and a reading sampled before that write must not revive it.
    {
        world.get<OwnerComponent>(actor).SetOwner(nullptr, 0);
        const uint32_t revisionBeforeCorpse = values.HealthRevision;

        ActorData corpseData{};
        corpseData.InitialActorValues.ActorValuesList[ActorValuesComponent::kHealthActorValueIndex] = 50.0f;
        corpseData.InitialActorValues.ActorMaxValuesList[ActorValuesComponent::kHealthActorValueIndex] = 150.0f;
        corpseData.IsDead = true;

        RequestOwnershipClaim claim{};
        claim.ServerId = actorId;
        claim.NewActorData = corpseData;
        Deliver(world, claim, pAlly);

        Check(std::fabs(HealthOf(values)) < 0.01f, "a claim that reports the actor dead leaves no health");
        Check(values.HealthRevision == revisionBeforeCorpse + 1, "the dead claim moves the revision once");
        Check(Authority::ClassifyHealthSnapshot(HealthOf(values), 50.0f, values.HealthRevision,
                                                revisionBeforeCorpse) == Authority::SnapshotVerdict::kStaleIncrease,
              "a reading sampled before the dead claim cannot revive the corpse");
    }

    std::printf("the save barrier\n");

    auto MakeAck = [](uint64_t aEpochId, const std::string& acSlot, bool aSuccess, const char* acReason) {
        CoordinatedSaveAck ack{};
        ack.EpochId = aEpochId;
        ack.SlotName = String(acSlot.c_str());
        ack.Success = aSuccess;
        ack.Reason = String(acReason);
        return ack;
    };

    // 6. Everyone reports its file written -> the epoch is recorded, once.
    {
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();
        const uint64_t epochBefore = coordinator.GetLastCommittedEpoch();

        Check(saves.TriggerCoordinatedSave(true, nullptr), "a save starts a barrier");
        const auto pending = coordinator.GetPendingBarrier();
        Check(pending.has_value(), "the barrier is armed");
        Check(pending.has_value() && pending->ExpectedPlayerIds.size() == 2, "every connected player is expected");
        Check(coordinator.GetLastCommittedEpoch() == epochBefore, "nothing is committed while players are saving");

        if (pending.has_value())
        {
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Check(coordinator.HasPendingBarrier(), "one ack is not enough");

            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pAlly);
            Check(!coordinator.HasPendingBarrier(), "the last ack closes the barrier");
            Check(coordinator.GetLastCommittedEpoch() == pending->EpochId, "the committed epoch is the barrier's epoch");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveCommitted, pending->EpochId) == 1,
                  "the commit is recorded once in the flight ring");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveAborted, pending->EpochId) == 0,
                  "a committed save is not also recorded as aborted");
        }
    }

    // 7. One refusal voids the barrier: nothing is recorded, and the abort is visible afterwards.
    {
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();
        const uint64_t epochBefore = coordinator.GetLastCommittedEpoch();

        Check(saves.TriggerCoordinatedSave(false, nullptr), "a manual save starts a barrier");
        const auto pending = coordinator.GetPendingBarrier();
        Check(pending.has_value(), "the manual barrier is armed");

        if (pending.has_value())
        {
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, false, "no disk space"), pAlly);

            Check(!coordinator.HasPendingBarrier(), "a refusal closes the barrier");
            Check(coordinator.GetLastCommittedEpoch() == epochBefore, "a refused save records no epoch");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveAborted, pending->EpochId) == 1,
                  "the abort is recorded in the flight ring");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveCommitted, pending->EpochId) == 0,
                  "a refused save is never recorded as committed");

            // 8. The refusing client finishes writing anyway and answers late. The barrier is gone,
            //    so the ack can only be ignored - it must not turn the abort into a commit.
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pAlly);
            Check(coordinator.GetLastCommittedEpoch() == epochBefore, "a late ack does not commit a voided barrier");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveCommitted, pending->EpochId) == 0,
                  "a late ack does not commit anything either");

            // 9. A duplicate of an ack that already counted changes nothing.
            const uint64_t committedEpoch = coordinator.GetLastCommittedEpoch();
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Check(coordinator.GetLastCommittedEpoch() == committedEpoch, "a duplicate ack does not move the epoch");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveCommitted, pending->EpochId) == 0,
                  "a duplicate ack does not commit twice");
        }
    }

    // 10. A player who leaves while its peers are saving is no longer waited for: the barrier
    //     resolves against the players that are still there instead of timing out.
    {
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();

        Check(saves.TriggerCoordinatedSave(true, nullptr), "a save starts a barrier with three scenarios behind it");
        const auto pending = coordinator.GetPendingBarrier();
        if (pending.has_value())
        {
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Check(coordinator.HasPendingBarrier(), "the save still waits for the other player");

            world.GetDispatcher().trigger(PlayerLeaveEvent{pAlly});
            world.GetDispatcher().update();

            Check(!coordinator.HasPendingBarrier(), "the player who left is no longer waited for");
            Check(coordinator.GetLastCommittedEpoch() == pending->EpochId,
                  "the remaining acks are enough to record the checkpoint");
        }
    }

    std::printf("the checkpoint file\n");

    // 11. A rollback is only as good as the file it reads. The format is a v2.0 body plus two
    //     trailing extension blocks (NPC spatial state, v2.0.3 identity/ledger state), each behind
    //     its own magic, and a reader that falls out of step by a single field restores a
    //     different world without complaining. Nothing exercised that path end to end.
    {
        auto& saves = world.GetSaveSnapshotService();
        const auto dataDir = std::filesystem::current_path() / "Data";
        std::error_code ec;
        std::filesystem::create_directories(dataDir, ec);
        const auto firstPath = dataDir / "rfab_harness_roundtrip_a.bin";
        const auto secondPath = dataDir / "rfab_harness_roundtrip_b.bin";

        // The same state must produce the same bytes: a checkpoint that reorders its own contents
        // between two writes cannot be compared, deduplicated or trusted.
        Check(saves.SaveCheckpointsToDisk(firstPath), "a checkpoint file is written");
        Check(saves.SaveCheckpointsToDisk(secondPath), "the same state writes a second file");
        Check(ReadAllBytes(firstPath) == ReadAllBytes(secondPath), "the same state writes identical bytes twice");

        const size_t snapshotCount = saves.GetSnapshotCount();
        const uint64_t committedEpoch = saves.GetLastCommittedEpoch();
        const std::string committedSlot = saves.GetLastCommittedSlot();

        Check(saves.LoadCheckpointsFromDisk(firstPath), "the file reads back");
        Check(saves.GetSnapshotCount() == snapshotCount, "the snapshot count survives the round trip");
        Check(saves.GetLastCommittedEpoch() == committedEpoch, "the committed epoch survives the round trip");
        Check(saves.GetLastCommittedSlot() == committedSlot, "the committed slot survives the round trip");

        Check(saves.SaveCheckpointsToDisk(secondPath), "the loaded state writes again");
        Check(ReadAllBytes(firstPath) == ReadAllBytes(secondPath),
              "load-then-save reproduces the file byte for byte");

        // A single flipped byte must be caught by the envelope hash, and a refused file must leave
        // the loaded state alone rather than half-applying it.
        auto corrupted = ReadAllBytes(firstPath);
        if (!corrupted.empty())
        {
            corrupted[corrupted.size() / 2] ^= 0xFF;
            WriteAllBytes(secondPath, corrupted);

            Check(!saves.LoadCheckpointsFromDisk(secondPath), "a corrupted checkpoint file is refused");
            Check(saves.GetLastCommittedEpoch() == committedEpoch, "a refused file leaves the state alone");
            Check(saves.GetSnapshotCount() == snapshotCount, "a refused file does not replace the history");
        }

        std::filesystem::remove(firstPath, ec);
        std::filesystem::remove(secondPath, ec);
    }

    // Leave no checkpoint history behind: the scenes above wrote one, and the next run would
    // otherwise restore it before the first assertion.
    std::filesystem::remove(std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin", removeEc);

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
