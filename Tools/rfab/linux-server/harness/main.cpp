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
//  11. the checkpoint file survives its own round trip and corruption is refused;
//  12. a rollback restores health and moves the revision;
//  13. a fresh server rebuilds actors, objects, progression, loot and calendar from the
//      checkpoint; the first join cannot overwrite the restored calendar;
//  14. after an actual process restart, a reused player ID cannot reclaim depleted loot,
//      including after rollback to either the latest or an older on-disk checkpoint.
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
#include <Events/PlayerJoinEvent.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/CoordinatedSaveAck.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyRelinquishControl.h>
#include <Messages/NotifySpawnData.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Messages/ServerMessageFactory.h>

#include <Services/CharacterService.h>
#include <Services/PartyService.h>
#include <Services/SaveSnapshotService.h>

#include "TransportSpy.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace
{
int g_failures = 0;

// Never let the harness delete or overwrite a real server's Data/rfab_checkpoints.bin.
class TemporaryWorkingDirectory
{
public:
    TemporaryWorkingDirectory() : m_original(std::filesystem::current_path())
    {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 100; ++i)
        {
            m_path = std::filesystem::temp_directory_path() /
                     ("rfab-server-harness-" + std::to_string(seed) + "-" + std::to_string(i));
            std::error_code ec;
            if (std::filesystem::create_directory(m_path, ec))
            {
                std::filesystem::current_path(m_path);
                return;
            }
            if (ec)
                throw std::filesystem::filesystem_error("create harness directory", m_path, ec);
        }
        throw std::runtime_error("could not create a unique harness directory");
    }

    ~TemporaryWorkingDirectory()
    {
        std::error_code ec;
        std::filesystem::current_path(m_original, ec);
        if (!ec)
            std::filesystem::remove_all(m_path, ec);
    }

private:
    std::filesystem::path m_original;
    std::filesystem::path m_path;
};

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

// Serializes a client packet onto a real TiltedPhoques::Buffer, extracts it through
// ClientMessageFactory::Extract (exercising the real wire bitstream parser), and
// dispatches the decoded message into the shipping server World.
template <class T> bool DeliverWire(World& aWorld, const T& acOutgoing, Player* apSender)
{
    TiltedPhoques::Buffer raw(65536);
    TiltedPhoques::Buffer::Writer writer(&raw);
    acOutgoing.Serialize(writer);

    TiltedPhoques::Buffer::Reader reader(&raw);
    const ClientMessageFactory factory;
    auto pExtracted = factory.Extract(reader);
    if (!pExtracted || pExtracted->GetOpcode() != acOutgoing.GetOpcode())
        return false;

    auto* pTyped = static_cast<T*>(pExtracted.get());
    aWorld.GetDispatcher().enqueue<PacketEvent<T>>(pTyped, apSender);
    aWorld.GetDispatcher().update();
    return true;
}

// Extracts all server packets sent to `aConn` (or broadcast via k_HSteamNetConnection_Invalid)
// from Harness::Outbox() using the real ServerMessageFactory::Extract bitstream decoder.
template <class TServerMsg>
std::vector<TServerMsg> DecodeOutboxMessagesFor(TiltedPhoques::ConnectionId_t aConn)
{
    std::vector<TServerMsg> decoded;
    const ServerMessageFactory factory;
    for (const auto& pkt : Harness::Outbox())
    {
        if (pkt.Connection != aConn && pkt.Connection != k_HSteamNetConnection_Invalid)
            continue;
        if (pkt.Bytes.empty())
            continue;

        // Pad with 16 zero bytes so TiltedCore's 64-bit word bit-reader never reads past allocation.
        TiltedPhoques::Buffer buf(pkt.Bytes.size() + 16);
        std::memset(buf.GetWriteData(), 0, pkt.Bytes.size() + 16);
        std::memcpy(buf.GetWriteData(), pkt.Bytes.data(), pkt.Bytes.size());
        TiltedPhoques::Buffer::Reader reader(&buf);
        auto pMsg = factory.Extract(reader);
        if (pMsg && pMsg->GetOpcode() == TServerMsg::Opcode)
            decoded.push_back(*static_cast<const TServerMsg*>(pMsg.get()));
    }
    return decoded;
}

template <class TMap>
bool HasApproxValue(const TMap& acMap, uint32_t aKey, float aExpected, float aEps = 0.01f)
{
    const auto it = acMap.find(aKey);
    return it != acMap.end() && std::fabs(it->second - aExpected) < aEps;
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

size_t CountCharactersByFormId(World& aWorld, const GameId& acFormId)
{
    size_t count = 0;
    auto view = aWorld.view<FormIdComponent, CharacterComponent>();
    for (const entt::entity entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id == acFormId)
            ++count;
    }
    return count;
}

