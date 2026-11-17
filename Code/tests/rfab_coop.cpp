#include <Coop/Encounter.h>
#include <Coop/GenerationQueue.h>
#include <Coop/ResetParticipant.h>
#include <Coop/Readiness.h>
#include <Coop/ScopedValue.h>
#include <TiltedCore/ViewBuffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <Messages/RequestCoopAction.h>
#include <Messages/NotifyCoopAction.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <catch2/catch.hpp>
#include <cstring>
#include <limits>

namespace
{
using namespace Coop;
Presence At(float x, uint32_t flags = Grounded | Stable | CanInteract, uint32_t revision = 0)
{
    Presence p;
    p.Location.Cell = GameId(0, 0x123);
    p.Location.X = x;
    p.Flags = flags;
    p.DownedRevision = revision;
    return p;
}
struct EncounterFixture
{
    EncounterRuntime R;
    EncounterFixture()
    {
        REQUIRE(R.Join(1)); REQUIRE(R.Join(2));
        R.Observe(1, At(0), 1000); R.Observe(2, At(96), 1000);
        R.Observe(1, At(0), 6000); R.Observe(2, At(96), 6000);
        REQUIRE(R.RegisterAnchor(1, Anchor{At(0).Location, 5000}, 6000));
        REQUIRE(R.RegisterAnchor(2, Anchor{At(96).Location, 5000}, 6000));
        R.Observe(1, At(0, InCombat | CanInteract), 7000);
        R.Observe(2, At(96, InCombat | Bleedout), 7000);
        REQUIRE(R.Members.at(2).Downed);
        R.Drain();
    }
    void Observe(uint64_t now, bool bothDown = false)
    {
        R.Observe(1, At(0, InCombat | (bothDown ? Bleedout : CanInteract), R.Members.at(1).Revision), now);
        R.Observe(2, At(96, InCombat | Bleedout, R.Members.at(2).Revision), now);
    }
    void Wipe()
    {
        for (uint64_t now = 8000; now <= 13000; now += 250)
        {
            Observe(now, true);
            R.Tick(now);
        }
    }
};
std::vector<uint8_t> Encode(const Packet& p)
{
    TiltedPhoques::Buffer buffer(1 << 20);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    p.Serialize(writer);
    return {buffer.GetWriteData(), buffer.GetWriteData() + writer.Size()};
}
Packet Decode(std::vector<uint8_t> bytes)
{
    TiltedPhoques::ViewBuffer buffer(bytes.data(), bytes.size());
    TiltedPhoques::Buffer::Reader reader(&buffer);
    Packet packet;
    packet.Deserialize(reader);
    return packet;
}
template <class T> std::vector<T> Events(EncounterRuntime& r)
{
    std::vector<T> out;
    for (auto& body : r.Drain())
        if (const auto* event = std::get_if<T>(&body))
            out.push_back(*event);
    return out;
}
}

TEST_CASE("RFAB downed is indefinite and ignores native regeneration/old bleedout reports", "[rfab][encounter]")
{
    EncounterFixture f;
    for (uint64_t now = 8000; now <= 208000; now += 1000)
    {
        f.R.Observe(1, At(0, CanInteract), now);
        f.R.Observe(2, At(96, CanInteract, 1), now); // native standing cannot clear the latch
        f.R.Tick(now);
    }
    CHECK(f.R.Members.at(2).Downed);
    CHECK_FALSE(f.R.Resetting);
    CHECK(Events<ResetPlan>(f.R).empty());
    CHECK_FALSE(f.R.Join(3));
    // A living member in a different cell is still a party member, not a reason for a solo wipe.
    auto away = At(50000); away.Location.Cell = GameId(0, 0x456);
    f.R.Observe(1, away, 209000); f.R.Tick(209000);
    CHECK(f.R.Cohort.size() == 2);
}

