#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/NotifyWorldItemDespawn.h>
#include <Messages/NotifyWorldItemSnapshot.h>
#include <Messages/NotifyWorldItemSpawn.h>
#include <Messages/RequestWorldItemDrop.h>
#include <Messages/RequestWorldItemPickup.h>
#include <Messages/ServerMessageFactory.h>

#include <common/CoopWorldStabilizer.h>
#include <common/Session/Features.h>
#include <common/Session/Resync.h>
#include <common/Session/WorldItems.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <vector>

// v4.1.0 architecture step 4: server-owned dropped items. See common/Session/WorldItems.h.

namespace
{
using namespace WorldItems;

struct TestPayload
{
    uint32_t BaseId{0};
    bool operator==(const TestPayload&) const = default;
};

using TestRegistry = Registry<TestPayload>;
using TestMirror = ClientMirror<TestPayload>;

constexpr uint64_t kCellA = PackKey(0, 0x1000);
constexpr uint64_t kCellB = PackKey(0, 0x2000);

ItemId MustDrop(TestRegistry& aRegistry, uint32_t aPlayer, uint64_t aCell, int32_t aCount, uint32_t aCookie, uint64_t aNow,
                uint32_t aBase = 0xF)
{
    ItemId id = kNoItem;
    REQUIRE(aRegistry.TryDrop(aPlayer, aCell, aCount, aCookie, TestPayload{aBase}, aNow, id) == DropResult::kAccepted);
    REQUIRE(id != kNoItem);
    return id;
}

Placement Interior(uint64_t aCell)
{
    Placement p{};
    p.CellKey = aCell;
    return p;
}

template <class TMessage> TMessage RoundTripServer(const TMessage& acSent)
{
    TiltedPhoques::Buffer buff(64 * 1024);
    TiltedPhoques::Buffer::Writer writer(&buff);
    acSent.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buff);
    const ServerMessageFactory factory;
    auto pMessage = factory.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == TMessage::Opcode);
    return *static_cast<TMessage*>(pMessage.get());
}

template <class TMessage> TMessage RoundTripClient(const TMessage& acSent)
{
    TiltedPhoques::Buffer buff(64 * 1024);
    TiltedPhoques::Buffer::Writer writer(&buff);
    acSent.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buff);
    const ClientMessageFactory factory;
    auto pMessage = factory.Extract(reader);
    REQUIRE(pMessage);
    REQUIRE(pMessage->GetOpcode() == TMessage::Opcode);
    return *static_cast<TMessage*>(pMessage.get());
}

