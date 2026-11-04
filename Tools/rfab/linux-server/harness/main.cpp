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
//      storm, no oscillation).

#include <spdlog/sinks/stdout_color_sinks.h>

#include <ConsoleRegistry.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/PacketEvent.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/RequestHealthChangeBroadcast.h>

#include "TransportSpy.h"

#include <cmath>
#include <cstdio>

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

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