TEST_CASE("RFAB revive needs a continuous five seconds and a fresh target revision", "[rfab][encounter]")
{
    EncounterFixture f;
    const Revive hold{2, 1, 1, true};
    CHECK_FALSE(f.R.ReviveHold(1, Revive{2, 0, 1, true}, 7000));
    REQUIRE(f.R.ReviveHold(1, hold, 7000));
    for (uint64_t now = 7250; now < 12000; now += 250)
    {
        f.Observe(now);
        REQUIRE(f.R.ReviveHold(1, hold, now));
        f.R.Tick(now);
        CHECK(f.R.Members.at(2).Downed);
    }
    f.Observe(12000);
    REQUIRE(f.R.ReviveHold(1, hold, 12000));
    f.R.Tick(12000);
    CHECK_FALSE(f.R.Members.at(2).Downed);
    CHECK(f.R.Members.at(2).Revision == 2);
    f.R.Observe(2, At(96, Bleedout, 1), 12200);
    CHECK_FALSE(f.R.Members.at(2).Downed); // old engine sample racing the get-up cannot re-down it
    f.R.Observe(2, At(96, Bleedout, 2), 12400);
    CHECK(f.R.Members.at(2).Downed);
    CHECK(f.R.Members.at(2).Revision == 3);
}

TEST_CASE("RFAB revive cancels on movement, hit, loading, release, timeout and disconnect", "[rfab][encounter][fault]")
{
    for (int reason = 0; reason < 6; ++reason)
    {
        INFO("cancellation reason " << reason);
        EncounterFixture f;
        REQUIRE(f.R.ReviveHold(1, Revive{2, 1, 9, true}, 7000));
        switch (reason)
        {
        case 0: f.R.Observe(1, At(-65, CanInteract), 7250); break;
        case 1: { auto hit = At(0, CanInteract); hit.HitSerial = 1; f.R.Observe(1, hit, 7250); break; }
        case 2: { Presence loading; loading.Flags = Loading; f.R.Observe(2, loading, 7250); break; }
        case 3: f.R.ReviveHold(1, Revive{2, 1, 9, false}, 7250); break;
        case 4: f.Observe(8000); break; // no hold heartbeat for >750ms
        case 5: f.R.Disconnect(1); break;
        }
        f.R.Tick(reason == 4 ? 8000 : 7250);
        CHECK(f.R.Holds.empty());
        CHECK(f.R.Members.at(2).Downed);
        f.R.Join(1); f.Observe(8250);
        CHECK_FALSE(f.R.ReviveHold(1, Revive{2, 1, 9, true}, 8250));
    }
}

TEST_CASE("RFAB anchors require stationary static ground and freeze across exterior cell movement", "[rfab][encounter][anchor]")
{
    EncounterRuntime r;
    REQUIRE(r.Join(1));
    auto outdoor = At(0); outdoor.Location.World = GameId(0, 0x3c);
    r.Observe(1, outdoor, 1000);
    CHECK_FALSE(r.RegisterAnchor(1, {outdoor.Location, 5000}, 5999));
    r.Observe(1, outdoor, 6000);
    REQUIRE(r.RegisterAnchor(1, {outdoor.Location, 5000}, 6000));
    outdoor.Flags |= InCombat;
    r.Observe(1, outdoor, 7000);
    outdoor.Location.Cell = GameId(0, 0x789); outdoor.Location.X = 4500;
    r.Observe(1, outdoor, 8000);
    CHECK_FALSE(r.RegisterAnchor(1, {outdoor.Location, 5000}, 14000));
    REQUIRE(r.Members.at(1).Frozen);
    CHECK(r.Members.at(1).Frozen->X == 0);
    CHECK(r.Members.at(1).Frozen->Cell == GameId(0, 0x123));
    EncounterRuntime moving; REQUIRE(moving.Join(1));
    moving.Observe(1, At(0), 1000); moving.Observe(1, At(64), 6000);
    CHECK_FALSE(moving.RegisterAnchor(1, {At(64).Location, 5000}, 6000));
}

