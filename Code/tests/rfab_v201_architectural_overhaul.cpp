#include <catch2/catch.hpp>

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <common/CoopAnimationState.h>
#include <common/CoopWorldView.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/NotifyExecuteLoad.h>

#include <cstdint>
#include <cstring>
#include <new>

namespace
{
struct MockRefCountedRef
{
    int32_t RefCount{0};
    int32_t DecCallsOnGarbage{0};

    void IncRefHandle() noexcept { ++RefCount; }
    void DecRefHandle() noexcept { --RefCount; }
};

// Exact mirror of Primitives.h GamePtr<T> memory-lifetime contract
template <class T>
struct TestGamePtr
{
    TestGamePtr() noexcept = default;
    TestGamePtr(std::nullptr_t) noexcept {}
    explicit TestGamePtr(T* apPointer) noexcept
        : m_pPointer(apPointer)
    {
        if (m_pPointer)
            m_pPointer->IncRefHandle();
    }
    TestGamePtr(TestGamePtr&& aRhs) noexcept
        : m_pPointer(aRhs.m_pPointer)
    {
        aRhs.m_pPointer = nullptr;
    }
    TestGamePtr& operator=(TestGamePtr&& aRhs) noexcept
    {
        if (this != &aRhs)
        {
            Release();
            m_pPointer = aRhs.m_pPointer;
            aRhs.m_pPointer = nullptr;
        }
        return *this;
    }
    TestGamePtr& operator=(std::nullptr_t) noexcept
    {
        Release();
        return *this;
    }
    ~TestGamePtr() noexcept { Release(); }

    void Release() noexcept
    {
        if (m_pPointer)
        {
            m_pPointer->DecRefHandle();
            m_pPointer = nullptr;
        }
    }

    T* Get() const noexcept { return m_pPointer; }

private:
    T* m_pPointer{nullptr};
};

TestGamePtr<MockRefCountedRef> SimulateCreateInCellNonNrvo(MockRefCountedRef* apNpc, bool aEarlyFail) noexcept
{
    if (aEarlyFail)
        return nullptr;

    TestGamePtr<MockRefCountedRef> pActor(apNpc);
    return pActor;
}
} // namespace

TEST_CASE("GamePtr move/release hardening prevents uninitialized DecRefHandle on non-NRVO returns", "[rfab][v201][gameptr]")
{
    MockRefCountedRef liveRef{};
    REQUIRE(liveRef.RefCount == 0);

    SECTION("Placement-new over dirty stack memory initializes m_pPointer to nullptr before move")
    {
        alignas(TestGamePtr<MockRefCountedRef>) unsigned char dirtyStorage[sizeof(TestGamePtr<MockRefCountedRef>)];
        std::memset(dirtyStorage, 0xFC, sizeof(dirtyStorage));

        auto* pConstructed = new (dirtyStorage) TestGamePtr<MockRefCountedRef>(SimulateCreateInCellNonNrvo(&liveRef, false));
        REQUIRE(pConstructed->Get() == &liveRef);
        REQUIRE(liveRef.RefCount == 1);

        pConstructed->~TestGamePtr();
        REQUIRE(liveRef.RefCount == 0);
    }

    SECTION("Early return nullptr on non-NRVO function leaves zero ref-count leaks or garbage dereferences")
    {
        auto pNull = SimulateCreateInCellNonNrvo(&liveRef, true);
        REQUIRE(pNull.Get() == nullptr);
        REQUIRE(liveRef.RefCount == 0);
    }

    SECTION("Move-assignment releases existing managed handle before adopting RHS")
    {
        MockRefCountedRef secondRef{};
        {
            TestGamePtr<MockRefCountedRef> pFirst(&liveRef);
            TestGamePtr<MockRefCountedRef> pSecond(&secondRef);
            REQUIRE(liveRef.RefCount == 1);
            REQUIRE(secondRef.RefCount == 1);

            pFirst = std::move(pSecond);
            REQUIRE(liveRef.RefCount == 0);
            REQUIRE(secondRef.RefCount == 1);
            REQUIRE(pSecond.Get() == nullptr);
        }
        REQUIRE(secondRef.RefCount == 0);
    }
}

TEST_CASE("NormalizePostReviveActorState clears both kEssentialDown (7) and kBleedout (8) after Fort Greymoor party wipe", "[rfab][v201][downed]")
{
    SECTION("Clears kEssentialDown (7) left by Skyrim essential knockdown on 0x14 and remote clones")
    {
        uint32_t flags1 = (CoopAnimationState::kLifeStateEssentialDown << CoopAnimationState::kLifeStateShift) |
                          (3u << CoopAnimationState::kKnockStateShift);
        uint32_t flags2 = CoopAnimationState::kStaggeredMask | CoopAnimationState::kRecoilMask;

        const bool hadDownedPosture = CoopAnimationState::NormalizePostReviveActorState(flags1, flags2);
        REQUIRE(hadDownedPosture);
        REQUIRE(CoopAnimationState::GetLifeState(flags1) == CoopAnimationState::kLifeStateAlive);
        REQUIRE((flags1 & CoopAnimationState::kKnockStateMask) == 0u);
        REQUIRE((flags2 & (CoopAnimationState::kStaggeredMask | CoopAnimationState::kRecoilMask)) == 0u);
    }

    SECTION("Clears kBleedout (8) and kDying (1) while leaving genuinely dead actors untouched")
    {
        uint32_t bleedFlags1 = (CoopAnimationState::kLifeStateBleedout << CoopAnimationState::kLifeStateShift);
        uint32_t bleedFlags2 = 0u;
        REQUIRE(CoopAnimationState::NormalizePostReviveActorState(bleedFlags1, bleedFlags2));
        REQUIRE(CoopAnimationState::GetLifeState(bleedFlags1) == CoopAnimationState::kLifeStateAlive);

        uint32_t dyingFlags1 = (CoopAnimationState::kLifeStateDying << CoopAnimationState::kLifeStateShift);
        uint32_t dyingFlags2 = 0u;
        REQUIRE(CoopAnimationState::NormalizePostReviveActorState(dyingFlags1, dyingFlags2));
        REQUIRE(CoopAnimationState::GetLifeState(dyingFlags1) == CoopAnimationState::kLifeStateAlive);

        uint32_t deadFlags1 = (CoopAnimationState::kLifeStateDead << CoopAnimationState::kLifeStateShift);
        uint32_t deadFlags2 = 0u;
        REQUIRE_FALSE(CoopAnimationState::NormalizePostReviveActorState(deadFlags1, deadFlags2));
        REQUIRE(CoopAnimationState::GetLifeState(deadFlags1) == CoopAnimationState::kLifeStateDead);
    }
}

