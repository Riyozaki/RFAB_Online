#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <Structs/Inventory.h>
#include <Structs/ActorValues.h>
#include <Coop/Health.h>
#include <catch2/catch.hpp>
#include <limits>
#include <Coop/InventoryLedger.h>
#include <Coop/DurableFile.h>
#include <filesystem>
#include <fstream>
#include <chrono>

TEST_CASE("Health prediction rebases local observations without echoing a remote hit", "[rfab][health]")
{
    Coop::HealthPrediction owner;
    owner.Reset(1, 1, 120.f);
    const auto local = owner.Observe(110.f);
    REQUIRE(local);
    REQUIRE(local->Delta == -10.f);
    // Another origin's -20 was serialized first. Our own -10 is still in flight.
    REQUIRE(owner.Accept(2, 100.f, 0) == 90.f);
    REQUIRE_FALSE(owner.Observe(90.f));
    REQUIRE(owner.Accept(3, 90.f, local->Sequence) == 90.f);
    REQUIRE(owner.Pending.empty());
    // Late older reply acknowledges nothing and cannot overwrite the latest HP.
    REQUIRE(owner.Accept(2, 100.f, 0) == 90.f);
    const auto heal = owner.Observe(100.f);
    REQUIRE(heal);
    REQUIRE(heal->Sequence > local->Sequence);
    REQUIRE(owner.Accept(4, 100.f, heal->Sequence) == 100.f);
    REQUIRE_FALSE(owner.Observe(100.f));
}

TEST_CASE("Health prediction preserves damage occurring between polling and an incoming snapshot", "[rfab][health]")
{
    Coop::HealthPrediction owner;
    owner.Reset(7, 10, 120.f);
    // The shipping receive path observes engine HP before it installs a remote snapshot.
    REQUIRE(owner.Observe(95.f));
    REQUIRE(owner.Accept(11, 110.f, 0) == 85.f);
    REQUIRE(owner.Accept(12, 85.f, 1) == 85.f);
    owner.Reset(8, 12, 85.f);
    REQUIRE(owner.Pending.empty());
    REQUIRE(owner.NextSequence == 1);
    REQUIRE(owner.Epoch == 8);
}

TEST_CASE("Health arithmetic and bounded prediction fail closed on malformed floats", "[rfab][health]")
{
    REQUIRE(Coop::AddHealth(120.f, -10.f) == 110.f);
    REQUIRE(Coop::AddHealth(120.f, 10.f) == 130.f);
    REQUIRE(Coop::AddHealth(10.f, -100.f) == 0.f);
    REQUIRE_FALSE(Coop::AddHealth(100.f, std::numeric_limits<float>::quiet_NaN()));
    REQUIRE_FALSE(Coop::AddHealth(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()));
    Coop::HealthPrediction owner;
    owner.Reset(1, 1, 100.f);
    for (size_t i = 0; i < Coop::HealthPrediction::kMaxPending; ++i)
        REQUIRE(owner.Queue(1.f));
    REQUIRE_FALSE(owner.Queue(1.f));
    REQUIRE_FALSE(owner.Observe(std::numeric_limits<float>::infinity()));
    REQUIRE(owner.Pending.size() == Coop::HealthPrediction::kMaxPending);
}

TEST_CASE("Inventory residual validation is exact and does not clamp a losing take into success", "[rfab][inventory]")
{
    Inventory inventory;
    Inventory::Entry entry{};
    entry.BaseId = GameId(0, 0x12eb7);
    entry.Count = -1;
    REQUIRE_FALSE(inventory.AddOrRemoveEntry(entry));
    REQUIRE(inventory.Entries.empty());
    entry.Count = 1;
    REQUIRE(inventory.AddOrRemoveEntry(entry));
    const auto before = inventory;
    entry.Count = -2;
    REQUIRE_FALSE(inventory.AddOrRemoveEntry(entry));
    REQUIRE(inventory == before);
    entry.Count = -1;
    REQUIRE(inventory.AddOrRemoveEntry(entry));
    REQUIRE_FALSE(inventory.AddOrRemoveEntry(entry));
    REQUIRE(inventory.Entries.empty());
    entry.Count = std::numeric_limits<int32_t>::max();
    REQUIRE(inventory.AddOrRemoveEntry(entry));
    entry.Count = 1;
    REQUIRE_FALSE(inventory.AddOrRemoveEntry(entry));
    entry.ExtraHealth = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(inventory.AddOrRemoveEntry(entry));
}

TEST_CASE("Different enchantment payloads are not the same inventory stack", "[rfab][inventory]")
{
    Inventory inventory;
    Inventory::Entry a{};
    a.BaseId = GameId(0, 0x12eb7);
    a.Count = 1;
    a.ExtraEnchantId = GameId(0xffffffff, 1);
    Inventory::EffectItem effect{};
    effect.EffectId = GameId(0, 0x123);
    effect.Magnitude = 10.f;
    a.EnchantData.Effects.push_back(effect);
    auto b = a;
    b.EnchantData.Effects[0].Magnitude = 20.f;
    REQUIRE_FALSE(a.CanBeMerged(b));
    REQUIRE(inventory.AddOrRemoveEntry(a));
    REQUIRE(inventory.AddOrRemoveEntry(b));
    REQUIRE(inventory.Entries.size() == 2);
    b.Count = -1;
    REQUIRE(inventory.AddOrRemoveEntry(b));
    REQUIRE(inventory.Entries.size() == 1);
    REQUIRE(inventory.Entries[0] == a);
}


TEST_CASE("Control HP snapshots reconcile same-epoch damage without rewinding the sender", "[rfab][health][control]")
{
    Coop::HealthPrediction owner;
    REQUIRE(owner.Adopt(2, 10, 120.f) == 120.f);
    REQUIRE(owner.Observe(100.f));
    REQUIRE(owner.NextSequence == 2);
    // This snapshot already includes our op1, but that op's direct ACK has not arrived yet.
    REQUIRE(owner.Adopt(2, 11, 100.f, 1) == 100.f);
    CHECK(owner.Pending.empty());
    CHECK(owner.NextSequence == 2);
    REQUIRE(owner.Observe(90.f)); // op2, unsent at the capture time of the next snapshot
    REQUIRE(owner.Adopt(2, 12, 80.f, 1) == 70.f);
    CHECK(owner.Pending.size() == 1);
    CHECK(owner.NextSequence == 3);
    CHECK(owner.Adopt(2, 10, 120.f, 0) == 70.f);
    CHECK_FALSE(owner.Adopt(1, 99, 999.f));
    CHECK(owner.Project() == 70.f);
    CHECK(owner.Adopt(3, 12, 80.f) == 80.f);
    CHECK(owner.NextSequence == 1);
    CHECK(owner.Pending.empty());
}

