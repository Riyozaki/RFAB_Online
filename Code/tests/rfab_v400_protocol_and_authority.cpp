#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/Stl.hpp>

#include <catch2/catch.hpp>

#include <Authority/Health.h>
#include <Authority/Lease.h>
#include <CoopWorldView.h>
#include <RfabModpackCompat.h>

TEST_CASE("v4.0.0 Protocol version family and SSOT invariants", "[rfab][v400][version]")
{
    REQUIRE(RfabCompat::kRfabOnlineVersion == "4.0.0");

    // Branch builds tagged from VERSION ("v4.0.0-g<sha>") interoperate with release "v4.0.0"
    // and strictly reject v3.1.x clients/servers across the v4.0.0 wire break.
    CHECK(RfabCompat::AreBuildVersionsProtocolCompatible("v4.0.0", "v4.0.0"));
    CHECK(RfabCompat::AreBuildVersionsProtocolCompatible("v4.0.0-g1a2b3c4", "v4.0.0"));
    CHECK(RfabCompat::AreBuildVersionsProtocolCompatible("v4.0.0", "v4.0.2-g9988776"));
    CHECK_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("v4.0.0-g1a2b3c4", "v3.1.8"));
    CHECK_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("v4.0.0", "v3.1.7-4-gcac3bf5"));
    CHECK_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("v4.0.0", "v2.1.0"));
}

TEST_CASE("v4.0.0 EntityIndexTable preserves ServerId binding across Remote-before-Local relinquish order",
          "[rfab][v400][worldview]")
{
    using Table = CoopWorldView::EntityIndexTable<uint32_t>;
    constexpr uint32_t kEntity = 7u;
    constexpr uint32_t kFormId = 0x000E5F72u;
    constexpr uint32_t kServerId = 0x42u;

    SECTION("Old remove-Local-before-Remote order opened a window where FindByServerId returned nullopt")
    {
        Table index;
        index.UpsertFormId(kEntity, kFormId, false);
        index.BindServerId(kEntity, Table::kLocal, kServerId);
        REQUIRE(index.FindByServerId(kServerId) == std::optional<uint32_t>{kEntity});

        index.UnbindServerId(kEntity, Table::kLocal);
        CHECK(index.FindByServerId(kServerId) == std::nullopt);
    }

    SECTION("v4.0.0 emplace-Remote-before-remove-Local order keeps FindByServerId uninterrupted")
    {
        Table index;
        index.UpsertFormId(kEntity, kFormId, false);
        index.BindServerId(kEntity, Table::kLocal, kServerId);
        REQUIRE(index.FindByServerId(kServerId) == std::optional<uint32_t>{kEntity});

        index.BindServerId(kEntity, Table::kRemote, kServerId);
        index.UnbindServerId(kEntity, Table::kLocal);

        CHECK(index.FindByServerId(kServerId) == std::optional<uint32_t>{kEntity});
        CHECK(index.FindActorByFormId(kFormId) == std::optional<uint32_t>{kEntity});
    }
}

TEST_CASE("v4.0.0 End-to-end HealthRevision causality prevents false kStaleIncrease on owner regen",
          "[rfab][v400][health]")
{
    // Mirrors the full v4.0.0 lifecycle:
    // 1) CreateCharacter initializes authoritative HP=200/200 and sends AssignCharacterResponse{HealthRevision=rev0}
    const auto spawned = Authority::ApplyAbsoluteHealth(0.0f, 200.0f, 200.0f, 0u);
    REQUIRE(spawned.Revision == 1u);

    float serverHp = spawned.Value;
    uint32_t serverRev = spawned.Revision;
    uint32_t ownerKnownRev = spawned.Revision; // learned from AssignCharacterResponse.HealthRevision

    // 2) Owner lands a -50 hit -> server writes 150 (rev=2) and sends empty-Values NotifyActorValueChanges ack
    const auto hit = Authority::ApplyHealthDelta(serverHp, 200.0f, -50.0f);
    const auto afterHit = Authority::ApplyAbsoluteHealth(serverHp, hit.UpdatedHealth, 200.0f, serverRev);
    serverHp = afterHit.Value;
    serverRev = afterHit.Revision;
    REQUIRE(serverHp == Approx(150.0f));
    REQUIRE(serverRev == 2u);

    // Without the v4.0.0 revision ack, ownerKnownRev stayed at 1 and legitimate regen (+10 -> 160) was
    // misclassified as kStaleIncrease. With the ack, ownerKnownRev advances to 2 and regen is accepted!
    CHECK(Authority::ClassifyHealthSnapshot(serverHp, 160.0f, serverRev, 1u) ==
          Authority::SnapshotVerdict::kStaleIncrease);

    ownerKnownRev = serverRev; // ack received
    CHECK(Authority::ClassifyHealthSnapshot(serverHp, 160.0f, serverRev, ownerKnownRev) ==
          Authority::SnapshotVerdict::kApply);

    const auto afterRegen = Authority::ApplyAbsoluteHealth(serverHp, 160.0f, 200.0f, serverRev);
    serverHp = afterRegen.Value;
    serverRev = afterRegen.Revision;
    ownerKnownRev = serverRev; // ack received
    REQUIRE(serverHp == Approx(160.0f));
    REQUIRE(serverRev == 3u);

    // 3) Peer lands killing blow -> server writes 0 (rev=4), NotifyDeathStateChange carries HealthRevision=4,
    //    and an in-flight pre-death owner snapshot (HP=160, rev=3) is cleanly refused!
    const auto afterKill = Authority::ApplyAbsoluteHealth(serverHp, 0.0f, 200.0f, serverRev);
    serverHp = afterKill.Value;
    serverRev = afterKill.Revision;
    REQUIRE(serverHp == 0.0f);
    REQUIRE(serverRev == 4u);

    CHECK(Authority::ClassifyHealthSnapshot(serverHp, 160.0f, serverRev, ownerKnownRev) ==
          Authority::SnapshotVerdict::kStaleIncrease);
}
