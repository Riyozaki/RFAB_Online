#if defined(RFAB_STANDALONE_TEST)
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#define TEST_CASE_IMPL2(line, name)                                                                                    \
    static void rfab_test_##line();                                                                                    \
    namespace                                                                                                          \
    {                                                                                                                  \
    struct RfabReg_##line                                                                                              \
    {                                                                                                                  \
        RfabReg_##line() { GetRfabTests().push_back({name, &rfab_test_##line}); }                                      \
    } s_reg_##line;                                                                                                    \
    }                                                                                                                  \
    static void rfab_test_##line()

#define TEST_CASE_EXPAND(line, name) TEST_CASE_IMPL2(line, name)
#define TEST_CASE(name, ...) TEST_CASE_EXPAND(__LINE__, name)

namespace
{
struct RfabTestEntry
{
    const char* Name;
    void (*Fn)();
};

inline std::vector<RfabTestEntry>& GetRfabTests()
{
    static std::vector<RfabTestEntry> s_tests;
    return s_tests;
}

inline int& GetAssertionCount()
{
    static int s_assertions = 0;
    return s_assertions;
}
} // namespace

#define REQUIRE(expr)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        ++GetAssertionCount();                                                                                         \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            std::cerr << "REQUIRE failed at " << __FILE__ << ":" << __LINE__ << ": " << #expr << "\n";                 \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (false)

#define REQUIRE_FALSE(expr) REQUIRE(!(expr))

#else
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <catch2/catch.hpp>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/ServerReferencesMoveRequest.h>
#endif

#include <Structs/GameId.h>
#include <Structs/GridCellCoords.h>
#include <common/CoopWorldView.h>

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#define TP_INTERNAL_COMPONENTS_GUARD
#include <Components/CellIdComponent.h>
#undef TP_INTERNAL_COMPONENTS_GUARD
#else
#include <Components/CellIdComponent.h>
#endif

TEST_CASE("CoopWorldView WorldOrCell enforces strict Interior/Exterior and WorldSpace walls", "[rfab][stage1][worldview]")
{
    const GameId kTamriel{0u, 0x00003Cu};
    const GameId kWhiterunWorld{0u, 0x01A26Fu};
    const GameId kFortGreymoor01{0u, 0x01F6CCu};
    const GameId kFortGreymoor02{0u, 0x0379FAu};
    const GameId kTempWildernessCellA{0xFFFFFFFFu, 0x000101u};
    const GameId kTempWildernessCellB{0xFFFFFFFFu, 0x000999u};

    const auto extTamrielA = CoopWorldView::WorldOrCell::FromPacket(kTamriel, kTempWildernessCellA, 8500.f, -4200.f);
    const auto extTamrielB = CoopWorldView::WorldOrCell::FromPacket(kTamriel, kTempWildernessCellB, 8600.f, -4150.f);
    const auto extWhiterun = CoopWorldView::WorldOrCell::FromPacket(kWhiterunWorld, GameId{0u, 0x01A270u}, 8500.f, -4200.f);
    const auto intFort01 = CoopWorldView::WorldOrCell::FromPacket(GameId{}, kFortGreymoor01, 100.f, 200.f);
    const auto intFort02 = CoopWorldView::WorldOrCell::FromPacket(GameId{}, kFortGreymoor02, 100.f, 200.f);

    REQUIRE(extTamrielA.IsValid());
    REQUIRE(extTamrielA.IsExterior());
    REQUIRE_FALSE(extTamrielA.IsInterior());

    // Two clients with completely different 0xFF00xxxx unnamed wilderness CellIds at the same Tamriel coordinates
    // MUST resolve to the exact same WorldOrCell!
    REQUIRE(extTamrielA == extTamrielB);
    REQUIRE(extTamrielA.IsSameSpace(extTamrielB));

    // Tamriel vs WhiterunWorld: different worldspaces NEVER share a space even at identical (X, Y)
    REQUIRE(extTamrielA != extWhiterun);
    REQUIRE_FALSE(extTamrielA.IsSameSpace(extWhiterun));

    // Exterior vs Interior: NEVER share a space (eliminates Mammoth/Giant inside Bandit Fort!)
    REQUIRE(extTamrielA != intFort01);
    REQUIRE_FALSE(extTamrielA.IsSameSpace(intFort01));
    REQUIRE_FALSE(intFort01.IsSameSpace(extTamrielA));

    // Different interior cells NEVER share a space
    REQUIRE(intFort01.IsInterior());
    REQUIRE(intFort01 != intFort02);
    REQUIRE_FALSE(intFort01.IsSameSpace(intFort02));

    // Invalid empty packet (neither WorldSpaceId nor non-temp InteriorCellId)
    const auto invalidTempInterior = CoopWorldView::WorldOrCell::FromPacket(GameId{}, kTempWildernessCellA, 0.f, 0.f);
    REQUIRE_FALSE(invalidTempInterior.IsValid());
}