namespace
{
struct LedgerFixture
{
    using Ledger = Coop::InventoryLedger;
    Ledger::State Disk;
    bool WriteOk{true};
    size_t Writes{};
    Ledger L{"test-campaign", [this](const auto& state) { ++Writes; if (!WriteOk) return false; Disk = state; return true; }};
    Inventory::Entry Item;
    LedgerFixture()
    {
        Item.BaseId = GameId(4, 0x123); Item.Count = 1;
        Item.ExtraHealth = 1.25f; Item.ExtraCharge = 17.f;
        Item.ExtraTextDisplayName = "Named blade";
        Item.ExtraOwnerId = GameId(2, 0x456);
        Ledger::Endpoint chest, alice, bob;
        chest.Items.Entries.push_back(Item); alice.Epoch = 7; bob.Epoch = 8;
        REQUIRE(L.Register("chest", chest));
        REQUIRE(L.Register("alice", alice));
        REQUIRE(L.Register("bob", bob));
    }
    Ledger::Request Take(std::string player = "alice", uint64_t nonce = 1)
    {
        Ledger::Request r;
        r.Character = player; r.Source = "chest"; r.Destination = player; r.Nonce = nonce;
        r.SourceRevision = L.Find(r.Source)->Revision; r.DestinationRevision = L.Find(player)->Revision;
        r.SourceEpoch = L.Find(r.Source)->Epoch; r.DestinationEpoch = L.Find(player)->Epoch;
        r.Item = Item;
        return r;
    }
};
}

TEST_CASE("Inventory arbitration reserves before native movement and serializes two takes", "[rfab][inventory][ledger]")
{
    using Status = Coop::InventoryLedger::Status;
    LedgerFixture f;
    const auto request = f.Take();
    const auto other = f.Take("bob");
    const auto prepared = f.L.Prepare(request, 1000);
    REQUIRE(prepared.Id);
    REQUIRE(prepared.State == Status::Prepared);
    CHECK(Coop::ExactCount(f.L.Find("chest")->Items, f.Item) == 1);
    CHECK(Coop::ExactCount(f.L.Find("alice")->Items, f.Item) == 0);
    CHECK(f.Disk.Records.at(prepared.Id).State == Status::Prepared);
    CHECK(f.L.Locked("chest"));
    CHECK(f.L.Locked("alice"));
    CHECK(f.L.Prepare(other, 1001).State == Status::Conflict);
    CHECK_FALSE(f.L.Observe("alice", {}, 7)); // periodic snapshots cannot erase escrow
    const auto committed = f.L.Receipt(prepared.Id, "alice", true, false);
    REQUIRE(committed.State == Status::Committed);
    CHECK(Coop::ExactCount(f.L.Find("chest")->Items, f.Item) == 0);
    CHECK(Coop::ExactCount(f.L.Find("alice")->Items, f.Item) == 1);
    CHECK(f.L.Find("chest")->Revision == request.SourceRevision + 1);
    CHECK_FALSE(f.L.Locked("chest"));
    const auto writes = f.Writes;
    CHECK(f.L.Prepare(request, 2000).State == Status::Committed);
    CHECK(f.L.Receipt(prepared.Id, "alice", true, false).State == Status::Committed);
    CHECK(f.Writes == writes);
    auto collision = request; collision.Item.Count = 2;
    CHECK(f.L.Prepare(collision, 2001).State == Status::Conflict);
    CHECK(f.L.Prepare(other, 2002).State == Status::Conflict); // stale source revision, never partial success
    CHECK(Coop::ExactCount(f.Disk.Endpoints.at("alice").Items, f.Item) == 1);
}

TEST_CASE("Inventory native receipt timeout and restart never replay an uncertain move", "[rfab][inventory][ledger]")
{
    using Ledger = Coop::InventoryLedger;
    LedgerFixture f;
    const auto pending = f.L.Prepare(f.Take(), 1000);
    REQUIRE(pending.State == Ledger::Status::Prepared);
    Ledger restarted("test-campaign", [&](const auto& state) { f.Disk = state; return true; });
    REQUIRE(restarted.Restore(f.Disk));
    CHECK(restarted.Find(pending.Id)->State == Ledger::Status::Quarantined);
    CHECK(restarted.Locked("chest"));
    CHECK(restarted.Receipt(pending.Id, "alice", true, false).State == Ledger::Status::Quarantined);
    CHECK(Coop::ExactCount(restarted.Find("chest")->Items, f.Item) == 1);
    CHECK(Coop::ExactCount(restarted.Find("alice")->Items, f.Item) == 0);
    REQUIRE(f.L.Expire(4000));
    CHECK(f.L.Find(pending.Id)->State == Ledger::Status::Quarantined);
    CHECK(f.L.Locked("alice"));
    CHECK(f.L.Receipt(pending.Id, "bob", true, false).State == Ledger::Status::Rejected);
}

TEST_CASE("Inventory aborted-before-native release is different from unknown native success", "[rfab][inventory][ledger]")
{
    using Status = Coop::InventoryLedger::Status;
    LedgerFixture f;
    auto r = f.Take();
    const auto pending = f.L.Prepare(r, 1000);
    REQUIRE(f.L.Receipt(pending.Id, "alice", false, false).State == Status::Rejected);
    CHECK_FALSE(f.L.Locked("chest"));
    CHECK(f.L.Prepare(r, 1001).State == Status::Rejected);
    r.Nonce++;
    const auto second = f.L.Prepare(r, 1002);
    REQUIRE(second.State == Status::Prepared);
    CHECK(f.L.Receipt(second.Id, "alice", false, true).State == Status::Quarantined);
    CHECK(f.L.Locked("chest"));
}