TEST_CASE("RFAB group wipe waits five seconds then requires unanimous prepare and commit receipts", "[rfab][encounter][reset]")
{
    EncounterFixture f;
    for (uint64_t now = 8000; now < 13000; now += 250)
    {
        f.Observe(now, true); f.R.Tick(now);
    }
    CHECK_FALSE(f.R.Resetting);
    f.Observe(13000, true); f.R.Tick(13000);
    REQUIRE(f.R.Resetting);
    const auto prepare = Events<ResetPlan>(f.R);
    REQUIRE(prepare.size() == 1);
    REQUIRE(prepare[0].Phase == ResetPhase::Prepare);
    CHECK(prepare[0].Actors.size() == 2);
    const auto id = f.R.Id;
    CHECK_FALSE(f.R.ResetAck(3, {id, ResetPhase::Prepare, true}, 13000));
    CHECK_FALSE(f.R.ResetAck(1, {id + 1, ResetPhase::Prepare, true}, 13000));
    REQUIRE(f.R.ResetAck(1, {id, ResetPhase::Prepare, true}, 13001));
    CHECK(Events<ResetPlan>(f.R).empty());
    REQUIRE(f.R.ResetAck(2, {id, ResetPhase::Prepare, true}, 13002));
    const auto commit = Events<ResetPlan>(f.R);
    REQUIRE(commit.size() == 1); CHECK(commit[0].Phase == ResetPhase::Commit);
    CHECK(f.R.Members.at(1).Downed); // prepare is not a revival
    REQUIRE(f.R.ResetAck(2, {id, ResetPhase::Commit, true}, 14000));
    CHECK(f.R.Resetting);
    REQUIRE(f.R.ResetAck(1, {id, ResetPhase::Commit, true}, 14001));
    CHECK_FALSE(f.R.Active); CHECK_FALSE(f.R.Resetting);
    CHECK_FALSE(f.R.Members.at(1).Downed); CHECK_FALSE(f.R.Members.at(2).Downed);
    const auto done = Events<ResetPlan>(f.R);
    REQUIRE(done.size() == 1); CHECK(done[0].Phase == ResetPhase::Complete);
}

TEST_CASE("RFAB refuses reset without anchors and blocks partial or timed-out recovery", "[rfab][encounter][fault]")
{
    SECTION("missing anchor")
    {
        EncounterFixture f; f.R.Members.at(1).Frozen.reset(); f.Wipe();
        CHECK(f.R.Blocked); CHECK_FALSE(f.R.Resetting);
        CHECK(Events<ResetPlan>(f.R).empty());
    }
    SECTION("overlapping anchors")
    {
        EncounterFixture f; f.R.Members.at(2).Frozen = f.R.Members.at(1).Frozen; f.Wipe();
        CHECK(f.R.Blocked); CHECK_FALSE(f.R.Resetting);
    }
    SECTION("failed receipt")
    {
        EncounterFixture f; f.Wipe();
        CHECK_FALSE(f.R.ResetAck(1, {f.R.Id, ResetPhase::Prepare, false}, 13001));
        CHECK(f.R.Blocked); CHECK(f.R.Members.at(1).Downed);
    }
    SECTION("timeout is not an automatic second teleport")
    {
        EncounterFixture f; f.Wipe(); f.R.Drain(); f.R.CheckTimeout(28000);
        CHECK(f.R.Blocked); CHECK_FALSE(f.R.Resetting);
        CHECK(Events<ResetPlan>(f.R).empty());
    }
    SECTION("disconnect cancels wipe and rebind retains the downed latch")
    {
        EncounterFixture f; f.R.Disconnect(2);
        f.Observe(8000); // a late packet cannot prove the old disconnected peer is ready by itself
        f.R.Rebind(2, 20); REQUIRE(f.R.Join(20));
        CHECK(f.R.Members.at(20).Downed); CHECK(f.R.Cohort.contains(20));
        CHECK_FALSE(f.R.Cohort.contains(2));
    }
}