TEST_CASE("CoopWorldView EvaluateViewDecision prevents interior bleed and exterior boundary thrash", "[rfab][stage1][worldview]")
{
    const GameId kTamriel{0u, 0x00003Cu};
    const GameId kFortGreymoor01{0u, 0x01F6CCu};

    CoopWorldView::ObserverState obsExterior{};
    obsExterior.EngineReady = true;
    obsExterior.Location = CoopWorldView::WorldOrCell::MakeExterior(kTamriel, GridCellCoords(0, 0));
    obsExterior.PosX = 0.f;
    obsExterior.PosY = 0.f;

    // 1. Exterior Mammoth inside 5x5 grid materializes when target cell is loaded
    CoopWorldView::RemoteViewQuery mammoth{};
    mammoth.Location = CoopWorldView::WorldOrCell::FromPacket(kTamriel, GameId{}, 6000.f, -3000.f);
    mammoth.PosX = 6000.f;
    mammoth.PosY = -3000.f;
    mammoth.CurrentlyMaterialized = false;
    mammoth.TargetCellLoadedInEngine = true;

    REQUIRE(CoopWorldView::EvaluateViewDecision(obsExterior, mammoth) == CoopWorldView::ViewDecision::kMaterialize);

    // 2. While cell is still streaming (TargetCellLoadedInEngine == false), do NOT materialize early
    mammoth.TargetCellLoadedInEngine = false;
    REQUIRE(CoopWorldView::EvaluateViewDecision(obsExterior, mammoth) == CoopWorldView::ViewDecision::kKeepDematerialized);

    // 3. Once materialized, entering FortGreymoor01 IMMEDIATELY dematerializes the exterior Mammoth!
    mammoth.CurrentlyMaterialized = true;
    mammoth.TargetCellLoadedInEngine = false;

    CoopWorldView::ObserverState obsInterior{};
    obsInterior.EngineReady = true;
    obsInterior.Location = CoopWorldView::WorldOrCell::MakeInterior(kFortGreymoor01);
    obsInterior.PosX = 250.f;
    obsInterior.PosY = -120.f;

    REQUIRE(CoopWorldView::EvaluateViewDecision(obsInterior, mammoth) == CoopWorldView::ViewDecision::kDematerialize);

    // And while dematerialized inside FortGreymoor01, the Mammoth stays dematerialized (NEVER spawns in fort!)
    mammoth.CurrentlyMaterialized = false;
    mammoth.TargetCellLoadedInEngine = true; // Even if someone mistakenly passed true, topological wall blocks it!
    REQUIRE(CoopWorldView::EvaluateViewDecision(obsInterior, mammoth) == CoopWorldView::ViewDecision::kKeepDematerialized);

    // 4. Exterior 5x5 boundary hysteresis:
    // For centerGrid=(0,0), outer 5x5 upper bound is +3*4096 = 12288, and 384-unit entry threshold is 11904!
    CoopWorldView::RemoteViewQuery edgePeer{};
    edgePeer.Location = CoopWorldView::WorldOrCell::FromPacket(kTamriel, GameId{}, 12100.f, 0.f);
    edgePeer.PosX = 12100.f; // Grid (2,0), between 11904 and 12288!
    edgePeer.PosY = 0.f;
    edgePeer.IsPlayer = true;
    edgePeer.TargetCellLoadedInEngine = true;

    // Not yet materialized -> stays dematerialized until clearly inside 11904
    edgePeer.CurrentlyMaterialized = false;
    REQUIRE(CoopWorldView::EvaluateViewDecision(obsExterior, edgePeer) == CoopWorldView::ViewDecision::kKeepDematerialized);

    // Already materialized -> stays materialized across the entire loaded 5x5 cell up to 12288 (zero Delete/Create blinking!)
    edgePeer.CurrentlyMaterialized = true;
    REQUIRE(CoopWorldView::EvaluateViewDecision(obsExterior, edgePeer) == CoopWorldView::ViewDecision::kKeepMaterialized);

    // Stepping into grid (3,0) (> 12288) -> dematerializes cleanly
    edgePeer.PosX = 12400.f;
    edgePeer.Location = CoopWorldView::WorldOrCell::FromPacket(kTamriel, GameId{}, 12400.f, 0.f);
    REQUIRE(CoopWorldView::EvaluateViewDecision(obsExterior, edgePeer) == CoopWorldView::ViewDecision::kDematerialize);

    // Dragons stay visible up to 20x20 grid
    CoopWorldView::RemoteViewQuery dragon{};
    dragon.Location = CoopWorldView::WorldOrCell::FromPacket(kTamriel, GameId{}, 28000.f, 0.f);
    dragon.PosX = 28000.f;
    dragon.PosY = 0.f;
    dragon.IsDragon = true;
    dragon.CurrentlyMaterialized = false;
    dragon.TargetCellLoadedInEngine = true;
    REQUIRE(CoopWorldView::EvaluateViewDecision(obsExterior, dragon) == CoopWorldView::ViewDecision::kMaterialize);

    // Loading screen (EngineReady == false) dematerializes / holds spawns safely
    CoopWorldView::ObserverState obsLoading = obsExterior;
    obsLoading.EngineReady = false;
    dragon.CurrentlyMaterialized = true;
    REQUIRE(CoopWorldView::EvaluateViewDecision(obsLoading, dragon) == CoopWorldView::ViewDecision::kDematerialize);
}

