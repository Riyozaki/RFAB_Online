#include <catch2/catch.hpp>

#include <CoopOwnership.h>
#include <CoopPacketSafety.h>
#include <RfabModpackCompat.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct MockScratchAllocator
{
    std::vector<uint8_t> Storage;
    size_t Offset{0};
    uint32_t ResetCount{0};

    explicit MockScratchAllocator(size_t aCapacity = 4096)
        : Storage(aCapacity, 0xCDu)
    {
    }

    uint8_t* Allocate(size_t aBytes)
    {
        if (Offset + aBytes > Storage.size())
            throw std::bad_alloc{};
        uint8_t* pOut = Storage.data() + Offset;
        Offset += aBytes;
        return pOut;
    }

    void Reset() noexcept
    {
        std::memset(Storage.data(), 0xFEu, Offset);
        Offset = 0;
        ++ResetCount;
    }
};

std::filesystem::path LocateRepoRootForV300Phase1Audit()
{
    std::error_code ec;
    auto searchUpwards = [&](std::filesystem::path cursor) -> std::filesystem::path
    {
        for (int depth = 0; depth < 8 && !cursor.empty(); ++depth)
        {
            if (std::filesystem::exists(cursor / "Code" / "client" / "World.h", ec) &&
                std::filesystem::exists(cursor / "Code" / "common" / "CoopPacketSafety.h", ec))
            {
                return cursor;
            }
            const auto parent = cursor.parent_path();
            if (parent == cursor)
                break;
            cursor = parent;
        }
        return {};
    };

    if (const auto fromCwd = searchUpwards(std::filesystem::current_path(ec)); !fromCwd.empty())
        return fromCwd;

    return searchUpwards(std::filesystem::path(__FILE__).parent_path());
}

std::string LoadV300RepoFile(const std::filesystem::path& acRoot, const char* apRelPath)
{
    std::ifstream ifs(acRoot / apRelPath, std::ios::binary);
    REQUIRE(ifs.good());
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}
} // namespace

TEST_CASE("v3.0 Phase 1: Reentrant ScopedScratchFrame protects outer packet buffer during nested Send()", "[rfab][v300][phase1]")
{
    SECTION("Nested Send() allocates linearly without clobbering outer buffer and resets once at depth 0")
    {
        MockScratchAllocator scratch(1024);
        uint32_t depth = 0;
        {
            CoopPacketSafety::ScopedScratchFrame<MockScratchAllocator> outerFrame(scratch, depth);
            REQUIRE(depth == 1u);
            REQUIRE(scratch.ResetCount == 0u);

            uint8_t* pOuterPacket = scratch.Allocate(64);
            for (size_t i = 0; i < 64; ++i)
                pOuterPacket[i] = static_cast<uint8_t>(i + 1u);

            {
                CoopPacketSafety::ScopedScratchFrame<MockScratchAllocator> innerFrame(scratch, depth);
                REQUIRE(depth == 2u);

                uint8_t* pInnerPacket = scratch.Allocate(32);
                std::memset(pInnerPacket, 0xAAu, 32);

                // Outer buffer MUST still hold its exact serialized payload before innerFrame unwinds:
                for (size_t i = 0; i < 64; ++i)
                    REQUIRE(pOuterPacket[i] == static_cast<uint8_t>(i + 1u));
            }

            // Inner frame exited, but outer Send() is STILL in flight: Reset() must NOT have fired!
            REQUIRE(depth == 1u);
            REQUIRE(scratch.ResetCount == 0u);
            REQUIRE(scratch.Offset == 96u);
            for (size_t i = 0; i < 64; ++i)
                REQUIRE(pOuterPacket[i] == static_cast<uint8_t>(i + 1u));
        }

        // Outermost frame exited -> single clean Reset()!
        REQUIRE(depth == 0u);
        REQUIRE(scratch.ResetCount == 1u);
        REQUIRE(scratch.Offset == 0u);
    }

    SECTION("Exception unwinding inside nested Send() restores depth == 0 and resets scratch arena")
    {
        MockScratchAllocator scratch(1024);
        uint32_t depth = 0;
        auto throwingNestedSend = [&]()
        {
            CoopPacketSafety::ScopedScratchFrame<MockScratchAllocator> outerFrame(scratch, depth);
            (void)scratch.Allocate(48);
            {
                CoopPacketSafety::ScopedScratchFrame<MockScratchAllocator> innerFrame(scratch, depth);
                (void)scratch.Allocate(24);
                throw std::runtime_error("simulated serialization error");
            }
        };

        REQUIRE_THROWS_AS(throwingNestedSend(), std::runtime_error);
        REQUIRE(depth == 0u);
        REQUIRE(scratch.ResetCount == 1u);
        REQUIRE(scratch.Offset == 0u);
    }
}

TEST_CASE("v3.0 Phase 1: Simulation Authority != Combat Target invariants in CoopOwnership", "[rfab][v300][phase1]")
{
    CoopOwnership::PolicyConfig policy{};
    policy.MinVoluntaryHoldMs = 1500;

    // Connected in-range owner going downed does NOT let peers yank AI simulation mid-combat:
    REQUIRE(CoopOwnership::EvaluateVoluntaryClaim(
                /*aCurrentOwnerId=*/10u,
                /*aChallengerId=*/20u,
                /*aLastChangedAtMs=*/5000ull,
                /*aNowMs=*/5300ull,
                /*aIsPlayerActor=*/false,
                /*aIsPlayerSummon=*/false,
                /*aOwnerValidInRange=*/true,
                /*aOwnerDowned=*/true,
                /*aChallengerDowned=*/false,
                policy) == CoopOwnership::ClaimDecision::kRejectHoldWindowActive);

    // Out-of-range / disconnected owner transfers immediately without waiting for hold window:
    REQUIRE(CoopOwnership::EvaluateVoluntaryClaim(
                /*aCurrentOwnerId=*/10u,
                /*aChallengerId=*/20u,
                /*aLastChangedAtMs=*/5000ull,
                /*aNowMs=*/5100ull,
                /*aIsPlayerActor=*/false,
                /*aIsPlayerSummon=*/false,
                /*aOwnerValidInRange=*/false,
                /*aOwnerDowned=*/false,
                /*aChallengerDowned=*/false,
                policy) == CoopOwnership::ClaimDecision::kAccept);

    // Downed challenger, player summon, and player avatar remain strictly protected:
    REQUIRE(CoopOwnership::EvaluateVoluntaryClaim(10u, 20u, 1000ull, 9000ull, false, false, true, false, true, policy) ==
            CoopOwnership::ClaimDecision::kRejectChallengerDowned);
    REQUIRE(CoopOwnership::EvaluateVoluntaryClaim(10u, 20u, 1000ull, 9000ull, false, true, false, false, false, policy) ==
            CoopOwnership::ClaimDecision::kRejectSummonPinned);
    REQUIRE(CoopOwnership::EvaluateVoluntaryClaim(10u, 20u, 1000ull, 9000ull, true, false, false, false, false, policy) ==
            CoopOwnership::ClaimDecision::kRejectPlayerAvatar);

    // Active-combat / non-leader claim rejection code is explicitly distinct:
    REQUIRE(CoopOwnership::ClaimDecision::kRejectOwnerActiveInCombatOrNonLeader != CoopOwnership::ClaimDecision::kAccept);
}

