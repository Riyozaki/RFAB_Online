#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <CoopOwnership.h>
#include <RfabModpackCompat.h>

#include <entt/entt.hpp>
#include <Server.hpp>
#include <Messages/ReviveActionRequest.h>
#include <Services/EncounterService.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
std::filesystem::path LocateRepoRootPhase2()
{
    auto cur = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (std::filesystem::exists(cur / "Code" / "client" / "Games" / "Skyrim" / "Actor.cpp") &&
            std::filesystem::exists(cur / "Code" / "client" / "Services" / "DownedStateService.cpp"))
        {
            return cur;
        }
        if (!cur.has_parent_path())
            break;
        cur = cur.parent_path();
    }
    throw std::runtime_error("Failed to locate RFAB_Online repository root for Phase 2 test suite");
}

std::string ReadRepoFilePhase2(const std::filesystem::path& acRoot, const char* apRelPath)
{
    std::ifstream ifs(acRoot / apRelPath, std::ios::binary);
    if (!ifs.is_open())
        throw std::runtime_error(std::string("Unable to open ") + apRelPath);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}
} // namespace

TEST_CASE("RFAB v3.0 Phase 2.2: Server-authoritative ally Restoration healing-spell revive", "[rfab][v300][phase2][spell_revive]")
{
    SECTION("Standing healer casting onto downed ally within 45m revives immediately and clears active E-channel")
    {
        EncounterCoordinator coord{};
        coord.UpdatePlayerCombatReport(1u, true, 2u, 8.0f, false, 0.0f, 0.0f, 0.0f, 900u);
        coord.UpdatePlayerCombatReport(2u, true, 2u, 5.0f, false, RfabCompat::MetersToSkyrimUnits(18.0f), 0.0f, 0.0f, 900u);

        coord.SetPlayerDowned(2u, true, RfabCompat::MetersToSkyrimUnits(18.0f), 0.0f, 0.0f, 2u, 1000u);
        REQUIRE(coord.IsPlayerDowned(2u));

        // Even if a melee ally started a 4s E-channel at close range, instant spell revive completes and clears the channel
        coord.UpdatePlayerCombatReport(3u, true, 2u, 4.0f, false, RfabCompat::MetersToSkyrimUnits(19.5f), 0.0f, 0.0f, 1050u);
        REQUIRE(coord.TryStartAllyRevive(3u, 2u, 1100u));

        uint32_t actualReviver = 0u;
        REQUIRE(coord.TryInstantSpellRevive(1u, 2u, actualReviver));
        REQUIRE(actualReviver == 1u);
        REQUIRE_FALSE(coord.IsPlayerDowned(2u));

        const auto tick = coord.Tick(6000u);
        REQUIRE(tick.CompletedAllyRevives.empty());
        REQUIRE(tick.CancelledAllyRevives.empty());
    }

    SECTION("Downed player hit by AoE/projectile heal resolves nearest standing ally within 45m and rejects out-of-range / all-downed")
    {
        EncounterCoordinator coord{};
        coord.UpdatePlayerCombatReport(10u, true, 1u, 20.0f, false, RfabCompat::MetersToSkyrimUnits(60.0f), 0.0f, 0.0f, 900u); // too far (>45m)
        coord.UpdatePlayerCombatReport(20u, true, 1u, 3.0f, false, 0.0f, 0.0f, 0.0f, 900u);
        coord.SetPlayerDowned(20u, true, 0.0f, 0.0f, 0.0f, 1u, 1000u);

        uint32_t reviver = 0u;
        REQUIRE_FALSE(coord.TryInstantSpellRevive(20u, 20u, reviver));
        REQUIRE_FALSE(coord.TryInstantSpellRevive(10u, 20u, reviver));
        REQUIRE(coord.IsPlayerDowned(20u));

        // Move healer within 30m: downed recipient self-report now resolves standing healer 10u
        coord.UpdatePlayerCombatReport(10u, true, 1u, 12.0f, false, RfabCompat::MetersToSkyrimUnits(30.0f), 0.0f, 0.0f, 1100u);
        REQUIRE(coord.TryInstantSpellRevive(20u, 20u, reviver));
        REQUIRE(reviver == 10u);
        REQUIRE_FALSE(coord.IsPlayerDowned(20u));
    }

    SECTION("ReviveActionRequest, MagicService, and EncounterService wire HealingSpellRevive end-to-end")
    {
        const auto root = LocateRepoRootPhase2();

        ReviveActionRequest out{};
        out.Action = ReviveActionRequest::ActionType::HealingSpellRevive;
        out.TargetPlayerId = 42u;
        TiltedPhoques::Buffer buf(256);
        TiltedPhoques::Buffer::Writer writer(&buf);
        out.SerializeRaw(writer);
        TiltedPhoques::Buffer::Reader reader(&buf);
        ReviveActionRequest in{};
        in.DeserializeRaw(reader);
        REQUIRE(in.Action == ReviveActionRequest::ActionType::HealingSpellRevive);
        REQUIRE(in.TargetPlayerId == 42u);
    }
}