TEST_CASE("CoopWorldView EvaluateMovementTransition flushes interpolation on cell/worldspace jumps", "[rfab][stage1][worldview]")
{
    const GameId kTamriel{0u, 0x00003Cu};
    const GameId kFortGreymoor01{0u, 0x01F6CCu};

    CoopWorldView::ObserverState obs{};
    obs.EngineReady = true;
    obs.Location = CoopWorldView::WorldOrCell::MakeExterior(kTamriel, GridCellCoords(0, 0));

    const auto prevExt = CoopWorldView::WorldOrCell::FromPacket(kTamriel, GameId{}, 1000.f, 1000.f);
    const auto nextExt = CoopWorldView::WorldOrCell::FromPacket(kTamriel, GameId{}, 1050.f, 1020.f);

    // Normal walking step: keep smooth interpolation buffer
    const auto smooth = CoopWorldView::EvaluateMovementTransition(
        obs, prevExt, 1000.f, 1000.f, 50.f, nextExt, 1050.f, 1020.f, 52.f);
    REQUIRE(smooth.HasAuthoritativeLocation);
    REQUIRE(smooth.CompatibleWithObserver);
    REQUIRE_FALSE(smooth.FlushInterpolationHistory);

    // Remote peer enters interior door: flush spline & mark incompatible with exterior observer!
    const auto nextInt = CoopWorldView::WorldOrCell::FromPacket(GameId{}, kFortGreymoor01, 0.f, 0.f);
    const auto doorCross = CoopWorldView::EvaluateMovementTransition(
        obs, prevExt, 1000.f, 1000.f, 50.f, nextInt, 0.f, 0.f, 0.f);
    REQUIRE(doorCross.HasAuthoritativeLocation);
    REQUIRE_FALSE(doorCross.CompatibleWithObserver);
    REQUIRE(doorCross.FlushInterpolationHistory);

    // Fast-travel / teleport > 2048 units inside same worldspace: flush spline so actor never slides across mountains!
    const auto sameGridFar = CoopWorldView::WorldOrCell::FromPacket(kTamriel, GameId{}, 3800.f, 3800.f);
    const auto fastTravel = CoopWorldView::EvaluateMovementTransition(
        obs, prevExt, 1000.f, 1000.f, 50.f, sameGridFar, 3800.f, 3800.f, 400.f);
    REQUIRE(fastTravel.LargePositionJump);
    REQUIRE(fastTravel.FlushInterpolationHistory);
}