TEST_CASE("Inventory I/O failure cannot publish a fake committed transfer", "[rfab][inventory][ledger]")
{
    using Status = Coop::InventoryLedger::Status;
    LedgerFixture f;
    const auto pending = f.L.Prepare(f.Take(), 1000);
    REQUIRE(pending.State == Status::Prepared);
    f.WriteOk = false;
    CHECK(f.L.Receipt(pending.Id, "alice", true, false).State == Status::Quarantined);
    CHECK(f.L.Faulted());
    CHECK(f.Disk.Records.at(pending.Id).State == Status::Prepared);
    CHECK(Coop::ExactCount(f.L.Find("chest")->Items, f.Item) == 1);
    CHECK(f.L.Prepare(f.Take("bob"), 1001).State == Status::Quarantined);
    CHECK(f.L.Prepare(pending.Action, 1002).State == Status::Quarantined); // cannot replay stale Prepared after durability loss
}

TEST_CASE("Inventory transfer fingerprints include names stolen owner and unique instance data", "[rfab][inventory]")
{
    LedgerFixture f;
    const auto before = f.L.Find("chest")->Items;
    auto r = f.Take();
    r.Item.ExtraTextDisplayName = "Other blade";
    CHECK(f.L.Prepare(r, 1000).State == Coop::InventoryLedger::Status::Rejected);
    r = f.Take(); r.Item.ExtraOwnerId = {};
    CHECK(f.L.Prepare(r, 1001).State == Coop::InventoryLedger::Status::Rejected);
    r = f.Take(); r.Item.HasUnsupportedExtra = true;
    CHECK_FALSE(Coop::Transferable(r.Item));
    r = f.Take(); r.Item.IsQuestItem = true;
    CHECK_FALSE(Coop::Transferable(r.Item));
    r = f.Take(); r.Item.BaseId.ModId = 0xFFFFFFFF;
    CHECK_FALSE(Coop::Transferable(r.Item));
    CHECK(f.L.Find("chest")->Items == before);
}

TEST_CASE("Shared storage uses campaign and backing plugin not load order or access point", "[rfab][inventory][storage]")
{
    const auto a = Coop::StorageKey("campaign", {"RFAB.ESP", 0x123456, false});
    const auto b = Coop::StorageKey("campaign", {"rfab.esp", 0x123456, false});
    REQUIRE(a);
    CHECK(a == b);
    CHECK(a != Coop::StorageKey("other-campaign", {"rfab.esp", 0x123456, false}));
    CHECK(a != Coop::StorageKey("campaign", {"rfab.esp", 0x123457, false}));
    CHECK_FALSE(Coop::StorageKey("../campaign", {"rfab.esp", 1, false}));
    CHECK_FALSE(Coop::StorageKey("campaign", {"../rfab.esp", 1, false}));
    CHECK_FALSE(Coop::StorageKey("campaign", {"rfab.esl", 0x1000, true}));
    Coop::InventoryLedger l("campaign", [](const auto&) { return true; });
    Coop::InventoryLedger::Endpoint storage;
    storage.Storage = storage.NeedsImport = true;
    REQUIRE(l.Register(*a, storage));
    Inventory oldItems;
    Inventory::Entry item; item.BaseId = GameId(1, 100); item.Count = 3;
    oldItems.Entries.push_back(item);
    CHECK_FALSE(l.ImportStorage(*a, oldItems, false));
    REQUIRE(l.ImportStorage(*a, oldItems, true));
    CHECK_FALSE(l.ImportStorage(*a, {}, true)); // a second player's personal chest cannot overwrite the campaign
    CHECK_FALSE(l.Observe(*a, {}, 0));
    CHECK(l.Find(*a)->Items == oldItems);
}

TEST_CASE("Inventory journal reload resolves every nested form by canonical plugin", "[rfab][inventory][persistence]")
{
    using Ledger = Coop::InventoryLedger;
    LedgerFixture f;
    const auto pending = f.L.Prepare(f.Take(), 1000);
    REQUIRE(pending.Id);
    auto name = [](GameId id) -> std::optional<Coop::StableForm> {
        return Coop::StableForm{id.ModId == 4 ? "Items.esp" : "Owners.esm", id.BaseId, false};
    };
    auto resolve = [](const Coop::StableForm& key) -> std::optional<GameId> {
        return GameId(key.Plugin == "items.esp" ? 99 : 77, key.Local);
    };
    const auto bytes = Ledger::Encode(f.L.Snapshot(), name);
    REQUIRE(bytes);
    const auto loaded = Ledger::Decode(*bytes, resolve);
    REQUIRE(loaded);
    const auto& entry = loaded->Endpoints.at("chest").Items.Entries.front();
    CHECK(entry.BaseId == GameId(99, 0x123));
    CHECK(entry.ExtraOwnerId == GameId(77, 0x456));
    CHECK(entry.ExtraTextDisplayName == f.Item.ExtraTextDisplayName);
    CHECK(loaded->Records.at(pending.Id).Action.Item == entry);
    for (size_t length = 0; length < bytes->size(); ++length)
        REQUIRE_FALSE(Ledger::Decode(std::span(bytes->data(), length), resolve));
    auto extra = *bytes; extra.push_back(0);
    CHECK_FALSE(Ledger::Decode(extra, resolve));
    CHECK_FALSE(Ledger::Decode(*bytes, [](const auto&) -> std::optional<GameId> { return {}; }));
}

TEST_CASE("Native transfer participant latches execution before touching Skyrim", "[rfab][inventory][receipts]")
{
    using P = Coop::TransferParticipant;
    P p;
    REQUIRE(p.Prepared(1) == P::Decision::Execute);
    CHECK(p.Prepared(1) == P::Decision::Quarantine); // reentry while executing, NOT a second call
    p.Applied(1, true, false);
    CHECK(p.Prepared(1) == P::Decision::ResendApplied);
    p.Applied(1, false, false); // an old failure cannot undo a positive native receipt
    CHECK(p.Prepared(1) == P::Decision::ResendApplied);
    p.Committed(1);
    CHECK(p.Prepared(1) == P::Decision::Ignore);
    REQUIRE(p.Prepared(2) == P::Decision::Execute);
    p.Applied(2, false, true);
    CHECK(p.Prepared(2) == P::Decision::Quarantine);
    REQUIRE(p.Prepared(3) == P::Decision::Execute);
    p.Applied(3, false, false);
    CHECK(p.Prepared(3) == P::Decision::ResendRejected);
}