WorldItemState SampleState(uint32_t aId)
{
    WorldItemState state{};
    state.Id = aId;
    state.Item.BaseId = GameId(1, 0x12EB7);
    state.Item.Count = 7;
    state.Item.ExtraCharge = 250.0f;
    state.Item.ExtraEnchantId = GameId(1, 0xABC);
    state.Item.ExtraEnchantCharge = 40;
    state.CellId = GameId(0, 0x3C);
    state.WorldSpaceId = GameId(0, 0x3C0);
    state.Position = {1024.5f, -2048.25f, 300.125f};
    state.Rotation = {0.0f, 0.0f, 3.1f};
    state.DropperPlayerId = 2;
    state.DropperCookie = 77;
    return state;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Server registry
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 world items: the server registry owns identity, bounds and arbitration", "[rfab][v410][worlditems]")
{
    SECTION("ids start at 1, are unique and every change moves the revision")
    {
        TestRegistry registry;
        CHECK(registry.GetRevision() == 0);
        const ItemId a = MustDrop(registry, 1, kCellA, 3, 11, 0);
        const ItemId b = MustDrop(registry, 1, kCellA, 1, 12, 0);
        CHECK(a == 1);
        CHECK(b == 2);
        CHECK(registry.GetRevision() == 2);
        REQUIRE(registry.Find(a));
        CHECK(registry.Find(a)->Count == 3);
        CHECK(registry.Find(a)->DropperCookie == 11);
        CHECK(registry.CountInCell(kCellA) == 2);
    }

    SECTION("invalid counts are refused without side effects")
    {
        TestRegistry registry;
        ItemId id = 99;
        CHECK(registry.TryDrop(1, kCellA, 0, 1, {}, 0, id) == DropResult::kInvalidCount);
        CHECK(id == kNoItem);
        CHECK(registry.TryDrop(1, kCellA, -5, 1, {}, 0, id) == DropResult::kInvalidCount);
        CHECK(registry.TryDrop(1, kCellA, registry.GetPolicy().MaxStackCount + 1, 1, {}, 0, id) == DropResult::kInvalidCount);
        CHECK(registry.GetRevision() == 0);
        CHECK(registry.Size() == 0);
    }

    SECTION("world and cell bounds refuse, never evict")
    {
        Policy policy{};
        policy.MaxItems = 4;
        policy.MaxItemsPerCell = 2;
        policy.DropBurst = 100;
        TestRegistry registry{policy};
        MustDrop(registry, 1, kCellA, 1, 1, 0);
        MustDrop(registry, 1, kCellA, 1, 2, 0);
        ItemId id = kNoItem;
        CHECK(registry.TryDrop(1, kCellA, 1, 3, {}, 0, id) == DropResult::kCellFull);
        MustDrop(registry, 1, kCellB, 1, 4, 0);
        MustDrop(registry, 1, kCellB, 1, 5, 0);
        CHECK(registry.TryDrop(1, PackKey(0, 0x3000), 1, 6, {}, 0, id) == DropResult::kWorldFull);
        CHECK(registry.Size() == 4);
        CHECK(registry.Find(1));  // nothing was evicted
    }

    SECTION("per-player token bucket: burst, refill, independent players, tokens spent only on acceptance")
    {
        Policy policy{};
        policy.DropBurst = 3;
        policy.DropRefillIntervalMs = 250;
        policy.MaxItemsPerCell = 1;
        TestRegistry registry{policy};

        // A refused drop (full cell) must not cost a token.
        MustDrop(registry, 1, PackKey(0, 1), 1, 1, 0);
        ItemId id = kNoItem;
        for (int i = 0; i < 10; ++i)
            CHECK(registry.TryDrop(1, PackKey(0, 1), 1, 2, {}, 0, id) == DropResult::kCellFull);
        MustDrop(registry, 1, PackKey(0, 2), 1, 3, 0);
        MustDrop(registry, 1, PackKey(0, 3), 1, 4, 0);
        CHECK(registry.TryDrop(1, PackKey(0, 4), 1, 5, {}, 0, id) == DropResult::kRateLimited);
        // Another player has their own bucket.
        MustDrop(registry, 2, PackKey(0, 5), 1, 6, 0);
        // One interval later exactly one token is back.
        MustDrop(registry, 1, PackKey(0, 6), 1, 7, 250);
        CHECK(registry.TryDrop(1, PackKey(0, 7), 1, 8, {}, 0, id) == DropResult::kRateLimited);
        // A long pause refills up to the burst, not beyond.
        for (uint32_t i = 0; i < 3; ++i)
            MustDrop(registry, 1, PackKey(0, 10 + i), 1, 9, 100'000);
        CHECK(registry.TryDrop(1, PackKey(0, 20), 1, 10, {}, 100'000, id) == DropResult::kRateLimited);
        // A clock going backwards never refills.
        CHECK(registry.TryDrop(1, PackKey(0, 21), 1, 11, {}, 50, id) == DropResult::kRateLimited);
        // Leaving forgets the bucket: a reconnect starts full.
        registry.ForgetPlayer(1);
        MustDrop(registry, 1, PackKey(0, 22), 1, 12, 100'000);
    }

    SECTION("claims: first processed wins, partial claims leave the remainder")
    {
        TestRegistry registry;
        const ItemId id = MustDrop(registry, 1, kCellA, 10, 1, 0);
        const uint32_t rev0 = registry.GetRevision();

        auto claim = registry.TryClaim(id, 0);
        CHECK(claim.Result == ClaimResult::kInvalidCount);
        CHECK(registry.GetRevision() == rev0);

        claim = registry.TryClaim(id, 4);
        CHECK(claim.Result == ClaimResult::kGrantedPartial);
        CHECK(claim.Granted == 4);
        CHECK(claim.Remaining == 6);
        CHECK(registry.Find(id)->Count == 6);
        CHECK(registry.GetRevision() == rev0 + 1);

        claim = registry.TryClaim(id, 100);  // more than there is: the rest
        CHECK(claim.Result == ClaimResult::kGrantedAll);
        CHECK(claim.Granted == 6);
        CHECK(claim.Remaining == 0);
        CHECK(registry.Find(id) == nullptr);
        CHECK(registry.CountInCell(kCellA) == 0);

        claim = registry.TryClaim(id, 1);  // the loser of a race
        CHECK(claim.Result == ClaimResult::kUnknownItem);
        CHECK(claim.Granted == 0);
        CHECK(registry.GetRevision() == rev0 + 2);
    }

    SECTION("ids are never reused, even after removal")
    {
        TestRegistry registry;
        std::set<ItemId> seen;
        for (int i = 0; i < 200; ++i)
        {
            const ItemId id = MustDrop(registry, 1 + (i % 7), PackKey(0, i % 5), 1, 1, static_cast<uint64_t>(i) * 1000);
            CHECK(seen.insert(id).second);
            if (i % 2 == 0)
                CHECK(registry.TryClaim(id, 1).Result == ClaimResult::kGrantedAll);
            else
                CHECK(registry.Remove(id));
        }
        CHECK_FALSE(registry.Remove(12345));
    }

    SECTION("export is sorted; import validates, keeps ids, clears cookies and moves the id counter")
    {
        TestRegistry source;
        for (int i = 0; i < 5; ++i)
            MustDrop(source, 1, kCellA, i + 1, 100 + i, 0);
        auto exported = source.Export();
        REQUIRE(exported.size() == 5);
        CHECK(std::is_sorted(exported.begin(), exported.end(), [](const auto& a, const auto& b) { return a.Id < b.Id; }));

        // Corrupt records a damaged checkpoint might contain.
        auto bad = exported[0];
        bad.Id = kNoItem;
        exported.push_back(bad);
        auto dup = exported[1];
        exported.push_back(dup);
        auto zero = exported[2];
        zero.Id = 900;
        zero.Count = 0;
        exported.push_back(zero);
        std::reverse(exported.begin(), exported.end());

        TestRegistry target;
        const uint32_t rev = target.GetRevision();
        CHECK(target.Import(exported) == 5);
        CHECK(target.GetRevision() != rev);  // the caller always broadcasts the result
        for (const auto& record : target.Export())
            CHECK(record.DropperCookie == 0);
        CHECK(target.CountInCell(kCellA) == 5);
        // A new drop never reuses an imported id.
        const ItemId fresh = MustDrop(target, 1, kCellB, 1, 1, 0);
        CHECK(fresh == 6);

        // Import replaces, it does not merge.
        TestRegistry replaced;
        MustDrop(replaced, 1, kCellA, 1, 1, 0);
        MustDrop(replaced, 1, kCellA, 1, 1, 0);
        CHECK(replaced.Import({}) == 0);
        CHECK(replaced.Size() == 0);
        CHECK(replaced.CountInCell(kCellA) == 0);
        CHECK(MustDrop(replaced, 1, kCellA, 1, 1, 0) == 3);
    }

    SECTION("import honours the bounds of the current policy")
    {
        Policy small{};
        small.MaxItems = 3;
        small.MaxItemsPerCell = 2;
        TestRegistry source;
        for (int i = 0; i < 6; ++i)
            MustDrop(source, 1, i < 3 ? kCellA : kCellB, 1, 1, 0);
        TestRegistry target{small};
        CHECK(target.Import(source.Export()) == 3);
        CHECK(target.CountInCell(kCellA) == 2);
        CHECK(target.CountInCell(kCellB) == 1);
    }

    SECTION("clear moves the revision only when something was removed")
    {
        TestRegistry registry;
        registry.Clear();
        CHECK(registry.GetRevision() == 0);
        MustDrop(registry, 1, kCellA, 1, 1, 0);
        registry.Clear();
        CHECK(registry.GetRevision() == 2);
        CHECK(registry.Size() == 0);
    }
}

TEST_CASE("v4.1.0 world items: revision comparison survives wraparound", "[rfab][v410][worlditems]")
{
    CHECK(IsNewerRevision(1, 0));
    CHECK_FALSE(IsNewerRevision(0, 0));
    CHECK_FALSE(IsNewerRevision(0, 1));
    CHECK(IsNewerRevision(0, 0xFFFFFFFFu));
    CHECK(IsNewerRevision(5, 0xFFFFFFF0u));
    CHECK_FALSE(IsNewerRevision(0xFFFFFFF0u, 5));

    TestMirror mirror;
    REQUIRE(mirror.ApplySnapshot(0xFFFFFFFEu, {}, 1).Vanished.empty());
    CHECK(mirror.AcceptRevision(0xFFFFFFFFu) == RevisionCheck::kInOrder);
    CHECK(mirror.AcceptRevision(0) == RevisionCheck::kInOrder);
    CHECK(mirror.AcceptRevision(1) == RevisionCheck::kInOrder);
    CHECK(mirror.AcceptRevision(0xFFFFFFFFu) == RevisionCheck::kDuplicate);
}

// ---------------------------------------------------------------------------------------------
// Client mirror
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 world items: the client mirror detects gaps and binds references by id", "[rfab][v410][worlditems]")
{
    constexpr uint32_t kMe = 1;
    constexpr uint32_t kPeer = 2;

    SECTION("revision sequence: no baseline is a gap, then in-order / duplicate / gap")
    {
        TestMirror mirror;
        CHECK_FALSE(mirror.HasBaseline());
        CHECK(mirror.AcceptRevision(10) == RevisionCheck::kGap);
        CHECK(mirror.HasBaseline());
        CHECK(mirror.AcceptRevision(11) == RevisionCheck::kInOrder);
        CHECK(mirror.AcceptRevision(11) == RevisionCheck::kDuplicate);
        CHECK(mirror.AcceptRevision(9) == RevisionCheck::kDuplicate);
        CHECK(mirror.AcceptRevision(14) == RevisionCheck::kGap);
        CHECK(mirror.AcceptRevision(15) == RevisionCheck::kInOrder);
        CHECK(mirror.GetGapCount() == 2);
        CHECK(mirror.GetDuplicateCount() == 2);
        CHECK(mirror.GetRevision() == 15);
    }

    SECTION("an own drop is adopted by its cookie, not materialized a second time")
    {
        TestMirror mirror;
        mirror.NotePendingDrop(5, 0xFF000801u, 0);
        mirror.NotePendingDrop(0, 0xFF000802u, 0);  // ignored: no cookie
        mirror.NotePendingDrop(6, 0, 0);            // ignored: no reference
        CHECK(mirror.PendingDropCount() == 1);

        // A peer's drop with the same cookie number is not ours.
        auto peer = mirror.Upsert(1, 3, kPeer, 5, Interior(kCellA), {}, kMe);
        CHECK(peer.Inserted);
        CHECK(peer.AdoptedFormId == 0);
        CHECK(mirror.PendingDropCount() == 1);

        auto mine = mirror.Upsert(2, 3, kMe, 5, Interior(kCellA), {}, kMe);
        CHECK(mine.AdoptedFormId == 0xFF000801u);
        CHECK(mirror.PendingDropCount() == 0);
        CHECK(mirror.FindByFormId(0xFF000801u) == 2);
        REQUIRE(mirror.Find(2));
        CHECK(mirror.Find(2)->Origin == BindingOrigin::kDroppedHere);

        const auto unbound = mirror.CollectUnbound(0);
        REQUIRE(unbound.size() == 1);
        CHECK(unbound[0] == 1);
    }

    SECTION("pending drops time out and are bounded, oldest evicted first")
    {
        ClientPolicy policy{};
        policy.MaxPendingDrops = 3;
        policy.PendingDropTimeoutMs = 1000;
        TestMirror mirror{policy};
        mirror.NotePendingDrop(1, 0xFF000001u, 0);
        mirror.NotePendingDrop(2, 0xFF000002u, 10);
        mirror.NotePendingDrop(3, 0xFF000003u, 20);
        mirror.NotePendingDrop(4, 0xFF000004u, 30);  // evicts cookie 1
        CHECK(mirror.PendingDropCount() == 3);
        CHECK(mirror.Upsert(1, 1, kMe, 1, Interior(kCellA), {}, kMe).AdoptedFormId == 0);
        CHECK(mirror.Upsert(2, 1, kMe, 2, Interior(kCellA), {}, kMe).AdoptedFormId == 0xFF000002u);
        CHECK(mirror.ExpirePendingDrops(1'019) == 0);  // cookie 3 (created at 20) is due at 1020
        CHECK(mirror.ExpirePendingDrops(1'020) == 1);
        CHECK(mirror.ExpirePendingDrops(1'030) == 1);
        CHECK(mirror.PendingDropCount() == 0);
        CHECK(mirror.GetExpiredDropCount() == 2);
    }

    SECTION("a changed stack makes the shown reference stale; the same stack keeps it")
    {
        TestMirror mirror;
        mirror.Upsert(1, 5, kPeer, 0, Interior(kCellA), {}, kMe);
        REQUIRE(mirror.Bind(1, 0xFF0000AAu, BindingOrigin::kMaterializedHere));
        CHECK(mirror.Upsert(1, 5, kPeer, 0, Interior(kCellA), {}, kMe).StaleFormId == 0);
        auto stale = mirror.Upsert(1, 2, kPeer, 0, Interior(kCellA), {}, kMe);
        CHECK(stale.StaleFormId == 0xFF0000AAu);
        CHECK(stale.StaleOrigin == BindingOrigin::kMaterializedHere);
        CHECK(mirror.FindByFormId(0xFF0000AAu) == kNoItem);
        CHECK(mirror.CollectUnbound(0) == std::vector<ItemId>{1});

        // The engine already shows the new count (partial pickup here): keep the binding.
        REQUIRE(mirror.Bind(1, 0xFF0000ABu, BindingOrigin::kDroppedHere));
        CHECK(mirror.Upsert(1, 1, kPeer, 0, Interior(kCellA), {}, kMe, /*shown*/ 1).StaleFormId == 0);
        CHECK(mirror.FindByFormId(0xFF0000ABu) == 1);
        // Invalid upserts change nothing.
        CHECK_FALSE(mirror.Upsert(kNoItem, 1, kPeer, 0, Interior(kCellA), {}, kMe).Inserted);
        CHECK_FALSE(mirror.Upsert(3, 0, kPeer, 0, Interior(kCellA), {}, kMe).Inserted);
        CHECK(mirror.Size() == 1);
    }

    SECTION("a despawn removes by id and reports the reference to delete")
    {
        TestMirror mirror;
        mirror.Upsert(1, 1, kPeer, 0, Interior(kCellA), {}, kMe);
        mirror.Upsert(2, 1, kPeer, 0, Interior(kCellA), {}, kMe);
        mirror.Bind(1, 0xFF000010u, BindingOrigin::kMaterializedHere);
        const auto removed = mirror.Remove(1);
        CHECK(removed.Id == 1);
        CHECK(removed.FormId == 0xFF000010u);
        CHECK(mirror.FindByFormId(0xFF000010u) == kNoItem);
        CHECK(mirror.Remove(2).FormId == 0);  // nothing shown: nothing to delete
        CHECK(mirror.Remove(2).Id == kNoItem);  // twice: harmless
        CHECK(mirror.Size() == 0);
    }

    SECTION("a whole-stack pickup releases the binding at once and holds the item back while the claim is in flight")
    {
        ClientPolicy policy{};
        policy.ClaimInFlightMs = 1000;
        TestMirror mirror{policy};
        mirror.Upsert(1, 4, kPeer, 0, Interior(kCellA), {}, kMe);
        mirror.Bind(1, 0xFF000020u, BindingOrigin::kMaterializedHere);
        mirror.ReleaseConsumedReference(1, 500);
        CHECK(mirror.FindByFormId(0xFF000020u) == kNoItem);
        CHECK(mirror.CollectUnbound(1'000).empty());  // not shown again while in flight
        // The server's despawn: nothing left to delete here.
        // (Alternative: the claim lost and the despawn never came - shown again after the window.)
        CHECK(mirror.CollectUnbound(1'500) == std::vector<ItemId>{1});

        // A remainder upsert answers the claim: shown again right away.
        mirror.Bind(1, 0xFF000021u, BindingOrigin::kMaterializedHere);
        mirror.ReleaseConsumedReference(1, 2'000);
        CHECK(mirror.CollectUnbound(2'001).empty());
        mirror.Upsert(1, 1, kPeer, 0, Interior(kCellA), {}, kMe);
        CHECK(mirror.CollectUnbound(2'001) == std::vector<ItemId>{1});
        CHECK(mirror.Remove(1).FormId == 0);
    }

    SECTION("materialization backs off and quarantines, and a form id shows one item only")
    {
        ClientPolicy policy{};
        policy.MaxMaterializeAttempts = 3;
        policy.MaterializeRetryMs = 100;
        policy.MaterializeQuarantineMs = 10'000;
        TestMirror mirror{policy};
        mirror.Upsert(1, 1, kPeer, 0, Interior(kCellA), {}, kMe);
        mirror.Upsert(2, 1, kPeer, 0, Interior(kCellA), {}, kMe);
        mirror.NoteMaterializeFailed(1, 0);
        CHECK(mirror.CollectUnbound(50) == std::vector<ItemId>{2});
        CHECK(mirror.CollectUnbound(100) == (std::vector<ItemId>{1, 2}));
        mirror.NoteMaterializeFailed(1, 100);
        mirror.NoteMaterializeFailed(1, 200);
        CHECK(mirror.GetQuarantineCount() == 1);
        CHECK(mirror.CollectUnbound(5'000) == std::vector<ItemId>{2});
        CHECK(mirror.CollectUnbound(10'200) == (std::vector<ItemId>{1, 2}));

        REQUIRE(mirror.Bind(1, 0xFF000030u, BindingOrigin::kMaterializedHere));
        REQUIRE(mirror.Bind(2, 0xFF000030u, BindingOrigin::kMaterializedHere));  // the engine reused the form id
        CHECK(mirror.FindByFormId(0xFF000030u) == 2);
        CHECK(mirror.Find(1)->BoundFormId == 0);
        CHECK(mirror.BoundCount() == 1);
        CHECK_FALSE(mirror.Bind(77, 0xFF000031u, BindingOrigin::kMaterializedHere));
        CHECK_FALSE(mirror.Bind(1, 0, BindingOrigin::kMaterializedHere));

        mirror.NoteReferenceLost(2);
        CHECK(mirror.FindByFormId(0xFF000030u) == kNoItem);
        CHECK(mirror.BoundCount() == 0);
    }

    SECTION("snapshot replaces: vanished, stale and adopted references are reported")
    {
        TestMirror mirror;
        mirror.Upsert(1, 1, kPeer, 0, Interior(kCellA), {}, kMe);
        mirror.Upsert(2, 5, kPeer, 0, Interior(kCellA), {}, kMe);
        mirror.Upsert(3, 1, kPeer, 0, Interior(kCellA), {}, kMe);
        mirror.Bind(1, 0xFF000041u, BindingOrigin::kMaterializedHere);
        mirror.Bind(2, 0xFF000042u, BindingOrigin::kMaterializedHere);
        mirror.NotePendingDrop(9, 0xFF000049u, 0);

        std::vector<TestMirror::SnapshotEntry> entries(3);
        entries[0].Id = 2;
        entries[0].Count = 4;  // stack changed
        entries[1].Id = 4;
        entries[1].Count = 2;
        entries[1].DropperPlayerId = kMe;
        entries[1].DropperCookie = 9;
        entries[2].Id = 5;
        entries[2].Count = 1;
        const auto effect = mirror.ApplySnapshot(40, entries, kMe);

        REQUIRE(effect.Vanished.size() == 1);
        CHECK(effect.Vanished[0].Id == 1);
        CHECK(effect.Vanished[0].FormId == 0xFF000041u);
        REQUIRE(effect.Stale.size() == 1);
        CHECK(effect.Stale[0].FormId == 0xFF000042u);
        REQUIRE(effect.Adopted.size() == 1);
        CHECK(effect.Adopted[0].Id == 4);
        CHECK(effect.Adopted[0].FormId == 0xFF000049u);
        CHECK(mirror.Size() == 3);
        CHECK(mirror.Find(3) == nullptr);  // unbound and absent: silently gone
        CHECK(mirror.GetRevision() == 40);
        CHECK(mirror.AcceptRevision(41) == RevisionCheck::kInOrder);
        CHECK(mirror.CollectUnbound(0) == (std::vector<ItemId>{2, 5}));
    }

    SECTION("world load forgets engine references but keeps records; session end returns bindings and forgets all")
    {
        TestMirror mirror;
        mirror.ApplySnapshot(3, {}, kMe);
        mirror.Upsert(1, 1, kPeer, 0, Interior(kCellA), {}, kMe);
        mirror.Upsert(2, 1, kMe, 0, Interior(kCellA), {}, kMe);
        mirror.Bind(1, 0xFF000051u, BindingOrigin::kMaterializedHere);
        mirror.Bind(2, 0xFF000052u, BindingOrigin::kDroppedHere);
        mirror.NoteMaterializeFailed(2, 0);
        mirror.NotePendingDrop(7, 0xFF000057u, 0);

        mirror.ForgetEngineReferences();
        CHECK(mirror.Size() == 2);
        CHECK(mirror.BoundCount() == 0);
        CHECK(mirror.PendingDropCount() == 0);
        CHECK(mirror.CollectUnbound(0) == (std::vector<ItemId>{1, 2}));
        CHECK(mirror.HasBaseline());

        mirror.Bind(1, 0xFF000061u, BindingOrigin::kMaterializedHere);
        const auto bindings = mirror.Reset();
        REQUIRE(bindings.size() == 1);
        CHECK(bindings[0].FormId == 0xFF000061u);
        CHECK(mirror.Size() == 0);
        CHECK_FALSE(mirror.HasBaseline());
        CHECK(mirror.AcceptRevision(4) == RevisionCheck::kGap);
    }
}

TEST_CASE("v4.1.0 world items: materialize range", "[rfab][v410][worlditems]")
{
    Placement item{};
    item.CellKey = kCellA;
    Placement viewer = item;
    CHECK(IsWithinMaterializeRange(item, viewer));
    viewer.CellKey = kCellB;
    CHECK_FALSE(IsWithinMaterializeRange(item, viewer));  // another interior
    item.CellKey = 0;
    viewer.CellKey = 0;
    CHECK_FALSE(IsWithinMaterializeRange(item, viewer));  // no cell at all is never "here"

    Placement outside{};
    outside.WorldSpaceKey = PackKey(0, 0x3C);
    outside.CellKey = kCellA;
    outside.X = 1000.0f;
    outside.Y = 1000.0f;
    Placement player = outside;
    player.CellKey = kCellB;  // another exterior cell of the same worldspace: distance decides
    CHECK(IsWithinMaterializeRange(outside, player));
    player.X += kExteriorMaterializeRadius;
    CHECK(IsWithinMaterializeRange(outside, player));
    player.X += 1.0f;
    CHECK_FALSE(IsWithinMaterializeRange(outside, player));
    player = outside;
    player.Z += 100'000.0f;  // height is ignored
    CHECK(IsWithinMaterializeRange(outside, player));
    player.WorldSpaceKey = PackKey(0, 0x3D);
    CHECK_FALSE(IsWithinMaterializeRange(outside, player));
    Placement inside{};
    inside.CellKey = kCellA;
    CHECK_FALSE(IsWithinMaterializeRange(outside, inside));
    CHECK_FALSE(IsWithinMaterializeRange(inside, outside));
}

// ---------------------------------------------------------------------------------------------
// Wire
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 world items: messages round-trip and sanitize hostile values", "[rfab][v410][worlditems][wire]")
{
    SECTION("spawn")
    {
        NotifyWorldItemSpawn sent{};
        sent.Revision = 0xFFFFFFF0u;
        sent.State = SampleState(42);
        const auto received = RoundTripServer(sent);
        CHECK(received == sent);
    }

    SECTION("despawn, including an unknown reason byte")
    {
        NotifyWorldItemDespawn sent{};
        sent.Revision = 9;
        sent.ItemId = 4;
        sent.Reason = DespawnReason::kClaimRejected;
        sent.PickerPlayerId = 3;
        CHECK(RoundTripServer(sent) == sent);

        CHECK(SanitizeDespawnReason(200) == DespawnReason::kRemoved);
        CHECK(std::string(ToString(DespawnReason::kPickedUp)) == "picked-up");
    }

    SECTION("snapshot, including a lying element count")
    {
        NotifyWorldItemSnapshot sent{};
        sent.Revision = 77;
        for (uint32_t i = 1; i <= 50; ++i)
            sent.Items.push_back(SampleState(i));
        CHECK(RoundTripServer(sent) == sent);

        TiltedPhoques::Buffer buff(256);
        TiltedPhoques::Buffer::Writer writer(&buff);
        writer.WriteBits(static_cast<uint8_t>(kNotifyWorldItemSnapshot), 8);
        TiltedPhoques::Serialization::WriteVarInt(writer, 1);
        TiltedPhoques::Serialization::WriteVarInt(writer, 1'000'000'000ull);  // claims a billion entries
        TiltedPhoques::Buffer::Reader reader(&buff);
        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);
        REQUIRE(pMessage);
        CHECK(static_cast<NotifyWorldItemSnapshot*>(pMessage.get())->Items.size() <= kMaxSnapshotItems);
    }

    SECTION("drop and pickup requests; non-finite and out-of-range floats never survive")
    {
        RequestWorldItemDrop drop{};
        drop.Cookie = 0xDEADBEEFu;
        drop.ActorServerId = 0;  // entity 0 is a valid server id
        drop.Item = SampleState(1).Item;
        drop.CellId = GameId(0, 0x3C);
        drop.WorldSpaceId = GameId(0, 0x3C0);
        drop.Position = {1.5f, 2.5f, -3.5f};
        drop.Rotation = {0.1f, 0.2f, 0.3f};
        CHECK(RoundTripClient(drop) == drop);

        drop.Position = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1e30f};
        drop.Rotation = {-std::numeric_limits<float>::infinity(), 0.0f, 1e9f};
        const auto clean = RoundTripClient(drop);
        CHECK(clean.Position.x == 0.0f);
        CHECK(clean.Position.y == 0.0f);
        CHECK(std::isfinite(clean.Position.z));
        CHECK(std::abs(clean.Position.z) <= 2097152.0f);
        CHECK(clean.Rotation.x == 0.0f);
        CHECK(std::abs(clean.Rotation.z) <= 1000.0f);

        RequestWorldItemPickup pickup{};
        pickup.ItemId = 12;
        pickup.PickerServerId = 0x40;
        pickup.Item = SampleState(1).Item;
        CHECK(RoundTripClient(pickup) == pickup);

        NotifyWorldItemSpawn spawn{};
        spawn.State = SampleState(5);
        spawn.State.Position.x = std::numeric_limits<float>::quiet_NaN();
        spawn.State.Item.Count = -4;
        const auto sanitized = RoundTripServer(spawn);
        CHECK(sanitized.State.Position.x == 0.0f);
        CHECK(sanitized.State.Item.Count == 0);
    }

    SECTION("opcodes are classified for the paused queue and survive a world reload")
    {
        for (const uint32_t op : {static_cast<uint32_t>(kNotifyWorldItemSpawn), static_cast<uint32_t>(kNotifyWorldItemDespawn),
                                  static_cast<uint32_t>(kNotifyWorldItemSnapshot)})
        {
            INFO("opcode " << op);
            CHECK(CoopWorldStabilizer::ClassifyPausedInboundPacket(op) ==
                  CoopWorldStabilizer::PausedInboundPacketAction::kQueueUntilUnpaused);
            CHECK(CoopWorldStabilizer::MustSurviveWorldReload(op));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Resync scope + feature bit
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 world items: resync scope and feature switch", "[rfab][v410][worlditems][resync]")
{
    using namespace Session::Resync;

    SECTION("a revision gap becomes one world-items request, independent of entity resyncs")
    {
        ClientLedger ledger{};
        CHECK(ledger.NoteGap(Scope::kWorldItems, kNoServerId, Reason::kWorldItemRevisionGap, 0));
        CHECK_FALSE(ledger.NoteGap(Scope::kWorldItems, kNoServerId, Reason::kWorldItemRevisionGap, 0));  // absorbed
        CHECK(ledger.HasPendingWorldItems());
        CHECK(ledger.NoteGap(Scope::kAllEntities, kNoServerId, Reason::kManual, 0));
        CHECK(ledger.HasPendingWorldItems());  // a whole-session resync does not include dropped items

        std::vector<Request> out;
        ledger.Poll(0, false, out);
        CHECK(out.empty());  // waits while the client cannot send
        ledger.Poll(10, true, out);
        const auto it = std::find_if(out.begin(), out.end(), [](const Request& r) { return r.RequestScope == Scope::kWorldItems; });
        REQUIRE(it != out.end());
        CHECK(it->GapReason == Reason::kWorldItemRevisionGap);
        CHECK(it->ServerId == kNoServerId);
        CHECK_FALSE(ledger.HasPendingWorldItems());

        // Completion, then a new gap inside the cooldown waits for it.
        ledger.OnComplete(it->RequestId, Outcome::kApplied, 20);
        CHECK(ledger.NoteGap(Scope::kWorldItems, kNoServerId, Reason::kWorldItemRevisionGap, 30));
        out.clear();
        ledger.Poll(40, true, out);
        CHECK(std::none_of(out.begin(), out.end(), [](const Request& r) { return r.RequestScope == Scope::kWorldItems; }));
        ledger.Poll(10 + Session::Resync::ClientPolicy{}.WorldItemsCooldownMs, true, out);
        CHECK(std::any_of(out.begin(), out.end(), [](const Request& r) { return r.RequestScope == Scope::kWorldItems; }));

        // A lost reply is re-armed by a world reload.
        ledger.AbandonInFlight(20'000);
        CHECK(ledger.HasPendingWorldItems());
    }

    SECTION("names and the server budget")
    {
        CHECK(std::string(ToString(Scope::kWorldItems)) == "world-items");
        CHECK(std::string(ToString(Reason::kWorldItemRevisionGap)) == "world-item-revision-gap");
        CHECK(IsKnownScope(static_cast<uint8_t>(Scope::kWorldItems)));
        ServerBudget budget{};
        CHECK(budget.Admit(Scope::kWorldItems, 1'000));
        CHECK_FALSE(budget.Admit(Scope::kWorldItems, 2'000));
        CHECK(budget.Admit(Scope::kStringCache, 2'000));
        CHECK(budget.Admit(Scope::kWorldItems, 3'000));
    }

    SECTION("feature bit")
    {
        using Session::Feature;
        CHECK(Session::Bit(Feature::kServerOwnedWorldItems) == (1u << 8));
        CHECK((Session::kAllKnownFeatureBits & Session::Bit(Feature::kServerOwnedWorldItems)) != 0);
        CHECK(Session::IsEnabledIn(0u, Feature::kServerOwnedWorldItems));
        CHECK_FALSE(Session::IsEnabledIn(Session::Bit(Feature::kServerOwnedWorldItems), Feature::kServerOwnedWorldItems));
        CHECK(Session::WithFeature(0u, Feature::kServerOwnedWorldItems, false) == Session::Bit(Feature::kServerOwnedWorldItems));
    }
}

// ---------------------------------------------------------------------------------------------
// Convergence under loss: the property the design exists for
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 world items: clients converge on the server under loss, races and reloads", "[rfab][v410][worlditems][sim]")
{
    // A server registry and three clients. Deltas to a client are dropped at random (a lost queue,
    // a world reload discarding the paused packets); the client notices the revision gap and gets a
    // snapshot. Clients race for the same items. Invariants:
    //  - nothing is ever granted twice: granted + still in the world == dropped, per item;
    //  - after every client has applied a final snapshot, every mirror equals the registry;
    //  - a client never holds two bindings to one form id.
    for (uint32_t seed = 1; seed <= 40; ++seed)
    {
        std::mt19937 rng(seed);
        TestRegistry server;
        constexpr uint32_t kClients = 3;
        std::vector<TestMirror> mirrors(kClients);
        std::vector<bool> needsSnapshot(kClients, true);
        std::map<ItemId, int64_t> dropped;
        std::map<ItemId, int64_t> granted;
        uint32_t nextForm = 0xFF000000u;
        uint64_t now = 0;
        uint32_t cookie = 1;

        const auto placementOf = [](const TestRegistry::Record& acRecord)
        {
            Placement p{};
            p.CellKey = acRecord.CellKey;
            return p;
        };
        const auto sendSnapshot = [&](uint32_t aClient)
        {
            std::vector<TestMirror::SnapshotEntry> entries;
            for (const auto& record : server.Export())
            {
                TestMirror::SnapshotEntry entry{};
                entry.Id = record.Id;
                entry.Count = record.Count;
                entry.DropperPlayerId = record.DropperPlayerId;
                entry.DropperCookie = record.DropperCookie;
                entry.Where = placementOf(record);
                entry.Payload = record.Payload;
                // The service reports what the bound reference shows; the sim's references always
                // show the count they were bound with.
                if (const auto* pMine = mirrors[aClient].Find(record.Id); pMine && pMine->BoundFormId != 0)
                    entry.ShownCount = pMine->Count;
                entries.push_back(entry);
            }
            mirrors[aClient].ApplySnapshot(server.GetRevision(), std::move(entries), aClient + 1);
            needsSnapshot[aClient] = false;
        };
        const auto broadcastUpsert = [&](const TestRegistry::Record& acRecord)
        {
            for (uint32_t c = 0; c < kClients; ++c)
            {
                if (rng() % 5 == 0)
                    continue;  // lost
                if (mirrors[c].AcceptRevision(server.GetRevision()) == RevisionCheck::kDuplicate)
                    continue;
                mirrors[c].Upsert(acRecord.Id, acRecord.Count, acRecord.DropperPlayerId, acRecord.DropperCookie,
                                  placementOf(acRecord), acRecord.Payload, c + 1);
            }
        };
        const auto broadcastDespawn = [&](ItemId aId)
        {
            for (uint32_t c = 0; c < kClients; ++c)
            {
                if (rng() % 5 == 0)
                    continue;  // lost
                (void)mirrors[c].AcceptRevision(server.GetRevision());
                (void)mirrors[c].Remove(aId);
            }
        };

        std::vector<uint32_t> gapCounts(kClients, 0);
        for (int step = 0; step < 600; ++step)
        {
            now += rng() % 300;
            const uint32_t c = rng() % kClients;
            switch (rng() % 6)
            {
            case 0:
            case 1:
            {
                // Client c drops a stack; its engine created a reference first.
                const uint32_t myCookie = cookie++;
                const uint32_t form = ++nextForm;
                mirrors[c].NotePendingDrop(myCookie, form, now);
                ItemId id = kNoItem;
                const int32_t count = 1 + static_cast<int32_t>(rng() % 5);
                if (server.TryDrop(c + 1, PackKey(0, rng() % 4), count, myCookie, TestPayload{rng() % 3}, now, id) == DropResult::kAccepted)
                {
                    dropped[id] = count;
                    broadcastUpsert(*server.Find(id));
                }
                break;
            }
            case 2:
            case 3:
            {
                // Client c picks up something it shows (or something it remembers but does not show).
                const auto bindings = mirrors[c].CollectBindings();
                if (bindings.empty())
                    break;
                const auto& pick = bindings[rng() % bindings.size()];
                const auto* pRecord = mirrors[c].Find(pick.Id);
                REQUIRE(pRecord);
                const int32_t want = (rng() % 3 == 0) ? std::max(1, pRecord->Count / 2) : pRecord->Count;
                if (want >= pRecord->Count)
                    mirrors[c].ReleaseConsumedReference(pick.Id, now);
                const auto claim = server.TryClaim(pick.Id, want);
                if (claim.Result == ClaimResult::kGrantedAll)
                {
                    granted[pick.Id] += claim.Granted;
                    broadcastDespawn(pick.Id);
                }
                else if (claim.Result == ClaimResult::kGrantedPartial)
                {
                    granted[pick.Id] += claim.Granted;
                    broadcastUpsert(*server.Find(pick.Id));
                }
                else
                {
                    // Lost the race: the server answers this claimer with a despawn at the current
                    // revision; it is applied whatever the revision says.
                    (void)mirrors[c].Remove(pick.Id);
                }
                break;
            }
            case 4:
            {
                // Materialize what is unbound.
                for (const ItemId id : mirrors[c].CollectUnbound(now))
                {
                    if (rng() % 4 == 0)
                        mirrors[c].NoteMaterializeFailed(id, now);
                    else
                        REQUIRE(mirrors[c].Bind(id, ++nextForm, BindingOrigin::kMaterializedHere));
                }
                break;
            }
            default:
            {
                // World reload on client c, or a snapshot answering its gap.
                if (rng() % 2 == 0)
                    mirrors[c].ForgetEngineReferences();
                if (mirrors[c].GetGapCount() != gapCounts[c] || needsSnapshot[c])
                {
                    gapCounts[c] = mirrors[c].GetGapCount();
                    sendSnapshot(c);
                }
                break;
            }
            }

            // No double grant, ever.
            for (const auto& [id, total] : dropped)
            {
                const auto* pRecord = server.Find(id);
                const int64_t inWorld = pRecord ? pRecord->Count : 0;
                REQUIRE(granted[id] + inWorld == total);
            }
            // One form id never shows two items.
            for (const auto& mirror : mirrors)
            {
                std::set<uint32_t> forms;
                for (const auto& binding : mirror.CollectBindings())
                    REQUIRE(forms.insert(binding.FormId).second);
            }
        }

        // Final repair: every client applies a snapshot and must equal the registry.
        for (uint32_t c = 0; c < kClients; ++c)
        {
            sendSnapshot(c);
            const auto records = server.Export();
            REQUIRE(mirrors[c].Size() == records.size());
            for (const auto& record : records)
            {
                const auto* pMine = mirrors[c].Find(record.Id);
                REQUIRE(pMine);
                CHECK(pMine->Count == record.Count);
                CHECK(pMine->Payload == record.Payload);
            }
            CHECK(mirrors[c].GetRevision() == server.GetRevision());
        }
    }
}