TEST_CASE("CoopWorldView ServerInterestRegistry guarantees symmetric spawn/remove and player model preservation", "[rfab][stage1][worldview]")
{
    CoopWorldView::ServerInterestRegistry registry;

    // Non-player NPC: enter range -> kSendSpawn, leave range -> kSendRemove
    REQUIRE(registry.UpdateInterest(1u, 500u, false, true) == CoopWorldView::ServerPeerSyncAction::kSendSpawn);
    REQUIRE(registry.IsMarkedInRange(1u, 500u));
    REQUIRE(registry.UpdateInterest(1u, 500u, false, true) == CoopWorldView::ServerPeerSyncAction::kNone);

    REQUIRE(registry.UpdateInterest(1u, 500u, false, false) == CoopWorldView::ServerPeerSyncAction::kSendRemove);
    REQUIRE_FALSE(registry.IsMarkedInRange(1u, 500u));
    REQUIRE(registry.UpdateInterest(1u, 500u, false, false) == CoopWorldView::ServerPeerSyncAction::kNone);

    // Human player entity: enter range -> kSendSpawn, leave range -> kSendPlayerCellChangedOnly (NEVER wipes peer WorldModel!)
    REQUIRE(registry.UpdateInterest(1u, 777u, true, true) == CoopWorldView::ServerPeerSyncAction::kSendSpawn);
    REQUIRE(registry.IsMarkedInRange(1u, 777u));
    REQUIRE(registry.UpdateInterest(1u, 777u, true, false) == CoopWorldView::ServerPeerSyncAction::kSendPlayerCellChangedOnly);
    REQUIRE_FALSE(registry.IsMarkedInRange(1u, 777u));
    // Re-entering range sends a fresh CharacterSpawnRequest immediately!
    REQUIRE(registry.UpdateInterest(1u, 777u, true, true) == CoopWorldView::ServerPeerSyncAction::kSendSpawn);

    // Observer disconnect cleans up state
    registry.RemoveObserver(1u);
    REQUIRE_FALSE(registry.IsMarkedInRange(1u, 777u));
    REQUIRE(registry.UpdateInterest(1u, 777u, true, true) == CoopWorldView::ServerPeerSyncAction::kSendSpawn);

    // Entity destruction cleans up across all observers
    REQUIRE(registry.UpdateInterest(2u, 777u, true, true) == CoopWorldView::ServerPeerSyncAction::kSendSpawn);
    registry.ForgetEntityEverywhere(777u);
    REQUIRE_FALSE(registry.IsMarkedInRange(1u, 777u));
    REQUIRE_FALSE(registry.IsMarkedInRange(2u, 777u));
}

TEST_CASE("CellIdComponent handles unnamed 0:0 exterior cells and rejects cross-space ranges", "[rfab][stage1][server]")
{
    const GameId kTamriel{0u, 0x00003Cu};
    const GameId kWhiterunWorld{0u, 0x01A26Fu};
    const GameId kFortGreymoor01{0u, 0x01F6CCu};

    // Unnamed wilderness cell has Cell == GameId(0, 0) (normalized from 0xFF00xxxx) + valid WorldSpaceId:
    // MUST evaluate truthy and valid!
    const CellIdComponent unnamedWildernessA{GameId{}, kTamriel, GridCellCoords(5, -3)};
    const CellIdComponent unnamedWildernessB{GameId{0u, 0x009911u}, kTamriel, GridCellCoords(6, -2)};
    const CellIdComponent whiterunCity{GameId{0u, 0x01A270u}, kWhiterunWorld, GridCellCoords(5, -3)};
    const CellIdComponent interiorFort{kFortGreymoor01, GameId{}, GridCellCoords(0, 0)};
    const CellIdComponent defaultInvalid{};

    REQUIRE_FALSE(defaultInvalid.IsValid());
    REQUIRE_FALSE(static_cast<bool>(defaultInvalid));

    REQUIRE(unnamedWildernessA.IsValid());
    REQUIRE(static_cast<bool>(unnamedWildernessA));
    REQUIRE(unnamedWildernessA.IsInRange(unnamedWildernessB, false));
    REQUIRE(unnamedWildernessB.IsInRange(unnamedWildernessA, false));

    // Different worldspace is NEVER in range
    REQUIRE_FALSE(unnamedWildernessA.IsInRange(whiterunCity, false));
    REQUIRE_FALSE(whiterunCity.IsInRange(unnamedWildernessA, false));

    // Interior vs Exterior is NEVER in range in either direction
    const CellIdComponent originExterior{GameId{}, kTamriel, GridCellCoords(0, 0)};
    REQUIRE_FALSE(originExterior.IsInRange(interiorFort, false));
    REQUIRE_FALSE(interiorFort.IsInRange(originExterior, false));
    REQUIRE(interiorFort.IsInRange(interiorFort, false));
}