TEST_CASE("RFAB action codec roundtrips records and rejects every byte truncation", "[rfab][wire]")
{
    std::vector<Body> records;
    records.emplace_back(Hello{1, NativeDowned | GroundedAnchors, "character-token", "se-1.5.97"});
    records.emplace_back(Welcome{NativeDowned, "campaign", ""});
    records.emplace_back(At(10));
    records.emplace_back(Revive{2, 1, 4, true});
    records.emplace_back(DownedState{2, 1, 1, 5000, true, 3, 12, 0.f});
    records.emplace_back(Anchor{At(0).Location, 5000});
    records.emplace_back(EncounterState{3, EncounterPhase::Started, 0, ""});
    records.emplace_back(ResetPlan{3, ResetPhase::Prepare, {{2, 3, 12, 100.f, true, At(0).Location}}});
    records.emplace_back(ResetReceipt{3, ResetPhase::Commit, true});
    records.emplace_back(EnemyContact{4, 2, true, false, false});
    records.emplace_back(EnemyScale{4, 2, 3, 100.f, 200.f, 190.f});
    records.emplace_back(Reward{RewardKind::Kill, 4, 1, 1, GameId(0, 0x50), {}, 100});
    records.emplace_back(RewardGrant{100, 500});
    records.emplace_back(RewardReceipt{100, true});
    Inventory::Entry item; item.BaseId = GameId(0, 0x12eb7); item.Count = 1;
    item.ExtraHealth = 1.25f; item.ExtraEnchantId = GameId(0xffffffff, 1);
    Inventory::EffectItem effect; effect.EffectId = GameId(0, 0x123); effect.Magnitude = 5.f;
    item.EnchantData.Effects.push_back(effect);
    records.emplace_back(StorageOpen{"rfab.esp", 0x123, GameId(1, 0x123), GameId(1, 0x456), true, {item}});
    records.emplace_back(StorageState{"campaign/rfab.esp/123", 10, 4, false, {item}});
    records.emplace_back(Transfer{1, 2, 1, 1, 7, 10, 20, 0, item});
    records.emplace_back(TransferReply{50, 7, TransferResult::Prepared, 2, 1, 2, 1, 1, 11, 21, item, ""});
    records.emplace_back(TransferReceipt{50, true, false});
    records.emplace_back(InventoryQuery{2});
    records.emplace_back(InventoryState{2, 1, 20, false, {item}});
    records.emplace_back(Checkpoint{5, CheckpointPhase::Quiesce, "digest-not-path", true});
    for (const auto& body : records)
    {
        INFO("variant " << body.index());
        Packet p; p.Session = 123; p.Generation = 4; p.Operation = 5; p.Content = body;
        const auto bytes = Encode(p);
        const auto decoded = Decode(bytes);
        REQUIRE(decoded.Valid);
        CHECK(decoded.Session == 123); CHECK(decoded.Generation == 4); CHECK(decoded.Operation == 5);
        CHECK(decoded.Content.index() == body.index());
        CHECK(Encode(decoded) == bytes); // includes every encoded field, not just the discriminator
        for (size_t length = 0; length < bytes.size(); ++length)
        {
            INFO("truncated at " << length << '/' << bytes.size());
            CHECK_FALSE(Decode({bytes.begin(), bytes.begin() + length}).Valid);
        }
    }
}

TEST_CASE("RFAB action codec rejects bounds, NaN, invalid enum and unknown body tags", "[rfab][wire][bounds]")
{
    Packet p;
    p.Content = Hello{1, 1, std::string(161, 'x'), "adapter"};
    CHECK_FALSE(Decode(Encode(p)).Valid);
    p.Content = EncounterState{1, static_cast<EncounterPhase>(255), 0, ""};
    CHECK_FALSE(Decode(Encode(p)).Valid);
    p.Content = Anchor{At(std::numeric_limits<float>::quiet_NaN()).Location, 5000};
    CHECK_FALSE(Decode(Encode(p)).Valid);
    p.Content = ResetPlan{1, ResetPhase::Prepare, std::vector<ResetActor>(4097)};
    CHECK_FALSE(Decode(Encode(p)).Valid);
    p.Content = Hello{};
    auto bytes = Encode(p);
    REQUIRE(bytes.size() > 24); bytes[24] = 255;
    CHECK_FALSE(Decode(bytes).Valid);
    // Reuse a previously decoded Packet: a later truncated input must not retain Valid=true.
    Packet reused; auto full = Encode(p);
    TiltedPhoques::ViewBuffer first(full.data(), full.size()); TiltedPhoques::Buffer::Reader firstReader(&first);
    reused.Deserialize(firstReader); REQUIRE(reused.Valid);
    TiltedPhoques::ViewBuffer empty(full.data(), 0); TiltedPhoques::Buffer::Reader emptyReader(&empty);
    reused.Deserialize(emptyReader); CHECK_FALSE(reused.Valid);
}

TEST_CASE("RFAB main-thread queue is reentrant and invalidates detached stale work", "[rfab][queue]")
{
    Coop::GenerationQueue queue;
    int count{};
    REQUIRE(queue.Post([&] { ++count; REQUIRE(queue.Post([&] { count += 10; })); }));
    REQUIRE(queue.Post([&] { count += 100; }));
    CHECK(queue.Drain() == 2);
    CHECK(count == 101);
    CHECK(queue.Drain() == 1);
    CHECK(count == 111);
    REQUIRE(queue.Post([&] { ++count; queue.Invalidate(); REQUIRE(queue.Post([&] { count += 1000; })); }));
    REQUIRE(queue.Post([&] { FAIL("detached pre-load work must not execute after the generation fence"); }));
    CHECK(queue.Drain() == 1);
    CHECK(count == 112);
    CHECK(queue.Drain() == 1);
    CHECK(count == 1112);
    for (size_t i = 0; i < Coop::GenerationQueue::kMaxPending; ++i)
        REQUIRE(queue.Post([&] { ++count; }));
    CHECK_FALSE(queue.Post([] {}));
    CHECK(queue.ConsumeOverflow());
    CHECK_FALSE(queue.ConsumeOverflow());
    queue.Invalidate();
    CHECK(queue.Drain() == 0);
    CHECK(count == 1112);
}