TEST_CASE("Atomic journal keeps old snapshot until flushed replacement and rejects corruption", "[rfab][persistence]")
{
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec) / ("rfab-ledger-" + std::to_string(seed));
    REQUIRE_FALSE(ec);
    struct Cleanup { std::filesystem::path Path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(Path, error); } } cleanup{root};
    Coop::DurableFile file(root / "Кампания.bin");
    std::vector<uint8_t> bytes;
    CHECK(file.Read(bytes) == Coop::DurableFile::ReadResult::Missing);
    const std::vector<uint8_t> a{1,2,3}, b{4,5,6,7};
    REQUIRE(file.Write(a));
    for (const auto step : {Coop::DurableFile::Step::BeforeWrite, Coop::DurableFile::Step::AfterWrite, Coop::DurableFile::Step::AfterFlush})
    {
        REQUIRE_FALSE(file.Write(b, [step](auto at) { return at == step; }));
        REQUIRE(file.Read(bytes) == Coop::DurableFile::ReadResult::Loaded);
        CHECK(bytes == a);
    }
    // Failure after rename has an unknown commit outcome to the caller. The next process reads
    // the valid replacement; neither process guesses that the previous state was retained.
    REQUIRE_FALSE(file.Write(b, [](auto step) { return step == Coop::DurableFile::Step::AfterReplace; }));
    REQUIRE(file.Read(bytes) == Coop::DurableFile::ReadResult::Loaded);
    CHECK(bytes == b);
    { std::fstream corrupt(root / "Кампания.bin", std::ios::in | std::ios::out | std::ios::binary); corrupt.seekp(16); corrupt.put('x'); }
    CHECK(file.Read(bytes) == Coop::DurableFile::ReadResult::Failed);
    CHECK(bytes.empty());
}

TEST_CASE("Every truncated inventory is invalid rather than an accepted partial snapshot", "[rfab][wire][inventory]")
{
    LedgerFixture f;
    Inventory inv = f.L.Find("chest")->Items;
    inv.Revision = 12;
    TiltedPhoques::Buffer buffer(4096);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    inv.Serialize(writer);
    for (size_t length = 0; length < writer.Size(); ++length)
    {
        TiltedPhoques::ViewBuffer truncated(buffer.GetWriteData(), length);
        TiltedPhoques::Buffer::Reader reader(&truncated);
        Inventory value; value.Deserialize(reader);
        REQUIRE_FALSE(value.IsValid());
    }
    TiltedPhoques::ViewBuffer encoded(buffer.GetWriteData(), writer.Size());
    TiltedPhoques::Buffer::Reader reader(&encoded);
    Inventory value; value.Deserialize(reader);
    CHECK(value.IsValid());
    CHECK(value.Revision == 12);
    CHECK(value == inv);
}


TEST_CASE("Inventory zero-valued extras retain presence across every codec", "[rfab][inventory][identity]")
{
    Inventory::Entry empty, depleted;
    empty.BaseId = GameId(0, 0x123); empty.Count = 1;
    depleted = empty; depleted.ExtraPresence = Inventory::Entry::Charge | Inventory::Entry::Soul;
    CHECK_FALSE(empty.CanBeMerged(depleted));
    CHECK(Coop::Transferable(depleted));
    Inventory bag; bag.Entries.push_back(depleted);
    TiltedPhoques::Buffer buffer(4096); TiltedPhoques::Buffer::Writer writer(&buffer);
    bag.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buffer);
    Inventory decoded; decoded.Deserialize(reader);
    REQUIRE(decoded.IsValid());
    CHECK(decoded.Entries == bag.Entries);
    CHECK(Coop::SameInventoryItems(decoded, bag));
    auto invalid = depleted; invalid.ExtraPresence |= 0x8000;
    CHECK_FALSE(Coop::Transferable(invalid));
}


TEST_CASE("Health watermark codec rejects duplicate origins overflow and every truncated payload", "[rfab][health][wire]")
{
    ActorValues values;
    values.HealthRevision = 9; values.HealthSequences.emplace(1, 3);
    values.ActorValuesList[24] = 95.f; values.ActorMaxValuesList[24] = 100.f;
    TiltedPhoques::Buffer full(4096); TiltedPhoques::Buffer::Writer writer(&full);
    values.Serialize(writer);
    for (size_t length = 0; length < writer.Size(); ++length)
    {
        TiltedPhoques::ViewBuffer truncated(full.GetWriteData(), length);
        TiltedPhoques::Buffer::Reader reader(&truncated);
        ActorValues decoded; decoded.Deserialize(reader);
        REQUIRE_FALSE(decoded.IsValid());
    }
    auto malformed = [](uint16_t count, bool duplicate) {
        TiltedPhoques::Buffer raw(4096); TiltedPhoques::Buffer::Writer out(&raw);
        out.WriteBits(1, 64); out.WriteBits(count, 16);
        for (uint16_t i = 0; i < count; ++i) { out.WriteBits(duplicate ? 1 : i + 1, 32); out.WriteBits(1, 64); }
        out.WriteBits(0, 16); out.WriteBits(0, 16); out.WriteBits(1, 1);
        TiltedPhoques::Buffer::Reader in(&raw); ActorValues decoded; decoded.Deserialize(in);
        return decoded.IsValid();
    };
    CHECK_FALSE(malformed(33, false));
    CHECK_FALSE(malformed(2, true));
    CHECK(malformed(32, false));
    values.HealthSequences[0] = 5;
    CHECK_FALSE(values.IsValid());
}

#include <Coop/ClientTimeline.h>
#include <Coop/SKSEBridge.h>
#include <Coop/DurableLease.h>