TEST_CASE("CoopWorldView CancelledCookieTracker and 350ms door-transition grace eliminate ghost churn", "[rfab][v201][worldview]")
{
    SECTION("CancelledCookieTracker ring buffer records, consumes once, and wraps cleanly")
    {
        CoopWorldView::CancelledCookieTracker<8> tracker{};
        tracker.RecordCancelled(42u);
        tracker.RecordCancelled(42u); // idempotent duplicate
        REQUIRE(tracker.ConsumeIfCancelled(42u));
        REQUIRE_FALSE(tracker.ConsumeIfCancelled(42u));

        for (uint32_t c = 100; c < 112; ++c)
            tracker.RecordCancelled(c);

        // Oldest 4 entries (100..103) rotated out; newest 8 (104..111) present
        REQUIRE_FALSE(tracker.ConsumeIfCancelled(100u));
        REQUIRE(tracker.ConsumeIfCancelled(104u));
        REQUIRE(tracker.ConsumeIfCancelled(111u));
    }

    SECTION("350ms door-transition grace holds remote human player materialized when peer follows through same door")
    {
        CoopWorldView::ObserverState obs{};
        obs.EngineReady = true;
        obs.Location = CoopWorldView::WorldOrCell::MakeExterior(GameId{0, 0x3C}, GridCellCoords(4, -2));
        obs.PosX = 18000.0f;
        obs.PosY = -6000.0f;

        CoopWorldView::RemoteViewQuery playerEnteredFort{};
        playerEnteredFort.Location = CoopWorldView::WorldOrCell::MakeInterior(GameId{0, 0x1527C});
        playerEnteredFort.IsPlayer = true;
        playerEnteredFort.CurrentlyMaterialized = true;
        playerEnteredFort.TargetCellLoadedInEngine = false;
        playerEnteredFort.NowSteadyMs = 1000ULL;
        playerEnteredFort.DematerializeGraceUntilMs = 1000ULL + CoopWorldView::kRemotePlayerDoorTransitionGraceMs;

        // Within 350ms grace window: held materialized so rapid peer door entry avoids Delete+Create churn
        REQUIRE(CoopWorldView::EvaluateViewDecision(obs, playerEnteredFort) == CoopWorldView::ViewDecision::kKeepMaterialized);

        // Once grace window expires and observer stayed outside: cleanly dematerialized
        playerEnteredFort.NowSteadyMs = 1350ULL;
        REQUIRE(CoopWorldView::EvaluateViewDecision(obs, playerEnteredFort) == CoopWorldView::ViewDecision::kDematerialize);

        // Non-player NPCs never linger across incompatible cells
        CoopWorldView::RemoteViewQuery npcEnteredFort = playerEnteredFort;
        npcEnteredFort.IsPlayer = false;
        npcEnteredFort.NowSteadyMs = 1100ULL;
        REQUIRE(CoopWorldView::EvaluateViewDecision(obs, npcEnteredFort) == CoopWorldView::ViewDecision::kDematerialize);
    }
}

TEST_CASE("NotifyActorTeleport wire round-trip preserves rollback spatial state for exterior Fort Greymoor NPCs", "[rfab][v201][rollback]")
{
    NotifyActorTeleport outgoing{};
    outgoing.FormId = GameId{0, 0x0E5F72};
    outgoing.WorldSpaceId = GameId{0, 0x00003C};
    outgoing.CellId = GameId{0, 0x00994E};
    outgoing.Position = {16850.5f, -7420.25f, 412.0f};

    uint8_t scratch[256]{};
    TiltedPhoques::Buffer buffer(scratch, sizeof(scratch));
    TiltedPhoques::Buffer::Writer writer(&buffer);
    outgoing.SerializeRaw(writer);

    TiltedPhoques::Buffer::Reader reader(&buffer);
    NotifyActorTeleport decoded{};
    decoded.DeserializeRaw(reader);

    REQUIRE(decoded.FormId == outgoing.FormId);
    REQUIRE(decoded.WorldSpaceId == outgoing.WorldSpaceId);
    REQUIRE(decoded.CellId == outgoing.CellId);
    REQUIRE(Approx(decoded.Position.x).margin(1.0f) == outgoing.Position.x);
    REQUIRE(Approx(decoded.Position.y).margin(1.0f) == outgoing.Position.y);
    REQUIRE(Approx(decoded.Position.z).margin(1.0f) == outgoing.Position.z);
}