float HealthOf(const ActorValuesComponent& acValues)
{
    float hp = -1.0f;
    return acValues.TryGetHealth(hp) ? hp : -1.0f;
}
// Executed with exec in a separate process: unlike a second World in this process, the static
// Player ID generator starts at 1. The parent has left a checkpoint whose claimant was player 1.
int CheckRestartedLoot()
{
    Console::ConsoleRegistry registry{"ConOut"};
    GameServer server{registry};
    World& world = server.GetWorld();
    Player* pPlayer = world.GetPlayerManager().Create(3);
    Check(pPlayer && pPlayer->GetId() == 1, "a new process reuses the former claimant's player ID");
    Check(world.GetSaveSnapshotService().GetSnapshotCount() >= 2, "the new process loads checkpoint history");
    if (!pPlayer)
        return 1;

    const GameId containerId{0u, 0x00054321u};
    const GameId itemId{0u, 0x00023456u};
    const entt::entity playerEntity = world.create();
    world.emplace<InventoryComponent>(playerEntity);
    pPlayer->SetCharacter(playerEntity);
    const auto attempt = [&]() {
        entt::entity container = entt::null;
        auto objects = world.view<FormIdComponent, ObjectComponent, InventoryComponent>();
        for (const auto entity : objects)
            if (objects.get<FormIdComponent>(entity).Id == containerId)
                container = entity;
        if (container == entt::null)
            return false;
        auto& inventory = world.get<InventoryComponent>(playerEntity).Content;
        inventory = Inventory{};
        Inventory::Entry item{};
        item.BaseId = itemId;
        item.Count = 1;
        inventory.AddOrRemoveEntry(item); // the client already awarded itself the item
        RequestInventoryChanges withdrawal{};
        withdrawal.ServerId = World::ToInteger(container);
        withdrawal.Item = item;
        withdrawal.Item.Count = -1;
        withdrawal.UpdateClients = false;
        Deliver(world, withdrawal, pPlayer);
        GameId lookup = itemId;
        return inventory.GetEntryCountById(lookup) == 0;
    };
    Check(attempt(), "reused player ID cannot withdraw previously depleted loot after restart");
    Check(world.GetSaveSnapshotService().TriggerRollbackToLatest(false), "the persisted checkpoint can be rolled back");
    Check(attempt(), "rollback to a persisted checkpoint still rejects the reused player ID");
    const uint64_t latestEpoch = world.GetSaveSnapshotService().GetLastCommittedEpoch();
    Check(world.GetSaveSnapshotService().TriggerRollbackToPrevious(), "an older persisted checkpoint can be rolled back");
    Check(world.GetSaveSnapshotService().GetLastCommittedEpoch() < latestEpoch, "rollback selected an older epoch");
    Check(attempt(), "rollback to an older persisted checkpoint also rejects the reused player ID");
    return g_failures == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    // The console registry asserts on a logger named by its caller; the server logs through
    // "ConOut" as well.
    auto pConsoleLogger = spdlog::stdout_color_mt("ConOut");
    pConsoleLogger->set_level(spdlog::level::warn);
    spdlog::set_default_logger(pConsoleLogger);

    if (argc == 2 && std::string(argv[1]) == "--check-restarted-loot")
        return CheckRestartedLoot();

    TemporaryWorkingDirectory isolatedDirectory;
    Console::ConsoleRegistry registry{"ConOut"};

    // The service restores persisted checkpoints from `cwd/Data` on construction. A file left by
    // a previous run would decide the starting epoch and session tag, so the scene would depend on
    // whatever ran before it: remove it before the server exists.
    std::error_code removeEc;
    std::filesystem::remove(std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin", removeEc);

    const GameId restartActorFormId{0u, 0x00012345u};
    const GameId restartContainerId{0u, 0x00054321u};
    const GameId depletedItemId{0u, 0x00023456u};
    const GameId remainingItemId{0u, 0x00034567u};
    uint64_t restartEpoch = 0;
    {
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

    // With no checkpoint, the first host still seeds the calendar.
    Check(world.GetSaveSnapshotService().GetSnapshotCount() == 0, "a new server has no checkpoint");
    TimeModel initialHostTime{};
    initialHostTime.Time = 2.0f;
    initialHostTime.Day = 2;
    initialHostTime.Month = 1;
    initialHostTime.Year = 100;
    world.GetDispatcher().trigger(PlayerJoinEvent{pOwner, {}, {}, DateTime{initialHostTime}});
    Check(world.GetCalendarService().GetDate() == CalendarService::TDate{2, 1, 100},
          "a new server seeds its date from the first player");
    Check(world.GetCalendarService().GetTime() == CalendarService::TTime{2, 0},
          "a new server seeds its time from the first player");

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

    std::printf("the rollback path\n");

    // 12. A rollback is an absolute write of the checkpoint's health, like a death or a transfer -
    //     so it has to move the revision. Written straight into the map it does not, and a reading
    //     that was in flight before the rollback still carries the current revision: the restore is
    //     undone, and a boss the party wiped comes back at whatever the last straggler saw.
    {
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();

        world.get<CharacterComponent>(actor).SetDead(false);
        values.SetHealth(40.0f, 100.0f);
        Check(std::fabs(HealthOf(values) - 40.0f) < 0.01f, "the actor is alive at 40 of 100 before the checkpoint");

        Check(saves.TriggerCoordinatedSave(true, nullptr), "a save starts for the checkpoint");
        const auto pending = coordinator.GetPendingBarrier();
        if (pending.has_value())
        {
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pAlly);
        }
        Check(saves.GetSnapshotCount() > 0, "the checkpoint is recorded before the rollback");

        // After the checkpoint: the fight is lost, the actor is dead.
        values.SetHealth(0.0f, 100.0f);
        world.get<CharacterComponent>(actor).SetDead(true);
        const uint32_t revisionAtDeath = values.HealthRevision;
        Check(std::fabs(HealthOf(values)) < 0.01f, "the actor is dead before the rollback");

        Check(saves.TriggerRollbackToLatest(false), "the rollback runs");

        Check(world.valid(actor), "the checkpoint's actor survives the rollback");
        if (world.valid(actor))
        {
            auto* pChar = world.try_get<CharacterComponent>(actor);
            auto* pAv = world.try_get<ActorValuesComponent>(actor);
            Check(pChar != nullptr && !pChar->IsDead(), "the rollback brings the actor back alive");
            if (pAv != nullptr)
            {
                float restoredHp = -1.0f;
                (void)pAv->TryGetHealth(restoredHp);
                Check(std::fabs(restoredHp - 40.0f) < 0.01f, "the rollback restores the checkpoint's health");
                Check(pAv->HealthRevision != revisionAtDeath, "the restore moves the health revision");
                Check(Authority::ClassifyHealthSnapshot(restoredHp, 55.0f, pAv->HealthRevision, revisionAtDeath) ==
                          Authority::SnapshotVerdict::kStaleIncrease,
                      "a reading sampled before the rollback cannot undo the restore");
            }
        }
    }

    std::printf("the restart path\n");
    {
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();
        auto& calendar = world.GetCalendarService();
        auto& ledger = world.GetRfabProgressionService().GetLedger();
        // Persistent actors need spatial state to be recreated after startup.
        auto& movement = world.get_or_emplace<MovementComponent>(actor);
        movement.Position = {123.0f, 456.0f, 789.0f};
        world.get<CharacterComponent>(actor).SetDead(false);
        values.SetHealth(40.0f, 100.0f);
        Check(calendar.SetDate(12, 4, 205), "the checkpoint date is set");
        Check(calendar.SetTime(13, 17, 33.0f), "the checkpoint time is set");
        Check(ledger.TryGrantXp("restart-test", 123).Accepted, "the checkpoint progression is set");
        Check(pOwner->GetId() == 1, "the checkpoint claimant is player ID 1");

        LockData lock{};
        lock.IsLocked = true;
        lock.LockLevel = 42;
        Inventory contents{};
        Inventory::Entry depleted{};
        depleted.BaseId = depletedItemId;
        depleted.Count = 3;
        contents.AddOrRemoveEntry(depleted);
        Inventory::Entry remaining{};
        remaining.BaseId = remainingItemId;
        remaining.Count = 7;
        contents.AddOrRemoveEntry(remaining);
        world.GetObjectService().UpsertRestoredObject(restartContainerId, kCell, {}, {}, lock, contents);
        entt::entity container = entt::null;
        auto objects = world.view<FormIdComponent, ObjectComponent, InventoryComponent>();
        for (const auto entity : objects)
            if (objects.get<FormIdComponent>(entity).Id == restartContainerId)
                container = entity;
        Check(container != entt::null, "the checkpoint container exists");
        if (container != entt::null)
        {
            RequestInventoryChanges withdrawal{};
            withdrawal.ServerId = World::ToInteger(container);
            withdrawal.Item = depleted;
            withdrawal.Item.Count = -3;
            withdrawal.UpdateClients = false;
            Deliver(world, withdrawal, pOwner);
            const auto depletion = world.GetInventoryService().CaptureLootLedgerSnapshot();
            Check(depletion.size() == 1, "a real withdrawal records the depletion");
        }

        Check(saves.TriggerCoordinatedSave(true, nullptr), "a save starts for the restart checkpoint");
        const auto pending = coordinator.GetPendingBarrier();
        Check(pending.has_value(), "the restart checkpoint has a pending barrier");
        if (pending.has_value())
        {
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pAlly);
            Check(coordinator.GetLastCommittedEpoch() == pending->EpochId, "the restart checkpoint commits");
            restartEpoch = pending->EpochId;
        }
        // Keep two consecutive checkpoints containing this depletion, to exercise older history.
        Check(saves.TriggerCoordinatedSave(true, nullptr), "a second restart checkpoint starts");
        const auto secondPending = coordinator.GetPendingBarrier();
        Check(secondPending.has_value(), "the second checkpoint has a pending barrier");
        if (secondPending)
        {
            Deliver(world, MakeAck(secondPending->EpochId, secondPending->SlotName, true, ""), pOwner);
            Deliver(world, MakeAck(secondPending->EpochId, secondPending->SlotName, true, ""), pAlly);
            Check(coordinator.GetLastCommittedEpoch() == secondPending->EpochId, "the second checkpoint commits");
            restartEpoch = secondPending->EpochId;
        }
        Check(saves.TriggerRollbackToLatest(false), "an in-process checkpoint can be rolled back");
        const auto sameProcessLedger = world.GetInventoryService().CaptureLootLedgerSnapshot();
        Check(sameProcessLedger.size() == 1 && sameProcessLedger[0].ClaimantPlayerId == pOwner->GetId(),
              "an in-process rollback keeps the current process's claimant ID");
        Check(saves.SaveCheckpointsToDisk(SaveSnapshotService::GetDefaultCheckpointStorePath()),
              "the restart checkpoint is written to the default path");
    }
    } // Destroy the first server and its world before constructing the next one.

    std::printf("the real process restart path\n");
    const auto storePath = SaveSnapshotService::GetDefaultCheckpointStorePath();
    const auto originalCheckpoint = ReadAllBytes(storePath);
    Check(!originalCheckpoint.empty(), "the child receives a nonempty checkpoint");
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0)
    {
        execl("/proc/self/exe", "servertests", "--check-restarted-loot", static_cast<char*>(nullptr));
        _exit(127);
    }
    int childStatus = 0;
    const bool childWaited = child > 0 && waitpid(child, &childStatus, 0) == child;
    Check(childWaited && WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0,
          "the checkpoint survives a fresh process and a reused player ID");
    // Child rollbacks rewrote the store; restore the original file for the independent tests below.
    if (!originalCheckpoint.empty())
        WriteAllBytes(storePath, originalCheckpoint);

    {
        Console::ConsoleRegistry restartRegistry{"ConOut"};
        GameServer restarted{restartRegistry};
        World& restored = restarted.GetWorld();
        const auto& saves = restored.GetSaveSnapshotService();
        Check(restartEpoch != 0 && saves.GetLastCommittedEpoch() == restartEpoch,
              "a fresh server reads the committed epoch in its constructor");
        const auto rewrittenPath = std::filesystem::current_path() / "Data" / "checkpoint-rewritten.bin";
        Check(saves.SaveCheckpointsToDisk(rewrittenPath), "the loaded checkpoint can be written again");
        Check(ReadAllBytes(rewrittenPath) == originalCheckpoint,
              "forgetting runtime claimant IDs does not alter the checkpoint file");
        Check(CountCharactersByFormId(restored, restartActorFormId) == 1,
              "a fresh server rebuilds the persistent actor exactly once");
        auto actors = restored.view<FormIdComponent, CharacterComponent>();
        for (const entt::entity entity : actors)
        {
            if (actors.get<FormIdComponent>(entity).Id != restartActorFormId)
                continue;
            auto* pAv = restored.try_get<ActorValuesComponent>(entity);
            float hp = -1.0f;
            Check(pAv != nullptr && pAv->TryGetHealth(hp) && std::fabs(hp - 40.0f) < 0.01f,
                  "the rebuilt actor carries the checkpoint's health");
            Check(!actors.get<CharacterComponent>(entity).IsDead(), "the rebuilt actor is alive");
            Check(restored.all_of<OwnerComponent>(entity) && restored.get<OwnerComponent>(entity).GetOwner() == nullptr,
                  "the rebuilt actor starts without an owner");
            const auto* pMov = restored.try_get<MovementComponent>(entity);
            Check(pMov && std::fabs(pMov->Position.x - 123.0f) < 0.01f &&
                      std::fabs(pMov->Position.y - 456.0f) < 0.01f &&
                      std::fabs(pMov->Position.z - 789.0f) < 0.01f,
                  "the rebuilt actor carries the checkpoint's position");
        }
        auto restoredObjects = restored.view<FormIdComponent, ObjectComponent, InventoryComponent>();
        size_t containerCount = 0;
        entt::entity restoredContainer = entt::null;
        for (const auto entity : restoredObjects)
        {
            if (restoredObjects.get<FormIdComponent>(entity).Id == restartContainerId)
            {
                ++containerCount;
                restoredContainer = entity;
            }
        }
        Check(containerCount == 1, "the fresh server rebuilds the checkpoint container exactly once");
        if (restoredContainer != entt::null)
        {
            const auto& obj = restoredObjects.get<ObjectComponent>(restoredContainer);
            const auto& inventory = restoredObjects.get<InventoryComponent>(restoredContainer).Content;
            GameId lookupDepleted = depletedItemId;
            GameId lookupRemaining = remainingItemId;
            Check(obj.CurrentLockData.IsLocked && obj.CurrentLockData.LockLevel == 42,
                  "the restored container keeps its lock");
            Check(inventory.GetEntryCountById(lookupDepleted) == 0 &&
                      inventory.GetEntryCountById(lookupRemaining) == 7,
                  "the restored container keeps its checkpoint inventory");
            const auto depletion = restored.GetInventoryService().CaptureLootLedgerSnapshot();
            const auto stableKey = restored.GetInventoryService().ResolveLootContainerKey(restoredContainer, 0);
            Check(depletion.size() == 1 && depletion[0].ContainerServerId == stableKey &&
                      depletion[0].ModId == depletedItemId.ModId && depletion[0].BaseId == depletedItemId.BaseId &&
                      depletion[0].ClaimantPlayerId == 0,
                  "the depletion keeps its stable key but its old claimant is unknown");
        }
        const auto progress = restored.GetRfabProgressionService().GetLedger().CaptureSnapshot();
        Check(progress.GrantedXpBySource.count("restart-test") == 1,
              "the fresh server restores the progression ledger");
        Check(restored.GetCalendarService().GetDate() == CalendarService::TDate{12, 4, 205},
              "the fresh server restores the checkpoint date");
        Check(restored.GetCalendarService().GetTime() == CalendarService::TTime{13, 17},
              "the fresh server restores the checkpoint time");
        // GameServer::UpdateTimeScale deliberately reapplies its configured scale at startup.
        Check(std::fabs(restored.GetCalendarService().GetTimeScale() - 20.0f) < 0.01f,
              "the configured time scale still takes precedence at server startup");
        Check(!restored.GetCalendarService().RestoreCheckpoint(99, 17, 20.0f, 12, 4, 205),
              "an invalid checkpoint calendar is rejected");
        Check(restored.GetCalendarService().GetTime() == CalendarService::TTime{13, 17} &&
                  restored.GetCalendarService().GetDate() == CalendarService::TDate{12, 4, 205},
              "a rejected calendar leaves the existing state untouched");
        Player* pHost = restored.GetPlayerManager().Create(3);
        Check(pHost != nullptr, "a player can join the restarted server");
        if (pHost)
        {
            TimeModel hostTime{};
            hostTime.Time = 2.0f;
            hostTime.Day = 2;
            hostTime.Month = 1;
            hostTime.Year = 100;
            restored.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{hostTime}});
            Check(restored.GetCalendarService().GetDate() == CalendarService::TDate{12, 4, 205},
                  "the first player does not overwrite the restored date");
            Check(restored.GetCalendarService().GetTime() == CalendarService::TTime{13, 17},
                  "the first player does not overwrite the restored time");
            if (restoredContainer != entt::null)
            {
                // Model a client that already awarded itself a duplicated item. The server must
                // use the persisted depletion to remove it again through the normal packet path.
                const entt::entity playerEntity = restored.create();
                auto& playerInventory = restored.emplace<InventoryComponent>(playerEntity).Content;
                Inventory::Entry duplicate{};
                duplicate.BaseId = depletedItemId;
                duplicate.Count = 1;
                playerInventory.AddOrRemoveEntry(duplicate);
                pHost->SetCharacter(playerEntity);
                RequestInventoryChanges lateWithdrawal{};
                lateWithdrawal.ServerId = World::ToInteger(restoredContainer);
                lateWithdrawal.Item = duplicate;
                lateWithdrawal.Item.Count = -1;
                lateWithdrawal.UpdateClients = false;
                Deliver(restored, lateWithdrawal, pHost);
                GameId lookup = depletedItemId;
                Check(playerInventory.GetEntryCountById(lookup) == 0,
                      "the restored depletion rejects a late peer's duplicate withdrawal");
            }
        }
    }

    // Leave no checkpoint history behind: the scenes above wrote one, and the next run would
    // otherwise restore it before the first assertion.
    std::filesystem::remove(std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin", removeEc);

    // =========================================================================
    // v4.0.0 Full Wire-Decoded Multi-Peer Protocol & Authority Integration Suite
    // =========================================================================
    std::printf("v4.0.0 wire-decoded multi-peer protocol & authority\n");
    {
        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 101;
        constexpr TiltedPhoques::ConnectionId_t kGuest2Conn = 102;
        constexpr TiltedPhoques::ConnectionId_t kGuest3Conn = 103;
        const GameId kDungeonCell{0u, 0x0009ABCDu};
        const GameId kFarCell{0u, 0x0009FFFEu};
        const GameId kPlacedRefId{1u, 0x000E5F72u};
        const GameId kRestlessDraugrBase{2u, 0x00023BC0u};
        const GameId kDraugrScourgeBase{2u, 0x00023BC1u};

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest2 = world.GetPlayerManager().Create(kGuest2Conn);
        Player* pGuest3 = world.GetPlayerManager().Create(kGuest3Conn);
        Check(pHost && pGuest2 && pGuest3, "three co-op peers connect to the server");

        TimeModel t0{};
        t0.Time = 12.0f;
        t0.Day = 5;
        t0.Month = 3;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest2, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest3, {}, {}, DateTime{t0}});

        Check(world.GetPartyService().IsPlayerLeader(pHost) &&
                  world.GetPartyService().IsPlayerInParty(pGuest2) &&
                  world.GetPartyService().IsPlayerInParty(pGuest3),
              "auto-party binds Host as party leader and both guests as party members");

        pHost->SetCellComponent(CellIdComponent{kFarCell});
        pGuest2->SetCellComponent(CellIdComponent{kDungeonCell});
        pGuest3->SetCellComponent(CellIdComponent{kFarCell});

        // 1. Guest2 streams the dungeon cell first and assigns a placed TESLevCharacter
        //    rolled locally as Restless Draugr (250/250 HP). Everything below is verified
        //    strictly from ServerMessageFactory-decoded wire bytes — zero server RAM peeking!
        Harness::ClearOutbox();
        AssignCharacterRequest g2Assign{};
        g2Assign.Cookie = 11u;
        g2Assign.ReferenceId = kPlacedRefId;
        g2Assign.FormId = kRestlessDraugrBase;
        g2Assign.CellId = kDungeonCell;
        g2Assign.Position = {100.0f, 200.0f, 0.0f};
        g2Assign.CurrentActorData.BaseId = kRestlessDraugrBase;
        g2Assign.CurrentActorData.InitialActorValues.ActorValuesList[24] = 250.0f;
        g2Assign.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 250.0f;
        Check(DeliverWire(world, g2Assign, pGuest2), "Guest2 AssignCharacterRequest round-trips over the wire");

        const auto g2AssignResps = DecodeOutboxMessagesFor<AssignCharacterResponse>(kGuest2Conn);
        Check(g2AssignResps.size() == 1 && g2AssignResps[0].Owner && g2AssignResps[0].OwnershipEpoch >= 1 &&
                  g2AssignResps[0].HealthRevision >= 1,
              "AssignCharacterResponse delivers initial OwnershipEpoch and non-zero HealthRevision on the wire");

        const uint32_t npcServerId = g2AssignResps.empty() ? 0u : g2AssignResps[0].ServerId;
        uint64_t g2KnownEpoch = g2AssignResps.empty() ? 0ull : g2AssignResps[0].OwnershipEpoch;
        uint32_t g2KnownHealthRev = g2AssignResps.empty() ? 0u : g2AssignResps[0].HealthRevision;

        // 2. Guest2 (owner) lands a -40 HP hit -> server sends empty-Values NotifyActorValueChanges
        //    revision ack back to Guest2 so Guest2's next regen snapshot is NOT falsely rejected!
        Harness::ClearOutbox();
        RequestHealthChangeBroadcast g2Hit{};
        g2Hit.Id = npcServerId;
        g2Hit.DeltaHealth = -40.0f;
        g2Hit.HitId = 501u;
        Check(DeliverWire(world, g2Hit, pGuest2), "Guest2 RequestHealthChangeBroadcast round-trips over the wire");

        const auto g2HitAcks = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuest2Conn);
        Check(g2HitAcks.size() == 1 && g2HitAcks[0].Id == npcServerId && g2HitAcks[0].Values.empty() &&
                  g2HitAcks[0].HealthRevision == g2KnownHealthRev + 1,
              "owner health delta emits an empty-Values NotifyActorValueChanges HealthRevision ack on the wire");
        if (!g2HitAcks.empty())
            g2KnownHealthRev = g2HitAcks[0].HealthRevision;

        // Guest3 enters the cell to observe Guest2's subsequent regen broadcast.
        pGuest3->SetCellComponent(CellIdComponent{kDungeonCell});
        world.GetCharacterService().ReconcileAllEntitiesForObserver(pGuest3);
        const auto g3InitialSpawns = DecodeOutboxMessagesFor<CharacterSpawnRequest>(kGuest3Conn);
        Check(!g3InitialSpawns.empty() && g3InitialSpawns.back().ServerId == npcServerId &&
                  g3InitialSpawns.back().BaseId == kRestlessDraugrBase &&
                  g3InitialSpawns.back().HealthRevision == g2KnownHealthRev,
              "CharacterSpawnRequest delivers BaseId and current HealthRevision to arriving observer");

        // Now Guest2 reports legitimate +15 HP Requiem troll/draugr regen (210 -> 225) echoing g2KnownHealthRev.
        Harness::ClearOutbox();
        RequestActorValueChanges g2Regen{};
        g2Regen.Id = npcServerId;
        g2Regen.HealthRevision = g2KnownHealthRev;
        g2Regen.Values[24] = 225.0f;
        Check(DeliverWire(world, g2Regen, pGuest2), "Guest2 regen RequestActorValueChanges round-trips over the wire");

        const auto g2RegenAcks = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuest2Conn);
        const auto g3RegenBroadcasts = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuest3Conn);
        Check(g2RegenAcks.size() == 1 && g2RegenAcks[0].Values.empty() &&
                  g2RegenAcks[0].HealthRevision == g2KnownHealthRev + 1,
              "legitimate owner regen echoing acked HealthRevision is accepted and acked (never kStaleIncrease)");
        Check(g3RegenBroadcasts.size() == 1 &&
                  HasApproxValue(g3RegenBroadcasts[0].Values, 24u, 225.0f) &&
                  g3RegenBroadcasts[0].HealthRevision == g2KnownHealthRev + 1,
              "accepted owner regen broadcasts updated HP=225 and new HealthRevision to observers");
        if (!g2RegenAcks.empty())
            g2KnownHealthRev = g2RegenAcks[0].HealthRevision;

        // 3. Party Leader (pHost) enters the cell where RFAB rolled Draugr Scourge for the SAME placed ref!
        //    Verify calm leader takeover preserves wounded HP=225 AND propagates Draugr Scourge BaseId to peers!
        pHost->SetCellComponent(CellIdComponent{kDungeonCell});
        Harness::ClearOutbox();
        AssignCharacterRequest hostAssign{};
        hostAssign.Cookie = 22u;
        hostAssign.ReferenceId = kPlacedRefId;
        hostAssign.FormId = kDraugrScourgeBase;
        hostAssign.CellId = kDungeonCell;
        hostAssign.Position = {100.0f, 200.0f, 0.0f};
        hostAssign.CurrentActorData.BaseId = kDraugrScourgeBase;
        hostAssign.CurrentActorData.InitialActorValues.ActorValuesList[24] = 400.0f;
        hostAssign.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 400.0f;
        Check(DeliverWire(world, hostAssign, pHost), "Host AssignCharacterRequest round-trips over the wire");

        const auto hostAssignResps = DecodeOutboxMessagesFor<AssignCharacterResponse>(kHostConn);
        const auto g2Relinquishes = DecodeOutboxMessagesFor<NotifyRelinquishControl>(kGuest2Conn);
        const auto g2SpawnDatas = DecodeOutboxMessagesFor<NotifySpawnData>(kGuest2Conn);
        const auto g3SpawnDatas = DecodeOutboxMessagesFor<NotifySpawnData>(kGuest3Conn);

        Check(hostAssignResps.size() == 1 && hostAssignResps[0].Owner &&
                  hostAssignResps[0].OwnershipEpoch > g2KnownEpoch &&
                  hostAssignResps[0].HealthRevision >= g2KnownHealthRev &&
                  HasApproxValue(hostAssignResps[0].AllActorValues.ActorValuesList, 24u, 225.0f),
              "Party Leader calm takeover grants new epoch, carries HealthRevision, and preserves wounded HP=225");
        Check(g2Relinquishes.size() == 1 && g2Relinquishes[0].ServerId == npcServerId &&
                  g2Relinquishes[0].OwnershipEpoch == hostAssignResps[0].OwnershipEpoch,
              "previous owner receives NotifyRelinquishControl with matching authoritative epoch");
        Check(!g2SpawnDatas.empty() && g2SpawnDatas.back().NewActorData.BaseId == kDraugrScourgeBase &&
                  !g3SpawnDatas.empty() && g3SpawnDatas.back().NewActorData.BaseId == kDraugrScourgeBase &&
                  g2SpawnDatas.back().HealthRevision == hostAssignResps[0].HealthRevision,
              "TESLevCharacter static BaseId parity propagates to all peers via NotifySpawnData on takeover");

        uint64_t hostEpoch = hostAssignResps[0].OwnershipEpoch;
        uint32_t authHealthRev = hostAssignResps[0].HealthRevision;

        // 4. Pure server-granted RequestOwnershipClaim rules:
        //    (a) Out-of-range peer claim is rejected with NotifyRelinquishControl.
        //    (b) Current owner redundant claim (kAlreadyOwner) does NOT emit NotifyOwnershipTransfer (no loop!).
        pGuest3->SetCellComponent(CellIdComponent{kFarCell});
        Harness::ClearOutbox();
        RequestOwnershipClaim farClaim{};
        farClaim.ServerId = npcServerId;
        farClaim.NewActorData = hostAssign.CurrentActorData;
        farClaim.NewActorData.InitialActorValues.ActorValuesList[24] = 225.0f;
        Check(DeliverWire(world, farClaim, pGuest3), "out-of-range RequestOwnershipClaim round-trips over the wire");
        const auto farRejects = DecodeOutboxMessagesFor<NotifyRelinquishControl>(kGuest3Conn);
        const auto farGrants = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kGuest3Conn);
        Check(farRejects.size() == 1 && farGrants.empty() && farRejects[0].OwnershipEpoch == hostEpoch,
              "out-of-range RequestOwnershipClaim is rejected with NotifyRelinquishControl at current epoch");

        Harness::ClearOutbox();
        RequestOwnershipClaim ownerSelfClaim{};
        ownerSelfClaim.ServerId = npcServerId;
        ownerSelfClaim.NewActorData = farClaim.NewActorData;
        Check(DeliverWire(world, ownerSelfClaim, pHost), "owner kAlreadyOwner RequestOwnershipClaim round-trips");
        const auto selfClaimGrants = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kHostConn);
        Check(selfClaimGrants.empty(),
              "kAlreadyOwner RequestOwnershipClaim never echoes NotifyOwnershipTransfer (zero claim/grant loop)");

        // 5. Self-healing WorldNpcLeasePolicy expiry:
        //    - Unpaused idle keepalives renew Host's lease without churning epoch.
        //    - Solo menu pause (no active peer in range) NEVER revokes Host's lease.
        //    - Co-op menu pause while Guest2 is actively simulating hands off cleanly on the wire!
        pGuest2->SetCellComponent(CellIdComponent{kFarCell});
        const uint64_t baseMs = 5'000'000'000ull;
        world.GetCharacterService().NotePlayerSimulationActivity(pHost, baseMs);

        ClientReferencesMoveRequest idleKeepalive{};
        idleKeepalive.Tick = 100u;
        Check(DeliverWire(world, idleKeepalive, pHost), "5-byte empty unpaused keepalive round-trips over the wire");

        // Re-anchor deterministic timestamps for exact lease boundary checks:
        world.GetCharacterService().NotePlayerSimulationActivity(pHost, baseMs + 2000ull);
        Harness::ClearOutbox();
        world.GetCharacterService().SweepExpiredWorldNpcLeases(baseMs + 15000ull); // > 8000ms, Guest2/Guest3 out of range
        Check(DecodeOutboxMessagesFor<NotifyRelinquishControl>(kHostConn).empty(),
              "solo pause menu (>8s without keepalives, no active peer in range) never revokes world NPC lease");

        // Host closes solo pause menu at baseMs + 16000 -> RefreshUnrevoked restores lease without epoch bump.
        world.GetCharacterService().NotePlayerSimulationActivity(pHost, baseMs + 16000ull);
        Check(world.get<OwnerComponent>(static_cast<entt::entity>(npcServerId)).GetOwner() == pHost,
              "resuming simulation after solo pause keeps Host as owner without churn");

        // Now Guest2 enters the cell and actively simulates while Host pauses in StatsMenu for >8s!
        pGuest2->SetCellComponent(CellIdComponent{kDungeonCell});
        pGuest3->SetCellComponent(CellIdComponent{kDungeonCell});
        world.GetCharacterService().NotePlayerSimulationActivity(pGuest2, baseMs + 24500ull);
        world.GetCharacterService().NotePlayerSimulationActivity(pGuest3, baseMs + 24500ull);
        Harness::ClearOutbox();
        world.GetCharacterService().SweepExpiredWorldNpcLeases(baseMs + 25000ull); // Host last active at +16000 (9s ago!)

        const auto hostExpiredRelinquish = DecodeOutboxMessagesFor<NotifyRelinquishControl>(kHostConn);
        const auto g2ExpiredGrant = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kGuest2Conn);
        Check(hostExpiredRelinquish.size() == 1 && g2ExpiredGrant.size() == 1 &&
                  g2ExpiredGrant[0].ServerId == npcServerId &&
                  g2ExpiredGrant[0].OwnershipEpoch == hostExpiredRelinquish[0].OwnershipEpoch &&
                  g2ExpiredGrant[0].OwnershipEpoch > hostEpoch &&
                  g2ExpiredGrant[0].HealthRevision >= authHealthRev,
              "co-op menu pause (>8s) hands expired world NPC lease to active peer with matching epoch & HealthRevision");

        const uint64_t g2HandedEpoch = g2ExpiredGrant.empty() ? 0ull : g2ExpiredGrant[0].OwnershipEpoch;

        // 6. 3-peer cascade DeclineList: Guest2 declines (streaming cell -> Position {0,0,0});
        //    Host is already in DeclineList and Guest2 is added -> server must pick Guest3 (never bounce to Host!).
        Harness::ClearOutbox();
        RequestOwnershipTransfer g2Decline{};
        g2Decline.ServerId = npcServerId;
        g2Decline.OwnershipEpoch = g2HandedEpoch;
        Check(DeliverWire(world, g2Decline, pGuest2), "Guest2 DeclineGrant RequestOwnershipTransfer round-trips");

        const auto g3CascadeGrant = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kGuest3Conn);
        const auto hostCascadeGrant = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kHostConn);
        Check(g3CascadeGrant.size() == 1 && hostCascadeGrant.empty() &&
                  g3CascadeGrant[0].OwnershipEpoch > g2HandedEpoch,
              "3-peer cascade decline skips both declined peers and grants Guest3 without ping-pong");

        const uint32_t revBeforeKill = g3CascadeGrant.empty() ? authHealthRev : g3CascadeGrant[0].HealthRevision;

        // 7. Wire NotifyDeathStateChange carries HealthRevision and blocks stale pre-death snapshots.
        Harness::ClearOutbox();
        RequestDeathStateChange g2Kill{};
        g2Kill.Id = npcServerId;
        g2Kill.IsDead = true;
        Check(DeliverWire(world, g2Kill, pGuest2), "RequestDeathStateChange round-trips over the wire");

        const auto g3DeathNotifies = DecodeOutboxMessagesFor<NotifyDeathStateChange>(kGuest3Conn);
        const auto g2KillRevAcks = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuest2Conn);
        Check(g3DeathNotifies.size() == 1 && g3DeathNotifies[0].IsDead &&
                  g3DeathNotifies[0].HealthRevision > revBeforeKill &&
                  g2KillRevAcks.size() == 1 &&
                  g2KillRevAcks[0].HealthRevision == g3DeathNotifies[0].HealthRevision,
              "NotifyDeathStateChange delivers bumped HealthRevision to peers and acks killer on the wire");

        const uint32_t deadWireRev = g3DeathNotifies.empty() ? 0u : g3DeathNotifies[0].HealthRevision;

        // Guest3 (current owner) sends an in-flight pre-death health reading echoing revBeforeKill -> refused & corrected!
        Harness::ClearOutbox();
        RequestActorValueChanges staleOwnerSnap{};
        staleOwnerSnap.Id = npcServerId;
        staleOwnerSnap.HealthRevision = revBeforeKill;
        staleOwnerSnap.Values[24] = 225.0f;
        Check(DeliverWire(world, staleOwnerSnap, pGuest3), "stale owner snapshot round-trips over the wire");

        const auto g3Corrections = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuest3Conn);
        Check(g3Corrections.size() == 1 &&
                  HasApproxValue(g3Corrections[0].Values, 24u, 0.0f) &&
                  g3Corrections[0].HealthRevision == deadWireRev,
              "stale pre-death snapshot is refused and answered on the wire with HP=0 and dead HealthRevision");

        std::filesystem::remove(std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin", removeEc);
    }

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
