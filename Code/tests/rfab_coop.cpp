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
    records.emplace_back(Reward{RewardKind::Kill, 4, 1, 1, GameId(0, 0x50), {}, 100, 1});
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

#include <Coop/RewardLedger.h>
#include <Coop/ClientTimeline.h>

namespace
{
struct RewardsFixture
{
    std::optional<RewardLedger::State> Disk;
    bool Writes{true};
    RewardLedger Ledger{"campaign", [this](const auto& s) { if (!Writes) return false; Disk = s; return true; }};
    RewardIdentity Quest(uint64_t slot = 1, uint64_t cycle = 0) const
    { return {RewardKind::Quest, {"Skyrim.esm", 0x12345, false}, {}, cycle, slot}; }
    RewardsFixture() { REQUIRE(Ledger.Attach("alice", 0, 0)); REQUIRE(Ledger.Attach("bob", 0, 0)); }
    void Apply(uint64_t id, const std::string& character, int32_t final)
    {
        const auto& r = *Ledger.Find(id); const auto d = r.Recipients.at(character);
        REQUIRE(Ledger.Receipt(id, character, true, false, final, d.BeforeRevision + 1,
            AdvanceRewardDigest(d.BeforeDigest, id, r.BaseXP)));
    }
};
}
TEST_CASE("Semantic rewards freeze equal bases and participants but keep personal final XP independent", "[rfab][rewards]")
{
    RewardsFixture f;
    const auto first = f.Ledger.Create(f.Quest(), 100, {"alice", "bob"}); REQUIRE(first);
    const auto same = f.Ledger.Create(f.Quest(), 150, {"alice"}); REQUIRE(same);
    CHECK(*same == *first); CHECK(f.Ledger.Find(*first)->BaseXP == 100);
    CHECK(f.Ledger.Find(*first)->Recipients.size() == 2); // neither a personal multiplier nor later membership rewrites the event
    const auto second = f.Ledger.Create(f.Quest(2), 100, {"alice", "bob"}); REQUIRE(second); CHECK(*second != *first);
    const auto repeatable = f.Ledger.Create(f.Quest(1, 1), 100, {"alice", "bob"}); REQUIRE(repeatable); CHECK(*repeatable != *first);
    REQUIRE(f.Ledger.OfferNext("alice", 100)); REQUIRE(f.Ledger.OfferNext("bob", 100));
    CHECK_FALSE(f.Ledger.OfferNext("alice", 101)); // one serial native application, not all pending at once
    f.Apply(*first, "alice", 125); f.Apply(*first, "bob", 200);
    CHECK(f.Ledger.Find(*first)->Recipients.at("alice").FinalXP == 125);
    CHECK(f.Ledger.Find(*first)->Recipients.at("bob").FinalXP == 200);
    const auto digest = AdvanceRewardDigest(0, *first, 100);
    CHECK(f.Ledger.Receipt(*first, "alice", true, false, 125, 1, digest));
    CHECK_FALSE(f.Ledger.Receipt(*first, "alice", true, false, 130, 1, digest));
    CHECK_FALSE(f.Ledger.Receipt(*first, "alice", false, false, 0, 0, 0));
    CHECK(f.Ledger.FindAccount("alice")->Revision == 1);
    CHECK(f.Ledger.FindAccount("alice")->Digest == f.Ledger.FindAccount("bob")->Digest);
    REQUIRE(f.Ledger.OfferNext("alice", 102));
    CHECK(f.Ledger.Find(*second)->Recipients.at("alice").State == RewardLedger::Stage::Offered);
}
TEST_CASE("Kill reference-life and once-per-campaign book keys never use equal amounts or BaseId races", "[rfab][rewards]")
{
    RewardsFixture f;
    RewardIdentity kill{RewardKind::Kill, {"Skyrim.esm", 0x100, false}, {}, 9001, 1};
    const auto id = f.Ledger.Create(kill, 50, {"alice", "bob"}); REQUIRE(id);
    auto other = kill; ++other.Source.Local; REQUIRE(f.Ledger.Create(other, 50, {"alice", "bob"}));
    other = kill; ++other.Incarnation; REQUIRE(f.Ledger.Create(other, 50, {"alice", "bob"}));
    other = kill; other.Source.Plugin = "SKYRIM.ESM";
    CHECK(f.Ledger.Create(other, 50, {"alice", "bob"}) == id);
    CHECK(f.Ledger.Snapshot().Records.size() == 3);
    other.Incarnation = 0; CHECK_FALSE(SemanticRewardKey(other));
    RewardIdentity book{RewardKind::Book, {"books.esl", 0xABC, true}, {}, 0, 1};
    REQUIRE(f.Ledger.Create(book, 200, {"alice", "bob"}));
    CHECK(f.Ledger.Create(book, 600, {"alice"}) == 4); // reader levels cannot turn one book into two group rewards
    book.Occurrence = 2; CHECK_FALSE(f.Ledger.Create(book, 200, {"alice"}));
    book.Occurrence = 1; book.Incarnation = 1; CHECK_FALSE(SemanticRewardKey(book));
    book.Incarnation = 0; book.Context = kill.Source; CHECK_FALSE(SemanticRewardKey(book));
    kill.Source.Local = 0x1000; kill.Source.Light = true; CHECK_FALSE(SemanticRewardKey(kill));
    kill.Source = {"../../unsafe.esp", 1, false}; CHECK_FALSE(f.Ledger.Create(kill, 50, {"alice"}));
}
TEST_CASE("Reward startup and disconnect never guess replay of an offered or applied grant", "[rfab][rewards][fault]")
{
    for (bool restart : {false, true})
    {
        RewardsFixture f;
        const auto id = f.Ledger.Create(f.Quest(), 100, {"alice", "bob"}); REQUIRE(id);
        REQUIRE(f.Ledger.OfferNext("alice", 100));
        if (restart)
        {
            const auto bytes = RewardLedger::Encode(*f.Disk); REQUIRE(bytes);
            const auto state = RewardLedger::Decode(*bytes); REQUIRE(state);
            RewardLedger newProcess("campaign", [](const auto&) { return true; });
            REQUIRE(newProcess.Restore(*state));
            CHECK_FALSE(newProcess.Attach("alice", 0, 0)); CHECK_FALSE(newProcess.OfferNext("alice", 200));
            CHECK(newProcess.Find(*id)->Recipients.at("alice").State == RewardLedger::Stage::Uncertain);
            CHECK(newProcess.Find(*id)->Recipients.at("bob").State == RewardLedger::Stage::Rejected); // never offered: no native uncertainty
            CHECK(newProcess.Attach("bob", 0, 0)); CHECK_FALSE(newProcess.OfferNext("bob", 200));
        }
        else
        {
            REQUIRE(f.Ledger.Disconnect("alice"));
            CHECK_FALSE(f.Ledger.Attach("alice", 0, 0));
            CHECK_FALSE(f.Ledger.Receipt(*id, "alice", true, false, 100, 1, AdvanceRewardDigest(0, *id, 100)));
        }
    }
    RewardsFixture f;
    const auto id = f.Ledger.Create(f.Quest(), 100, {"alice"}); REQUIRE(id);
    REQUIRE(f.Ledger.OfferNext("alice", 100)); REQUIRE(f.Ledger.Expire(15099));
    CHECK_FALSE(f.Ledger.FindAccount("alice")->Blocked);
    REQUIRE(f.Ledger.Expire(15100)); CHECK(f.Ledger.FindAccount("alice")->Blocked);
    CHECK_FALSE(f.Ledger.OfferNext("alice", 15101));
}
TEST_CASE("Reward durability faults and old-save progress cannot produce a memory-only payment", "[rfab][rewards][fault]")
{
    RewardsFixture f;
    const auto id = f.Ledger.Create(f.Quest(), 100, {"alice"}); REQUIRE(id);
    const auto snapshot = RewardLedger::Encode(*f.Disk); REQUIRE(snapshot);
    for (size_t n = 0; n < snapshot->size(); ++n) CHECK_FALSE(RewardLedger::Decode(std::span(snapshot->data(), n)));
    auto trailing = *snapshot; trailing.push_back(0); CHECK_FALSE(RewardLedger::Decode(trailing));
    auto bad = *f.Disk; bad.Accounts.at("alice").Revision = 1; bad.Accounts.at("alice").Digest = 1;
    CHECK_FALSE(RewardLedger::Encode(bad));
    REQUIRE(f.Ledger.OfferNext("alice", 1)); f.Apply(*id, "alice", 0); // a legitimate zero personal multiplier still consumes this event
    CHECK_FALSE(f.Ledger.Attach("alice", 0, 0));
    CHECK(f.Ledger.Attach("alice", 1, AdvanceRewardDigest(0, *id, 100)));
    CHECK_FALSE(f.Ledger.Attach("alice", 1, 7));
    f.Writes = false;
    CHECK_FALSE(f.Ledger.Create(f.Quest(2), 100, {"alice"}));
    CHECK(f.Ledger.Faulted()); CHECK(f.Ledger.Snapshot().Records.size() == 1);
    RewardsFixture offered;
    REQUIRE(offered.Ledger.Create(offered.Quest(), 100, {"alice"})); offered.Writes = false;
    CHECK_FALSE(offered.Ledger.OfferNext("alice", 100)); CHECK(offered.Ledger.Faulted());
    CHECK(offered.Ledger.Find(1)->Recipients.at("alice").State == RewardLedger::Stage::Pending);
}
TEST_CASE("Semantic receipt chain lives with both native XP pools in the character co-save", "[rfab][rewards][timeline]")
{
    std::optional<NativeReceipt> disk;
    ClientTimeline timeline([&](const auto& r) { disk = r; return true; });
    CharacterState state{"00112233445566778899aabbccddeeff", 0, {}};
    REQUIRE(timeline.Load(state, {}));
    const auto id = NativeEffect(EffectKind::Reward, 2, 3, 10);
    SavedRewards reward{"campaign", 1, AdvanceRewardDigest(0, 10, 100), 10, 100, 125};
    CHECK_FALSE(timeline.StampReward(reward)); REQUIRE(timeline.Begin(EffectKind::Reward, id));
    CHECK_FALSE(timeline.StampReward(reward)); REQUIRE(timeline.Applied(id));
    auto bad = reward; bad.Digest ^= 123; CHECK_FALSE(timeline.StampReward(bad));
    REQUIRE(timeline.StampReward(reward)); REQUIRE(timeline.StampReward(reward));
    CHECK_FALSE(timeline.CanSave()); REQUIRE(timeline.Commit(id));
    const auto bytes = timeline.State().Encode(); REQUIRE(bytes);
    const auto decoded = CharacterState::Decode(*bytes); REQUIRE(decoded); CHECK(*decoded == timeline.State());
    ClientTimeline restarted([](const auto&) { return true; }); REQUIRE(restarted.Load(*decoded, disk));
    CHECK_FALSE(restarted.Load(state, disk)); // no old-ESS replay after spending in the newer timeline
    const auto next = NativeEffect(EffectKind::Reward, 2, 3, 11);
    REQUIRE(timeline.Begin(EffectKind::Reward, next)); REQUIRE(timeline.Applied(next));
    bad = reward; bad.Revision = 3; bad.LastReward = 11; CHECK_FALSE(timeline.StampReward(bad));
    bad.Revision = 2; bad.Digest = AdvanceRewardDigest(reward.Digest, 11, 100); REQUIRE(timeline.StampReward(bad));
    REQUIRE(timeline.Commit(next));
    auto duplicate = timeline.State(); duplicate.Rewards.push_back(duplicate.Rewards.front()); CHECK_FALSE(duplicate.Valid());
    for (size_t n = 0; n < bytes->size(); ++n) REQUIRE_FALSE(CharacterState::Decode(std::span(bytes->data(), n)));
}