namespace
{
struct TimelineFixture
{
    Coop::CharacterState Saved{"00112233445566778899aabbccddeeff", 0, {}};
    std::optional<Coop::NativeReceipt> Disk;
    bool Writes{true};
    Coop::ClientTimeline Timeline{[this](const auto& next) { if (!Writes) return false; Disk = next; return true; }};
    const Coop::EffectId Id{1, 1, 17};
    TimelineFixture() { REQUIRE(Timeline.Load(Saved, {})); }
};
}
TEST_CASE("Save-bound native effects distinguish Prepared Applied and committed ESS cursor", "[rfab][timeline]")
{
    TimelineFixture f;
    REQUIRE(f.Timeline.Begin(Coop::EffectKind::Transfer, f.Id));
    CHECK(f.Disk->Stage == Coop::EffectStage::Prepared);
    CHECK(f.Timeline.State().Cursor == 0);
    CHECK_FALSE(f.Timeline.CanSave());
    CHECK_FALSE(f.Timeline.Begin(Coop::EffectKind::Transfer, f.Id));
    REQUIRE(f.Timeline.Applied(f.Id));
    CHECK(f.Timeline.State().Cursor == 1);
    CHECK_FALSE(f.Timeline.CanSave());
    REQUIRE(f.Timeline.Commit(f.Id));
    CHECK(f.Timeline.CanSave());
    CHECK(f.Timeline.Commit(f.Id));
    CHECK_FALSE(f.Timeline.Begin(Coop::EffectKind::Transfer, f.Id));
    const auto saved = f.Timeline.State();
    Coop::ClientTimeline restart([](const auto&) { return true; });
    REQUIRE(restart.Load(saved, f.Disk));
    CHECK(restart.CanSave());
    CHECK_FALSE(restart.Load(f.Saved, f.Disk)); // same character, older ESS cursor
    CHECK_FALSE(restart.Load(saved, {})); // lost receipt is not an empty bootstrap
}
TEST_CASE("No restart state of a nonterminal receipt reauthorizes native execution", "[rfab][timeline][fault]")
{
    for (auto stage : {Coop::EffectStage::Prepared, Coop::EffectStage::Applied, Coop::EffectStage::Uncertain})
    {
        TimelineFixture f;
        Coop::NativeReceipt receipt{f.Saved.Character, f.Id, Coop::EffectKind::Transfer, stage, 0, 1};
        f.Saved.Cursor = stage == Coop::EffectStage::Prepared ? 0 : 1;
        CHECK_FALSE(f.Timeline.Load(f.Saved, receipt));
        CHECK_FALSE(f.Timeline.Begin(Coop::EffectKind::Transfer, f.Id));
        CHECK_FALSE(f.Timeline.CanSave());
    }
}
TEST_CASE("Native I/O fault latches at every write-ahead stage without fake cursor success", "[rfab][timeline][fault]")
{
    TimelineFixture before;
    before.Writes = false;
    CHECK_FALSE(before.Timeline.Begin(Coop::EffectKind::Transfer, before.Id));
    CHECK_FALSE(before.Timeline.Ready());
    CHECK(before.Timeline.State().Cursor == 0);
    TimelineFixture after;
    REQUIRE(after.Timeline.Begin(Coop::EffectKind::Transfer, after.Id));
    after.Writes = false;
    CHECK_FALSE(after.Timeline.Applied(after.Id));
    CHECK(after.Timeline.State().Cursor == 0);
    CHECK(after.Disk->Stage == Coop::EffectStage::Prepared);
    CHECK_FALSE(after.Timeline.Ready());
    TimelineFixture commit;
    REQUIRE(commit.Timeline.Begin(Coop::EffectKind::Transfer, commit.Id));
    REQUIRE(commit.Timeline.Applied(commit.Id));
    commit.Writes = false;
    CHECK_FALSE(commit.Timeline.Commit(commit.Id));
    CHECK(commit.Disk->Stage == Coop::EffectStage::Applied);
    CHECK_FALSE(commit.Timeline.CanSave());
}
TEST_CASE("Proven pre-native abort releases the cursor but never retries the same logical effect", "[rfab][timeline]")
{
    TimelineFixture f;
    REQUIRE(f.Timeline.Begin(Coop::EffectKind::Transfer, f.Id));
    REQUIRE(f.Timeline.AbortBeforeNative(f.Id));
    CHECK(f.Timeline.CanSave());
    CHECK(f.Timeline.State().Cursor == 0);
    CHECK_FALSE(f.Timeline.Begin(Coop::EffectKind::Transfer, f.Id));
    CHECK_FALSE(f.Timeline.Applied(f.Id));
    CHECK(f.Timeline.Begin(Coop::EffectKind::Transfer, {1,1,18}));
    f.Timeline.Uncertain({1,1,18});
    CHECK_FALSE(f.Timeline.CanSave());
}
TEST_CASE("Co-save identity and HP scale stamps are bounded exact and never load-order session only", "[rfab][timeline][codec]")
{
    TimelineFixture f;
    REQUIRE(f.Timeline.Begin(Coop::EffectKind::Scale, f.Id));
    REQUIRE(f.Timeline.StampScale({0x12345, 1, 9001, 100.f, 200.f}));
    REQUIRE(f.Timeline.Applied(f.Id)); REQUIRE(f.Timeline.Commit(f.Id));
    const auto bytes = f.Timeline.State().Encode(); REQUIRE(bytes);
    REQUIRE(Coop::CharacterState::Decode(*bytes));
    CHECK(*Coop::CharacterState::Decode(*bytes) == f.Timeline.State());
    for (size_t length = 0; length < bytes->size(); ++length)
        REQUIRE_FALSE(Coop::CharacterState::Decode(std::span(bytes->data(), length)));
    const auto receipt = f.Disk->Encode(); REQUIRE(receipt);
    for (size_t length = 0; length < receipt->size(); ++length)
        REQUIRE_FALSE(Coop::NativeReceipt::Decode(std::span(receipt->data(), length)));
    auto invalid = f.Timeline.State(); invalid.Scales.push_back(invalid.Scales.front());
    CHECK_FALSE(invalid.Encode());
    invalid = f.Timeline.State(); invalid.Scales.front().Maximum = std::numeric_limits<float>::infinity();
    CHECK_FALSE(invalid.Encode());
    invalid = f.Timeline.State(); invalid.Character = "../../not-a-save";
    CHECK_FALSE(invalid.Encode());
    CHECK(Coop::SKSE::kRuntime1597 == ((1u << 24) | (5u << 16) | (97u << 4)));
    CHECK(sizeof(Coop::SKSE::Serialization) == 104);
}
TEST_CASE("A campaign journal has at most one live process owner and can be reacquired", "[rfab][durable][lease]")
{
    const auto path = std::filesystem::temp_directory_path() / ("rfab-lease-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        Coop::DurableLease first(path); REQUIRE(first.Acquired());
        Coop::DurableLease other(path); CHECK_FALSE(other.Acquired());
    }
    { Coop::DurableLease again(path); CHECK(again.Acquired()); }
    std::error_code ec; std::filesystem::remove(path, ec);
}