TEST_CASE("RFAB native reset commands are fenced and require post-native readiness", "[rfab][reset][fault]")
{
    Coop::ResetParticipant client;
    using Decision = Coop::ResetParticipant::Decision;
    ResetPlan prepare{1, ResetPhase::Prepare, {{1, 0, 0, 0.f, true, At(0).Location}, {2, 0, 0, 0.f, true, At(96).Location}}};
    ResetPlan commit = prepare; commit.Phase = ResetPhase::Commit;
    for (auto& actor : commit.Actors) { actor.Epoch = 3; actor.HealthRevision = 5; actor.Health = 100.f; }
    CHECK(client.Accept(commit, 1) == Decision::Reject);
    REQUIRE(client.Accept(prepare, 1) == Decision::Prepare);
    CHECK(client.Accept(prepare, 1) == Decision::Duplicate);
    CHECK(client.Accept(commit, 1) == Decision::Reject); // resolving a destination still needs a prepare receipt
    client.Receipt(true);
    auto changed = commit; changed.Actors[0].Destination.X += 100.f;
    CHECK(client.Accept(changed, 1) == Decision::Reject);
    changed = commit; changed.Actors.pop_back();
    CHECK(client.Accept(changed, 1) == Decision::Reject);
    REQUIRE(client.Accept(commit, 1) == Decision::Commit);
    CHECK(client.Pending);
    CHECK(client.Accept(commit, 1) == Decision::Duplicate); // no repeated native MoveTo
    CHECK_FALSE(client.LastReceipt); // not ready just because a native method was called
    ResetPlan complete{1, ResetPhase::Complete, {}};
    CHECK(client.Accept(complete, 1) == Decision::Reject);
    client.Receipt(true);
    CHECK(client.Accept(complete, 1) == Decision::Complete);
    CHECK(client.Accept(commit, 1) == Decision::Reject);
    prepare.Encounter = 2;
    CHECK(client.Accept(prepare, 1) == Decision::Prepare);
    client.Receipt(false);
    commit.Encounter = 2;
    CHECK(client.Accept(commit, 1) == Decision::Reject);
}


TEST_CASE("Reset readiness cannot acknowledge inside native bleedout grace or across a load", "[rfab][reset][readiness]")
{
    Coop::ReadinessGate gate;
    using Result = Coop::ReadinessGate::Result;
    gate.Begin(100);
    for (uint64_t now = 100; now < 2100; now += 100)
        CHECK(gate.Poll(now, true) == Result::Waiting); // raw standing can be forced before the graph stands
    CHECK(gate.Poll(2100, false) == Result::Waiting); // graph still bleeding out
    for (uint64_t now = 2200; now < 2700; now += 100)
        CHECK(gate.Poll(now, true) == Result::Waiting);
    CHECK(gate.Poll(2700, true) == Result::Ready);
    CHECK(gate.Poll(2800, false) == Result::Ready); // one terminal receipt, not a second operation
    gate.Begin(100, 0);
    CHECK(gate.Poll(100, true) == Result::Waiting);
    CHECK(gate.Poll(600, true) == Result::Waiting); // a stall is not continuous observed readiness
    CHECK(gate.Poll(700, false) == Result::Waiting);
    for (uint64_t now = 800; now < 1300; now += 100)
        CHECK(gate.Poll(now, true) == Result::Waiting);
    CHECK(gate.Poll(1300, true) == Result::Ready);
    gate.Begin(100);
    CHECK(gate.Poll(12100, true) == Result::Failed); // cannot succeed at/after the deadline
    gate.Begin(100);
    CHECK(gate.Poll(99, true) == Result::Failed);
}


TEST_CASE("Native override scopes preserve an outer reentrant TLS value", "[rfab][native][tls]")
{
    bool value = false;
    {
        Coop::ScopedValue outer(value, true);
        CHECK(value);
        { Coop::ScopedValue inner(value, false); CHECK_FALSE(value); }
        CHECK(value);
        { Coop::ScopedValue inner(value, true); CHECK(value); }
        CHECK(value);
    }
    CHECK_FALSE(value);
}