#if !defined(RFAB_STANDALONE_TEST)
TEST_CASE("Stage 1 packets serialize WorldSpaceId and IsDragon cleanly", "[rfab][stage1][encoding]")
{
    using namespace TiltedPhoques;

    CharacterSpawnRequest spawn{};
    spawn.ServerId = 0x1234u;
    spawn.FormId = GameId{0u, 0x01038A8u};
    spawn.BaseId = GameId{0u, 0x001E7B0u};
    spawn.CellId = GameId{0u, 0x0009911u};
    spawn.WorldSpaceId = GameId{0u, 0x00003Cu};
    spawn.Position.x = 12345.5f;
    spawn.Position.y = -6789.25f;
    spawn.Position.z = 512.0f;
    spawn.Rotation.x = 0.25f;
    spawn.Rotation.y = 1.5f;
    spawn.ChangeFlags = 0x42u;
    spawn.PlayerId = 7u;
    spawn.IsDead = false;
    spawn.IsPlayer = false;
    spawn.IsWeaponDrawn = true;
    spawn.IsPlayerSummon = false;
    spawn.IsDragon = true;

    Buffer rawSpawnBuf(4096);
    Buffer::Writer spawnWriter(&rawSpawnBuf);
    spawn.Serialize(spawnWriter);

    Buffer::Reader spawnReader(&rawSpawnBuf);
    ServerMessageFactory serverFactory;
    auto extractedSpawn = serverFactory.Extract(spawnReader);
    REQUIRE(extractedSpawn != nullptr);
    const auto* pDecodedSpawn = dynamic_cast<const CharacterSpawnRequest*>(extractedSpawn.get());
    REQUIRE(pDecodedSpawn != nullptr);
    REQUIRE(*pDecodedSpawn == spawn);
    REQUIRE(pDecodedSpawn->WorldSpaceId == GameId(0u, 0x00003Cu));
    REQUIRE(pDecodedSpawn->IsDragon == true);

    ServerReferencesMoveRequest moveMsg{};
    moveMsg.Tick = 987654u;
    auto& refUpdate = moveMsg.Updates[0x1234u];
    refUpdate.UpdatedMovement.CellId = GameId{0u, 0x01F6CCu};
    refUpdate.UpdatedMovement.WorldSpaceId = GameId{};
    refUpdate.UpdatedMovement.Position.x = 200.f;
    refUpdate.UpdatedMovement.Position.y = -300.f;
    refUpdate.UpdatedMovement.Position.z = 64.f;

    Buffer rawMoveBuf(4096);
    Buffer::Writer moveWriter(&rawMoveBuf);
    moveMsg.Serialize(moveWriter);

    Buffer::Reader moveReader(&rawMoveBuf);
    auto extractedMove = serverFactory.Extract(moveReader);
    REQUIRE(extractedMove != nullptr);
    const auto* pDecodedMove = dynamic_cast<const ServerReferencesMoveRequest*>(extractedMove.get());
    REQUIRE(pDecodedMove != nullptr);
    REQUIRE(*pDecodedMove == moveMsg);
    REQUIRE(pDecodedMove->Updates.at(0x1234u).UpdatedMovement.CellId == GameId(0u, 0x01F6CCu));
}
#else
int main()
{
    for (const auto& entry : GetRfabTests())
    {
        entry.Fn();
        std::cout << "[PASS] " << entry.Name << "\n";
    }
    std::cout << "All " << GetRfabTests().size() << " Stage 1 test suites (" << GetAssertionCount()
              << " assertions) passed!\n";
    return 0;
}
#endif