#include <Coop/RewardMailbox.h>
#include <thread>

namespace
{
Rewards::Completion DummyRewardApply(uint64_t epoch, uint64_t token, int32_t base)
{ return {epoch, token, Rewards::Result::Applied, base, Rewards::Wallet | Rewards::Engine}; }
Rewards::Adapter TestRewardAdapter()
{
    Rewards::Adapter adapter;
    adapter.Signature = 0xAABBCCDD; adapter.Kinds = 15; adapter.Flags = Rewards::kRequiredFlags;
    std::strcpy(adapter.Name, "portable-installed-profile"); adapter.ApplyReward = DummyRewardApply;
    return adapter;
}
Rewards::Source QuestSource(uint64_t epoch, uint64_t slot = 1)
{
    Rewards::Source source;
    source.WorldEpoch = epoch; source.Kind = static_cast<uint32_t>(RewardKind::Quest);
    source.SourceForm = 0x12345; source.BaseXP = 100; source.Occurrence = slot;
    return source;
}
}
TEST_CASE("Native semantic ingress is generation-bound bounded and holds the original path during negotiation", "[rfab][rewards][bridge]")
{
    RewardMailbox mailbox;
    mailbox.NewEpoch(10);
    const auto adapter = TestRewardAdapter(); REQUIRE(mailbox.Register(10, adapter));
    auto source = QuestSource(10);
    CHECK(mailbox.Submit(10, source) == Rewards::Submission::PassThrough); // ordinary offline RFAB remains ordinary RFAB
    mailbox.Configure(10, Rewards::Mode::Negotiating);
    CHECK(mailbox.Submit(10, source) == Rewards::Submission::Queued);
    CHECK(mailbox.Submit(10, source) == Rewards::Submission::Queued); // exact duplicate doesn't consume queue capacity
    CHECK(mailbox.Take(10).empty());
    mailbox.Configure(10, Rewards::Mode::Active);
    const auto events = mailbox.Take(10); REQUIRE(events.size() == 1); CHECK(events[0].Nonce == 1);
    CHECK(events[0].Source.SourceForm == 0x12345);
    mailbox.NewEpoch(11);
    CHECK_FALSE(mailbox.Register(10, adapter));
    CHECK(mailbox.Submit(10, source) == Rewards::Submission::Blocked); // never pass an old event into a new single-player world
    REQUIRE(mailbox.Register(11, adapter));
    mailbox.NewEpoch(11); CHECK(mailbox.Adapter(11)); // an early OnPlayerLoadGame registration survives main-thread adoption
    mailbox.NewEpoch(9); CHECK(mailbox.Adapter(11));
    mailbox.Configure(11, Rewards::Mode::Active);
    for (uint64_t i = 1; i <= RewardMailbox::kCapacity; ++i) CHECK(mailbox.Submit(11, QuestSource(11, i)) == Rewards::Submission::Queued);
    CHECK(mailbox.Submit(11, QuestSource(11, 1000)) == Rewards::Submission::Blocked);
    CHECK(mailbox.ConsumeFault()); CHECK_FALSE(mailbox.ConsumeFault());
    mailbox.Configure(11, Rewards::Mode::Negotiating);
    CHECK(mailbox.Mode() == Rewards::Mode::Blocked); CHECK_FALSE(mailbox.Adapter(11)); // consuming a notification never clears quarantine
}
TEST_CASE("Native dual-pool completion cannot echo reapply switch adapters or release another epoch", "[rfab][rewards][bridge]")
{
    RewardMailbox mailbox; mailbox.NewEpoch(1);
    auto adapter = TestRewardAdapter(); REQUIRE(mailbox.Register(1, adapter));
    mailbox.Configure(1, Rewards::Mode::Active);
    REQUIRE(mailbox.BeginApplication(1, 50)); CHECK_FALSE(mailbox.BeginApplication(1, 51));
    auto echo = QuestSource(1); echo.InboundToken = 50;
    CHECK(mailbox.Submit(1, echo) == Rewards::Submission::PassThrough); CHECK(mailbox.Take(1).empty());
    echo.InboundToken = 51; CHECK(mailbox.Submit(1, echo) == Rewards::Submission::Blocked);
    const auto completion = DummyRewardApply(1, 50, 125);
    CHECK_FALSE(mailbox.Complete(1, {1, 49, Rewards::Result::Applied, 125, 3}));
    REQUIRE(mailbox.Complete(1, completion)); CHECK(mailbox.Complete(1, completion));
    REQUIRE(mailbox.Completion(1, 50)); CHECK_FALSE(mailbox.FinishApplication(1, 51));
    REQUIRE(mailbox.FinishApplication(1, 50)); CHECK_FALSE(mailbox.Complete(1, completion));
    REQUIRE(mailbox.BeginApplication(1, 51));
    CHECK_FALSE(mailbox.Complete(1, {1, 51, Rewards::Result::Applied, 125, Rewards::Wallet}));
    CHECK(mailbox.ConsumeFault()); CHECK_FALSE(mailbox.Adapter(1));
    mailbox.NewEpoch(2); REQUIRE(mailbox.Register(2, adapter)); mailbox.Configure(2, Rewards::Mode::Active);
    REQUIRE(mailbox.BeginApplication(2, 51));
    CHECK_FALSE(mailbox.Complete(1, completion)); CHECK_FALSE(mailbox.FinishApplication(1, 50));
    CHECK(mailbox.HasPending());
    CHECK(mailbox.Complete(2, {2, 51, Rewards::Result::Applied, 200, 3}));
    CHECK_FALSE(mailbox.Complete(2, {2, 51, Rewards::Result::Applied, 205, 3})); // conflicting receipts are not last-writer-wins
    CHECK(mailbox.ConsumeFault());
    mailbox.NewEpoch(3); REQUIRE(mailbox.Register(3, adapter)); mailbox.Configure(3, Rewards::Mode::Active);
    adapter.Signature++;
    CHECK_FALSE(mailbox.Register(3, adapter)); CHECK(mailbox.ConsumeFault());
}
TEST_CASE("Clean semantic reconnect can negotiate but a late disconnected event cannot become solo XP", "[rfab][rewards][bridge]")
{
    RewardMailbox mailbox; mailbox.NewEpoch(1); REQUIRE(mailbox.Register(1, TestRewardAdapter()));
    mailbox.Configure(1, Rewards::Mode::Active); mailbox.Configure(1, Rewards::Mode::Offline);
    CHECK(mailbox.Mode() == Rewards::Mode::Blocked);
    mailbox.Configure(1, Rewards::Mode::Negotiating); CHECK(mailbox.Mode() == Rewards::Mode::Negotiating);
    mailbox.Configure(1, Rewards::Mode::Active); mailbox.Configure(1, Rewards::Mode::Offline);
    CHECK(mailbox.Submit(1, QuestSource(1)) == Rewards::Submission::Blocked);
    mailbox.Configure(1, Rewards::Mode::Negotiating); CHECK(mailbox.Mode() == Rewards::Mode::Blocked);
    CHECK(mailbox.ConsumeFault());
}
TEST_CASE("Reward callbacks race a load only as owned records, never native ECS work", "[rfab][rewards][bridge][race]")
{
    for (uint64_t i = 0; i < 64; ++i)
    {
        RewardMailbox mailbox; mailbox.NewEpoch(1); REQUIRE(mailbox.Register(1, TestRewardAdapter())); mailbox.Configure(1, Rewards::Mode::Active);
        REQUIRE(mailbox.BeginApplication(1, 90));
        std::thread completion([&] { mailbox.Complete(1, {1, 90, Rewards::Result::Applied, 125, 3}); });
        mailbox.NewEpoch(2); completion.join();
        CHECK_FALSE(mailbox.Completion(2, 90)); CHECK_FALSE(mailbox.HasPending());
        REQUIRE(mailbox.Register(2, TestRewardAdapter())); mailbox.Configure(2, Rewards::Mode::Active); REQUIRE(mailbox.BeginApplication(2, 91));
        CHECK_FALSE(mailbox.FinishApplication(1, 90)); CHECK(mailbox.HasPending());
    }
}
TEST_CASE("Personal RFAB rounding is forward-only and never reconstructs a shared base from wallets", "[rfab][rewards]")
{
    CHECK(PersonalRewardXP(103, 1.25) == 125); CHECK(PersonalRewardXP(103, 2.0) == 205);
    CHECK(PersonalRewardXP(100, 0.0) == 0); CHECK(PersonalRewardXP(100, 1.0, 1) == 100);
    CHECK_FALSE(PersonalRewardXP(0, 1.0)); CHECK_FALSE(PersonalRewardXP(100, -1.0));
    CHECK_FALSE(PersonalRewardXP(100, std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(PersonalRewardXP(100, std::numeric_limits<double>::infinity()));
    CHECK_FALSE(PersonalRewardXP(1000000, 1e100)); CHECK_FALSE(PersonalRewardXP(100, 1.0, 0));
}

#include <Coop/EnemyHealth.h>
TEST_CASE("Scaled health inbox cannot repeatedly restore old full-snapshot non-health AVs", "[rfab][health][scale]")
{
    ActorValues full;
    full.Scale = {100, 100.f, 200.f}; full.HealthRevision = 4;
    full.ActorValuesList[24] = 150.f; full.ActorMaxValuesList[24] = 200.f;
    full.ActorValuesList[26] = 100.f; full.ActorMaxValuesList[26] = 150.f;
    const auto projected = ScaleHealthOnly(full); REQUIRE(projected);
    CHECK(projected->ActorValuesList.size() == 1); CHECK(projected->ActorMaxValuesList.size() == 1);
    auto pending = *projected;
    auto reply = *projected; reply.HealthRevision++; reply.ActorValuesList[24] = 140.f;
    REQUIRE(MergeScaleSnapshot(pending, reply)); CHECK(pending.ActorValuesList.at(24) == 140.f);
    CHECK_FALSE(pending.ActorValuesList.contains(26)); CHECK_FALSE(pending.ActorMaxValuesList.contains(26));
    full.ActorValuesList.erase(24); CHECK_FALSE(ScaleHealthOnly(full));
}
TEST_CASE("Papyrus reward tickets have one atomic claim and cannot write again after completion or load", "[rfab][rewards][bridge][race]")
{
    RewardMailbox mailbox; mailbox.NewEpoch(1); REQUIRE(mailbox.Register(1, TestRewardAdapter()));
    mailbox.Configure(1, Rewards::Mode::Active); REQUIRE(mailbox.BeginApplication(1, 99));
    CHECK_FALSE(mailbox.CanApply(1, 99));
    bool one{}, two{};
    std::thread a([&] { one = mailbox.ClaimApplication(1, 99); });
    std::thread b([&] { two = mailbox.ClaimApplication(1, 99); });
    a.join(); b.join(); CHECK(one != two); CHECK(mailbox.CanApply(1, 99));
    CHECK_FALSE(mailbox.ClaimApplication(1, 99)); CHECK_FALSE(mailbox.CanApply(1, 98));
    REQUIRE(mailbox.Complete(1, {1, 99, Rewards::Result::Applied, 125, 3}));
    CHECK_FALSE(mailbox.CanApply(1, 99)); REQUIRE(mailbox.FinishApplication(1, 99));
    REQUIRE(mailbox.BeginApplication(1, 100)); REQUIRE(mailbox.ClaimApplication(1, 100));
    mailbox.NewEpoch(2);
    CHECK_FALSE(mailbox.CanApply(1, 100)); CHECK_FALSE(mailbox.ClaimApplication(1, 100));
}

#include <Coop/PersonalReward.h>
TEST_CASE("Personal RFAB requests carry properties not caller-selected amounts and are taken once", "[rfab][rewards][personal]")
{
    PersonalRewardMailbox mailbox;
    const PersonalRewardRequest first{5, 8, 0x123, 0x124, 5};
    CHECK_FALSE(mailbox.Submit(first)); mailbox.NewEpoch(5);
    REQUIRE(mailbox.Submit(first)); CHECK_FALSE(mailbox.Take(5, 7));
    auto conflict = first; conflict.Quantum = 10; CHECK_FALSE(mailbox.Submit(conflict));
    auto next = first; next.Token++;
    CHECK_FALSE(mailbox.Submit(next)); // unconsumed application cannot be overwritten
    REQUIRE(mailbox.Take(5, 8)); CHECK_FALSE(mailbox.Take(5, 8));
    CHECK(mailbox.Submit(first)); CHECK_FALSE(mailbox.Take(5, 8)); // exact resend never re-enqueues after native work
    REQUIRE(mailbox.Submit(next));
    mailbox.NewEpoch(6); CHECK_FALSE(mailbox.Take(5, 9)); CHECK_FALSE(mailbox.Submit(first));
    mailbox.NewEpoch(5); next.Epoch = 6; REQUIRE(mailbox.Submit(next));
    CHECK_FALSE(mailbox.Take(5, 9)); REQUIRE(mailbox.Take(6, 9));
    next.Token++;
    next.Multiplier = next.Wallet; CHECK_FALSE(mailbox.Submit(next));
    next.Multiplier++; next.Quantum = 0; CHECK_FALSE(mailbox.Submit(next));
    next.Quantum = 1001; CHECK_FALSE(mailbox.Submit(next));
}
TEST_CASE("An audited Papyrus float recipe preserves its own rounding and both independent pool increments", "[rfab][rewards][personal]")
{
    const auto a = PlanPersonalReward(103, 1.25f, 1000.f, 250.5f, 5);
    const auto b = PlanPersonalReward(103, 2.f, 10.f, 0.f, 5);
    REQUIRE(a); REQUIRE(b);
    CHECK(a->Final == 125); CHECK(a->WalletAfter == 1125.f); CHECK(a->EngineAfter == 375.5f);
    CHECK(b->Final == 205); CHECK(b->WalletAfter == 215.f); CHECK(b->EngineAfter == 205.f);
    // Float multiplication rounds 3*(float(5/3)) to 5 BEFORE the Papyrus int cast.
    // Substituting a double/math formula here would silently change RFAB's personal multiplier.
    CHECK(PapyrusPersonalRewardXP(3, 5.f / 3.f, 5) == 5);
    CHECK(PersonalRewardXP(3, 5.f / 3.f, 5) == 0); // the other ABI helper deliberately has different precision
    CHECK(PapyrusPersonalRewardXP(103, 0.f, 5) == 0);
    CHECK_FALSE(PlanPersonalReward(103, 1.25f, 16777216.f, 0.f, 5)); // +125 cannot be stored exactly in this float wallet
    CHECK_FALSE(PlanPersonalReward(103, 1.25f, 0.f, 16777216.f, 5));
    CHECK_FALSE(PlanPersonalReward(103, 1.f, -1.f, 0.f, 5));
    CHECK_FALSE(PlanPersonalReward(103, 1.f, 0.f, std::numeric_limits<float>::quiet_NaN(), 5));
    CHECK_FALSE(PapyrusPersonalRewardXP(0, 1.f, 5)); CHECK_FALSE(PapyrusPersonalRewardXP(100, -1.f, 5));
    CHECK_FALSE(PapyrusPersonalRewardXP(100, std::numeric_limits<float>::infinity(), 5));
    CHECK_FALSE(PapyrusPersonalRewardXP(100, 1.f, 0)); CHECK_FALSE(PapyrusPersonalRewardXP(1000000, 1000.f, 5));
}

#include <Coop/RewardProfile.h>
namespace
{
std::string RewardProfileTable()
{
    const std::string sha(64, 'a');
    return "RFAB_REWARDS_V1\tportable-installed-profile\t00000000aabbccdd\t15\t0\tpapyrus-float-v1\n" +
        sha + "\tscripts/RFABCoopRewards.pex\n" + sha + "\tscripts/ReviewedHandler.pex\n" + sha + "\tSKSE/Plugins/RFAB.dll\n";
}
}
TEST_CASE("Installed Papyrus XP profile requires compiled receiver API and exact bounded identities", "[rfab][rewards][profile]")
{
    const auto profile = RewardProfile::Parse(RewardProfileTable()); REQUIRE(profile);
    CHECK(profile->Name == "portable-installed-profile"); CHECK(profile->Signature == 0xAABBCCDD);
    CHECK(profile->Kinds == 15); CHECK_FALSE(profile->Bizarre); REQUIRE(profile->Files.size() == 3);
    CHECK(profile->Files[1].Path == "scripts/ReviewedHandler.pex"); CHECK(profile->Files[1].SHA256[0] == 0xaa);
    auto adapter = TestRewardAdapter(); adapter.ApplyReward = nullptr; adapter.Receiver = 123;
    CHECK(profile->Matches(adapter));
    adapter.Flags |= Rewards::Bizarre; CHECK_FALSE(profile->Matches(adapter));
    adapter.Flags = Rewards::kRequiredFlags; ++adapter.Signature; CHECK_FALSE(profile->Matches(adapter));
    --adapter.Signature; adapter.Kinds = 7; CHECK_FALSE(profile->Matches(adapter));
    adapter.Kinds = 15; adapter.ApplyReward = DummyRewardApply; CHECK_FALSE(profile->Matches(adapter));
    adapter.ApplyReward = nullptr; std::memset(adapter.Name, 'x', sizeof(adapter.Name)); CHECK_FALSE(profile->Matches(adapter));
    auto windows = RewardProfileTable();
    for (size_t i = 0; i < windows.size(); ++i) if (windows[i] == '\n') { windows.insert(i, 1, '\r'); ++i; }
    CHECK(RewardProfile::Parse(windows));
}
TEST_CASE("Installed XP profile refuses unknown recipe paths duplicate casing and uncompiled API-only tables", "[rfab][rewards][profile]")
{
    const auto replace = [](std::string text, std::string_view from, std::string_view to) {
        const auto at = text.find(from); REQUIRE(at != text.npos); return text.replace(at, from.size(), to);
    };
    const auto good = RewardProfileTable();
    for (const auto& bad : {
        replace(good, "V1\t", "V2\t"), replace(good, "papyrus-float-v1", "guessed-rfab-dll"),
        replace(good, "\t15\t", "\t16\t"), replace(good, "\t15\t", "\t0\t"), replace(good, "\t0\t", "\t2\t"),
        replace(good, "00000000aabbccdd", "0000000000000000"), replace(good, "ReviewedHandler.pex", "../Bad.pex"),
        replace(good, "ReviewedHandler.pex", "x\\bad.pex"), replace(good, "scripts/ReviewedHandler.pex", "/outside.pex"),
        replace(good, "ReviewedHandler.pex", "rfabcooprewards.pex"), replace(good, "RFABCoopRewards.pex", "MissingApi.pex"),
        replace(good, "scripts/ReviewedHandler.pex", "SKSE/Plugins/OnlyDll.dll"), replace(good, "RFAB.dll", "../Outside.dll"),
        replace(good, std::string(64, 'a'), std::string(64, 'g')), good + "\n", std::string(65537, 'x')})
        CHECK_FALSE(RewardProfile::Parse(bad));
    CHECK_FALSE(RewardProfile::Parse({}));
    const auto first = good.find('\n'); CHECK_FALSE(RewardProfile::Parse(good.substr(0, first + 1)));
}
TEST_CASE("Truncated and byte-mutated installed XP tables cannot bypass parser bounds", "[rfab][rewards][profile][fuzz]")
{
    const auto good = RewardProfileTable();
    // Exercise all prefixes and individual byte damage under the same sanitizers as wire code.
    const auto inspect = [](const std::optional<RewardProfile>& profile) {
        if (!profile) return;
        CHECK_FALSE(profile->Name.empty()); CHECK(profile->Signature != 0); CHECK(profile->Kinds <= 15);
        CHECK(profile->Files.size() >= 2); CHECK(profile->Files.size() <= 128);
        for (const auto& file : profile->Files)
        { CHECK(file.Path.find("..") == file.Path.npos); CHECK(file.Path.find('\\') == file.Path.npos); }
    };
    for (size_t i = 0; i <= good.size(); ++i) inspect(RewardProfile::Parse(good.substr(0, i)));
    for (size_t i = 0; i < good.size(); ++i)
        for (unsigned byte : {0u, 9u, 10u, 47u, 127u, 255u})
        { auto damaged = good; damaged[i] = static_cast<char>(byte); inspect(RewardProfile::Parse(damaged)); }
}