#include <Coop/StorageCatalog.h>
TEST_CASE("Installed storage catalog is canonical bounded tab separated and all-or-nothing", "[rfab][storage][profile]")
{
    Coop::StorageCatalog catalog;
    const std::string rules = "# generated from installed VMAD\r\nRFAB.esp\t000801\t0\tStorage Patch.esp\t000800\t1\r\nrfab.esp\t000802\t0\tstorage patch.esp\t000800\t1\r\n";
    REQUIRE(catalog.Parse(rules)); REQUIRE(catalog.Rules().size() == 2);
    const auto signature = catalog.Signature(); REQUIRE(signature);
    REQUIRE(catalog.Backing({"rfab.ESP", 0x801, false}));
    CHECK(catalog.Backing({"rfab.esp", 0x801, false})->Plugin == "storage patch.esp");
    CHECK(catalog.IsBacking({"Storage Patch.ESP", 0x800, true}));
    CHECK_FALSE(catalog.IsBacking({"Storage Patch.ESP", 0x800, false}));
    Coop::StorageCatalog reordered;
    REQUIRE(reordered.Parse("rfab.esp\t802\t0\tstorage patch.esp\t800\t1\nrfab.esp\t801\t0\tstorage patch.esp\t800\t1"));
    CHECK(reordered.Signature() == signature);
    for (const std::string bad : {"", "rfab.esp 801 0 rfab.esp 800 0", "rfab.esp\t801\t0\trfab.esp\t800\t2",
            "rfab.esp\t801\t0\trfab.esp\t1800\t1", "../rfab.esp\t801\t0\trfab.esp\t800\t0",
            "rfab.esp\t801\t0\trfab.esp\t801\t0", "rfab.esp\t801\t0\trfab.esp\t800\t0\textra"})
    {
        REQUIRE(catalog.Parse(rules));
        REQUIRE_FALSE(catalog.Parse(bad));
        CHECK(catalog.Rules().empty()); CHECK(catalog.Signature() == 0);
    }
    CHECK_FALSE(catalog.Parse(rules + rules));
    CHECK(catalog.Rules().empty());
}
TEST_CASE("Shared bank cache adoption is save-bound campaign-specific and cannot erase old personal contents by inference", "[rfab][storage][timeline]")
{
    TimelineFixture f;
    const auto id = Coop::NativeEffect(Coop::EffectKind::Storage, 1, 1, 17);
    REQUIRE(id.Valid()); CHECK(id != f.Id);
    CHECK_FALSE(Coop::NativeEffect(Coop::EffectKind::Storage, 1, 1, 1ull << 60).Valid());
    CHECK_FALSE(f.Timeline.StampStorage({0x123, "campaign", 1}));
    REQUIRE(f.Timeline.Begin(Coop::EffectKind::Storage, id));
    REQUIRE(f.Timeline.StampStorage({0x123, "campaign", 1}));
    CHECK_FALSE(f.Timeline.StampStorage({0x123, "different", 2}));
    CHECK_FALSE(f.Timeline.StampStorage({0x123, "campaign", 0}));
    REQUIRE(f.Timeline.Applied(id)); REQUIRE(f.Timeline.Commit(id));
    const auto bytes = f.Timeline.State().Encode(); REQUIRE(bytes);
    REQUIRE(Coop::CharacterState::Decode(*bytes));
    CHECK(*Coop::CharacterState::Decode(*bytes) == f.Timeline.State());
    for (size_t n = 0; n < bytes->size(); ++n) REQUIRE_FALSE(Coop::CharacterState::Decode(std::span(bytes->data(), n)));
    Coop::ClientTimeline loaded([](const auto&) { return true; });
    REQUIRE(loaded.Load(f.Timeline.State(), f.Disk));
    CHECK(loaded.State().Storages.front().Campaign == "campaign");
    CHECK_FALSE(loaded.Load(f.Saved, f.Disk));
    auto invalid = f.Timeline.State(); invalid.Storages.push_back(invalid.Storages.front());
    CHECK_FALSE(invalid.Encode());
}

#include <Coop/StorageCache.h>
TEST_CASE("Same-revision bank snapshots never clone back a native paired withdrawal", "[rfab][storage][timeline][race]")
{
    using D = Coop::StorageCacheDecision;
    const Coop::SavedStorage saved{0x123, "campaign", 9};
    CHECK(Coop::StorageCachePolicy(&saved, "campaign", 9, false, false, true, false) == D::Current);
    CHECK(Coop::StorageCachePolicy(&saved, "campaign", 9, false, true, false, true) == D::AwaitNewRevision);
    CHECK(Coop::StorageCachePolicy(&saved, "campaign", 8, false, true, false, true) == D::Blocked);
    CHECK(Coop::StorageCachePolicy(&saved, "other", 10, false, true, false, true) == D::Blocked);
    CHECK(Coop::StorageCachePolicy(&saved, "campaign", 10, false, false, false, false) == D::Reconcile);
    CHECK(Coop::StorageCachePolicy(nullptr, "campaign", 1, true, false, false, true) == D::AwaitImport);
    CHECK(Coop::StorageCachePolicy(nullptr, "campaign", 2, false, false, false, true) == D::Blocked);
    CHECK(Coop::StorageCachePolicy(nullptr, "campaign", 2, false, true, false, false) == D::Blocked);
    CHECK(Coop::StorageCachePolicy(nullptr, "campaign", 2, false, true, false, true) == D::Adopt);
    TimelineFixture f;
    REQUIRE(f.Timeline.Begin(Coop::EffectKind::Transfer, f.Id));
    CHECK_FALSE(f.Timeline.StampStorage(saved));
    REQUIRE(f.Timeline.Applied(f.Id));
    REQUIRE(f.Timeline.StampStorage(saved));
    CHECK_FALSE(f.Timeline.CanSave());
    REQUIRE(f.Timeline.Commit(f.Id));
    CHECK(f.Timeline.State().Storages.front() == saved);
    CHECK_FALSE(f.Timeline.StampStorage({0x123, "campaign", 10}));
}

#include <Coop/SaveMailbox.h>
#include <barrier>
#include <thread>
namespace
{
struct MailboxFixture
{
    Coop::SaveMailbox Mail;
    const Coop::CharacterState State{"00112233445566778899aabbccddeeff", 0, {}};
    const std::shared_ptr<const std::vector<uint8_t>> Bytes = std::make_shared<const std::vector<uint8_t>>(*State.Encode());
    uint64_t Epoch{};
    MailboxFixture()
    {
        Epoch = Mail.BeginLoad(); REQUIRE(Epoch);
        Mail.SubmitRecord(Epoch, {State, true, false, true});
        Mail.CompleteLoad(Epoch, true);
        REQUIRE(Mail.TakeCompleted());
        REQUIRE(Mail.Publish(Epoch, Bytes, true));
        REQUIRE(Mail.SaveAllowed());
    }
};
}
TEST_CASE("A preload cannot be reopened by a stale main-thread snapshot publication", "[rfab][timeline][mailbox][race]")
{
    MailboxFixture f;
    const auto loading = f.Mail.BeginLoad(); REQUIRE(loading != f.Epoch);
    CHECK_FALSE(f.Mail.SaveAllowed());
    CHECK_FALSE(f.Mail.ReadyForEpoch(f.Epoch));
    CHECK_FALSE(f.Mail.BeginSave()); CHECK_FALSE(f.Mail.BeginNative(f.Epoch));
    CHECK_FALSE(f.Mail.Publish(f.Epoch, f.Bytes, true));
    CHECK_FALSE(f.Mail.Publish(loading, f.Bytes, true)); // main thread has not adopted this load
    CHECK(f.Mail.BeginLoad() == 0); // no overlapping outer request
    CHECK(f.Mail.NativePreLoad() == loading); // PreLoad/Revert join, not a different generation
    f.Mail.SubmitRecord(f.Epoch, {f.State, true, false, true}); // slow old decoder is ignored
    f.Mail.CompleteLoad(f.Epoch, true);
    CHECK_FALSE(f.Mail.TakeCompleted());
    f.Mail.SubmitRecord(loading, {f.State, true, false, true});
    f.Mail.CompleteLoad(loading, true);
    const auto completed = f.Mail.TakeCompleted(); REQUIRE(completed);
    REQUIRE(completed->State); CHECK(completed->Epoch == loading); CHECK(completed->FromSave);
    CHECK_FALSE(f.Mail.SaveAllowed()); // taking a callback is NOT receipt adoption
    REQUIRE(f.Mail.Publish(loading, f.Bytes, true));
    CHECK(f.Mail.ReadyForEpoch(loading));
    CHECK(f.Mail.SaveAllowed());
    f.Mail.CompleteLoad(loading, true); // late/duplicate callback cannot create a fresh character
    CHECK_FALSE(f.Mail.TakeCompleted());
}
TEST_CASE("An outer load rejection without SKSE completion stays quarantined and can be retried", "[rfab][timeline][mailbox]")
{
    MailboxFixture f;
    const auto failed = f.Mail.BeginLoad(); REQUIRE(failed);
    f.Mail.RejectLoad(failed);
    const auto completed = f.Mail.TakeCompleted(); REQUIRE(completed);
    CHECK_FALSE(completed->Success); CHECK_FALSE(completed->State); CHECK_FALSE(completed->FromSave);
    CHECK(completed->Epoch == failed);
    CHECK(f.Mail.ConsumeFault()); CHECK_FALSE(f.Mail.ConsumeFault());
    REQUIRE(f.Mail.Publish(failed, f.Bytes, true));
    CHECK_FALSE(f.Mail.SaveAllowed()); CHECK_FALSE(f.Mail.ReadyForEpoch(failed));
    const auto retry = f.Mail.BeginLoad(); REQUIRE(retry); REQUIRE(retry != failed);
    f.Mail.CompleteLoad(failed, true); f.Mail.RejectLoad(failed);
    CHECK_FALSE(f.Mail.TakeCompleted());
    f.Mail.SubmitRecord(retry, {f.State, true, false, true});
    f.Mail.CompleteLoad(retry, true); REQUIRE(f.Mail.TakeCompleted());
    REQUIRE(f.Mail.Publish(retry, f.Bytes, true)); CHECK(f.Mail.SaveAllowed());
}
TEST_CASE("Missing co-save and new game differ from corrupt duplicate or contradictory records", "[rfab][timeline][mailbox]")
{
    for (int mode = 0; mode < 6; ++mode)
    {
        MailboxFixture f;
        const auto epoch = f.Mail.BeginLoad(); REQUIRE(epoch);
        if (mode == 1) f.Mail.SubmitRecord(epoch, {std::nullopt, true, true, false});
        if (mode == 2) f.Mail.SubmitRecord(epoch, {std::nullopt, true, false, true});
        if (mode == 3)
        {
            f.Mail.SubmitRecord(epoch, {f.State, true, false, true});
            f.Mail.SubmitRecord(epoch, {f.State, true, false, true});
        }
        if (mode == 4)
        {
            auto invalid = f.State; invalid.Character = "not-a-character";
            f.Mail.SubmitRecord(epoch, {invalid, true, false, true});
        }
        if (mode == 5) f.Mail.SubmitRecord(epoch, {f.State, true, false, true});
        f.Mail.CompleteLoad(epoch, true);
        if (mode == 5) f.Mail.RejectLoad(epoch); // outer false overrides inner success before adoption
        const auto loaded = f.Mail.TakeCompleted(); REQUIRE(loaded);
        CHECK(loaded->Success == (mode != 5));
        CHECK(loaded->Corrupt == (mode >= 1 && mode <= 4));
        CHECK_FALSE(loaded->FromSave);
        REQUIRE(f.Mail.Publish(epoch, f.Bytes, true));
        CHECK(f.Mail.SaveAllowed() == (mode == 0)); // offline bootstrap allowed; no co-op readiness claimed
    }
    MailboxFixture f;
    f.Mail.CompleteLoad(0, true, true); // real SKSE NewGame, even without an outer load request
    const auto fresh = f.Mail.TakeCompleted(); REQUIRE(fresh);
    CHECK(fresh->Success); CHECK_FALSE(fresh->FromSave); CHECK_FALSE(fresh->State);
    CHECK(fresh->Epoch != f.Epoch);
    CHECK_FALSE(f.Mail.Publish(f.Epoch, f.Bytes, true));
}
TEST_CASE("Native effect reservation and game-save snapshot pin are mutually exclusive", "[rfab][timeline][mailbox][race]")
{
    for (int round = 0; round < 128; ++round)
    {
        MailboxFixture f;
        std::barrier start(3);
        std::optional<uint64_t> native, save;
        std::thread effect([&] { start.arrive_and_wait(); native = f.Mail.BeginNative(f.Epoch); });
        std::thread writer([&] { start.arrive_and_wait(); save = f.Mail.BeginSave(); });
        start.arrive_and_wait(); effect.join(); writer.join();
        REQUIRE(bool(native) != bool(save));
        if (native)
        {
            CHECK(f.Mail.OwnsNative(f.Epoch, *native));
            CHECK_FALSE(f.Mail.BeginNative(f.Epoch)); CHECK_FALSE(f.Mail.BeginSave());
            REQUIRE(f.Mail.Publish(f.Epoch, f.Bytes, false)); // journal Prepared
            f.Mail.EndNative(f.Epoch + 1, *native); // another generation cannot release this pin
            CHECK(f.Mail.OwnsNative(f.Epoch, *native));
            REQUIRE(f.Mail.Publish(f.Epoch, f.Bytes, true)); // final cursor published before release
            CHECK_FALSE(f.Mail.SaveAllowed());
            f.Mail.EndNative(f.Epoch, *native);
        }
        else
        {
            CHECK_FALSE(f.Mail.BeginNative(f.Epoch)); CHECK_FALSE(f.Mail.Publish(f.Epoch, f.Bytes, false));
            CHECK(f.Mail.BeginLoad() == 0);
            REQUIRE(f.Mail.BeginSerialization());
            const auto pinned = f.Mail.SaveSnapshot(); REQUIRE(pinned.Bytes);
            CHECK(pinned.Bytes == f.Bytes);
            f.Mail.EndSerialization(pinned, true);
            CHECK_FALSE(f.Mail.SaveAllowed()); // engine is still in the OUTER save call
            f.Mail.EndSave(*save, true);
        }
        CHECK(f.Mail.SaveAllowed());
    }
}
TEST_CASE("Queued serialization pins the request cursor after the outer bool return", "[rfab][timeline][mailbox][race]")
{
    MailboxFixture f;
    const auto ticket = f.Mail.BeginSave(); REQUIRE(ticket);
    f.Mail.EndSave(*ticket, true);
    CHECK_FALSE(f.Mail.SaveAllowed()); CHECK_FALSE(f.Mail.BeginNative(f.Epoch)); CHECK(f.Mail.BeginLoad() == 0);
    const auto other = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{9,8,7});
    CHECK_FALSE(f.Mail.Publish(f.Epoch, other, true));
    REQUIRE(f.Mail.BeginSerialization());
    const auto before = f.Mail.SaveSnapshot(); REQUIRE(before.Bytes); CHECK(before.Bytes == f.Bytes);
    f.Mail.EndSerialization(before, true);
    CHECK(f.Mail.SaveAllowed());
    REQUIRE(f.Mail.Publish(f.Epoch, other, true));
    const auto next = f.Mail.BeginSave(); REQUIRE(next); REQUIRE(*next != *ticket);
    REQUIRE(f.Mail.BeginSerialization());
    const auto after = f.Mail.SaveSnapshot(); REQUIRE(after.Bytes); CHECK(after.Bytes == other);
    f.Mail.EndSave(*ticket, false); f.Mail.EndSerialization(before, false); // old completions cannot fault/release new save
    CHECK_FALSE(f.Mail.ConsumeFault()); CHECK_FALSE(f.Mail.SaveAllowed());
    f.Mail.EndSerialization(after, true); CHECK_FALSE(f.Mail.SaveAllowed());
    f.Mail.EndSave(*next, true); CHECK(f.Mail.SaveAllowed());
}
TEST_CASE("Serialization errors bypassed save paths and native overlapping loads are not certified", "[rfab][timeline][mailbox]")
{
    MailboxFixture f;
    const auto refused = f.Mail.BeginSave(); REQUIRE(refused);
    f.Mail.EndSave(*refused, false); // rejected before serialization, old snapshot is unchanged
    CHECK(f.Mail.SaveAllowed());
    const auto save = f.Mail.BeginSave(); REQUIRE(save); REQUIRE(f.Mail.BeginSerialization());
    const auto bytes = f.Mail.SaveSnapshot(); REQUIRE(bytes.Bytes);
    f.Mail.EndSerialization(bytes, false); f.Mail.EndSave(*save, true);
    CHECK(f.Mail.ConsumeFault()); CHECK_FALSE(f.Mail.SaveAllowed());
    REQUIRE(f.Mail.Publish(f.Epoch, f.Bytes, true)); CHECK_FALSE(f.Mail.SaveAllowed());
    CHECK_FALSE(f.Mail.BeginNative(f.Epoch));
    MailboxFixture bypass;
    CHECK_FALSE(bypass.Mail.BeginSerialization()); CHECK(bypass.Mail.ConsumeFault());
    CHECK_FALSE(bypass.Mail.SaveAllowed());
    MailboxFixture overlap;
    const auto pinned = overlap.Mail.BeginSave(); REQUIRE(pinned); REQUIRE(overlap.Mail.BeginSerialization());
    const auto old = overlap.Mail.SaveSnapshot(); REQUIRE(old.Bytes);
    const auto epoch = overlap.Mail.NativePreLoad(); REQUIRE(epoch != overlap.Epoch);
    overlap.Mail.SubmitRecord(epoch, {overlap.State, true, false, true});
    overlap.Mail.CompleteLoad(epoch, true);
    const auto loaded = overlap.Mail.TakeCompleted(); REQUIRE(loaded); CHECK(loaded->Corrupt);
    overlap.Mail.EndSerialization(old, true); overlap.Mail.EndSave(*pinned, true);
    REQUIRE(overlap.Mail.Publish(epoch, overlap.Bytes, true)); CHECK_FALSE(overlap.Mail.SaveAllowed());
}
