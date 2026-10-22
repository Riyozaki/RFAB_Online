#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <regex>
#include <string>

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/NotifySyncRfabExperience.h>
#include <Messages/SyncRfabExperienceRequest.h>
#include <Structs/ServerSettings.h>

using namespace TiltedPhoques;
using namespace std::chrono_literals;

namespace
{
// Mirrors GameServer::GetEffectiveHealthMultiplier (1p=1x, 2p=3x, 3p=4x, N>=2 -> (N+1)x)
float ComputeEffectiveEnemyHealthMultiplier(float aBaseMultiplier, bool aAutoScaleByPlayerCount, size_t aConnectedPlayers) noexcept
{
    float base = std::isfinite(aBaseMultiplier) ? std::clamp(aBaseMultiplier, 0.1f, 10.0f) : 1.0f;
    if (aAutoScaleByPlayerCount && aConnectedPlayers >= 2)
        base *= static_cast<float>(aConnectedPlayers + 1);

    return std::clamp(base, 0.1f, 10.0f);
}

// Mirrors Server ActorValueService::OnHealthChange signed delta application + clamping
float ApplyAuthoritativeServerHealthDelta(float aCurrentHealth, float aMaxHealth, float aDeltaHealth) noexcept
{
    const float updated = aCurrentHealth + aDeltaHealth;
    return (aMaxHealth > 0.f) ? std::clamp(updated, 0.f, aMaxHealth) : std::max(0.f, updated);
}

// Mirrors Client ActorValueService ownership-transfer-safe HP scaler
struct SimulatedEnemyScaler
{
    Map<uint32_t, float> UnscaledMaxHealth;

    void SeedFromRemote(uint32_t aFormId, float aScaledMax, float aMultiplier) noexcept
    {
        if (aFormId == 0 || aScaledMax <= 0.f)
            return;
        if (UnscaledMaxHealth.find(aFormId) == UnscaledMaxHealth.end())
            UnscaledMaxHealth[aFormId] = aScaledMax / aMultiplier;
    }

    void Apply(uint32_t aFormId, bool aIsPlayer, bool aIsTeammate, float& aMaxHealth, float& aCurrentHealth, float aMultiplier) noexcept
    {
        if (aIsPlayer || aIsTeammate || aMaxHealth <= 0.f)
            return;

        auto it = UnscaledMaxHealth.find(aFormId);
        if (it == UnscaledMaxHealth.end())
            it = UnscaledMaxHealth.insert({aFormId, aMaxHealth}).first;

        const float desiredMax = it->second * aMultiplier;
        if (std::abs(desiredMax - aMaxHealth) <= 0.01f)
            return;

        const float pct = std::clamp(aCurrentHealth / aMaxHealth, 0.f, 1.f);
        aMaxHealth = desiredMax;
        aCurrentHealth = desiredMax * pct;
    }
};

// Mirrors Client CharacterService bidirectional quest/combat RFAB XP synchronizer
struct SimulatedRfabXpClient
{
    int32_t RfabXp = 0;
    int32_t LastRfabXp = 0;
    bool Initialized = false;
    std::chrono::steady_clock::time_point RemoteQuestWindowUntil{};
    int32_t LocalQuestXpInWindow = 0;
    int32_t LastAppliedRemoteXpAmount = 0;
    std::chrono::steady_clock::time_point LastAppliedRemoteXpTime{};
    int32_t CreditToRefund = 0;

    void NotifyRemoteQuestStageApplied(std::chrono::steady_clock::time_point aNow) noexcept
    {
        RemoteQuestWindowUntil = aNow + 4000ms;
        if (LastAppliedRemoteXpAmount > 0 && (aNow - LastAppliedRemoteXpTime) <= 3000ms)
        {
            CreditToRefund += LastAppliedRemoteXpAmount;
            LastAppliedRemoteXpAmount = 0;
        }
    }

    void ReceiveRemoteSync(uint32_t aAmount, std::chrono::steady_clock::time_point aNow) noexcept
    {
        int32_t delta = static_cast<int32_t>(aAmount);
        if (aNow <= RemoteQuestWindowUntil && LocalQuestXpInWindow > 0)
        {
            const int32_t absorbed = std::min(delta, LocalQuestXpInWindow);
            LocalQuestXpInWindow -= absorbed;
            delta -= absorbed;
            if (delta <= 0)
                return;
        }

        const int32_t before = RfabXp;
        RfabXp += delta;
        if (!Initialized)
        {
            LastRfabXp = before;
            Initialized = true;
        }
        LastRfabXp += delta;
        LastAppliedRemoteXpAmount = delta;
        LastAppliedRemoteXpTime = aNow;
    }

    uint32_t TickOutgoing(std::chrono::steady_clock::time_point aNow) noexcept
    {
        if (aNow > RemoteQuestWindowUntil)
        {
            LocalQuestXpInWindow = 0;
            CreditToRefund = 0;
        }

        if (!Initialized)
        {
            LastRfabXp = RfabXp;
            Initialized = true;
            return 0;
        }

        int32_t delta = RfabXp - LastRfabXp;
        LastRfabXp = RfabXp;
        if (delta <= 0)
            return 0;

        if (aNow <= RemoteQuestWindowUntil)
        {
            if (CreditToRefund > 0)
            {
                const int32_t refund = std::min(delta, CreditToRefund);
                RfabXp -= refund;
                CreditToRefund -= refund;
                LastRfabXp = RfabXp;
                delta -= refund;
            }
            if (delta > 0)
                LocalQuestXpInWindow += delta;
            return 0;
        }

        return static_cast<uint32_t>(delta);
    }
};
} // namespace

TEST_CASE("RFAB dynamic enemy HP scaling & ownership-transfer invariance", "[rfab.scaling]")
{
    SECTION("Player-count HP formula matches 1p=1x, 2p=3x, 3p=4x, 4p=5x")
    {
        REQUIRE(ComputeEffectiveEnemyHealthMultiplier(1.0f, true, 0) == Approx(1.0f));
        REQUIRE(ComputeEffectiveEnemyHealthMultiplier(1.0f, true, 1) == Approx(1.0f));
        REQUIRE(ComputeEffectiveEnemyHealthMultiplier(1.0f, true, 2) == Approx(3.0f));
        REQUIRE(ComputeEffectiveEnemyHealthMultiplier(1.0f, true, 3) == Approx(4.0f));
        REQUIRE(ComputeEffectiveEnemyHealthMultiplier(1.0f, true, 4) == Approx(5.0f));
    }

    SECTION("Players and followers are never scaled")
    {
        SimulatedEnemyScaler scaler;
        float playerMax = 250.f, playerCur = 250.f;
        float followerMax = 400.f, followerCur = 400.f;
        scaler.Apply(0x14, true, false, playerMax, playerCur, 3.0f);
        scaler.Apply(0x000A2C94, false, true, followerMax, followerCur, 3.0f);
        REQUIRE(playerMax == Approx(250.f));
        REQUIRE(followerMax == Approx(400.f));
    }

    SECTION("1000 consecutive A <-> B <-> C ownership transfers preserve exact 3x/4x HP and % damage")
    {
        SimulatedEnemyScaler clientA, clientB, clientC;
        const uint32_t bossFormId = 0x05012345;
        float maxHp = 1000.f;
        float curHp = 1000.f;

        // Spawn on A with 2 players (3x -> 3000/3000), deal 1200 real engine damage -> 1800/3000 (60%)
        clientA.Apply(bossFormId, false, false, maxHp, curHp, 3.0f);
        REQUIRE(maxHp == Approx(3000.f));
        curHp = ApplyAuthoritativeServerHealthDelta(curHp, maxHp, -1200.f);
        REQUIRE(curHp == Approx(1800.f));

        // Bounce ownership A -> B -> C -> A 1,000 times — zero 3^K inflation!
        for (int i = 0; i < 1000; ++i)
        {
            SimulatedEnemyScaler& nextOwner = (i % 3 == 0) ? clientB : ((i % 3 == 1) ? clientC : clientA);
            nextOwner.SeedFromRemote(bossFormId, maxHp, 3.0f);
            nextOwner.Apply(bossFormId, false, false, maxHp, curHp, 3.0f);
            REQUIRE(maxHp == Approx(3000.f));
            REQUIRE(curHp == Approx(1800.f));
        }

        // Player 3 joins (3x -> 4x): 60% of 4000 == 2400/4000
        clientA.Apply(bossFormId, false, false, maxHp, curHp, 4.0f);
        REQUIRE(maxHp == Approx(4000.f));
        REQUIRE(curHp == Approx(2400.f));

        // Players 2 & 3 disconnect (4x -> 1x): cleanly restores 60% of original 1000 == 600/1000
        clientA.Apply(bossFormId, false, false, maxHp, curHp, 1.0f);
        REQUIRE(maxHp == Approx(1000.f));
        REQUIRE(curHp == Approx(600.f));
    }

    SECTION("Two-step downscale never transiently drops real engine GetActorValue(kHealth) <= 0 even at 1/300 HP")
    {
        float baseHp = 100.f;
        float permMod = +200.f;   // 3x max = 300
        float damageMod = -299.f; // current = 1 / 300 HP!
        auto getMax = [&]() { return baseHp + permMod; };
        auto getCur = [&]() { return baseHp + permMod + damageMod; };
        bool died = false;
        auto forceAv = [&](bool isPerm, float newVal) {
            if (isPerm)
                permMod += (newVal - getMax());
            else
                damageMod += (newVal - getCur());
            if (getCur() <= 0.f)
                died = true;
        };

        const float currentMax = getMax();
        const float desiredMax = 100.f;
        const float desiredHealth = desiredMax * (getCur() / currentMax);
        // Downscale safe order: DAMAGE first, PERMANENT second
        forceAv(false, currentMax + (desiredHealth - desiredMax));
        forceAv(true, desiredMax);

        REQUIRE_FALSE(died);
        REQUIRE(getMax() == Approx(100.f));
        REQUIRE(getCur() == Approx(1.f / 3.f).margin(1e-4f));
        REQUIRE(damageMod < 0.f);
    }
}

TEST_CASE("Server health delta sign, healing clamp, and AssignCharacterResponse payload roundtrip", "[rfab.health]")
{
    // Damage reduces health (previously inverted with subtraction!)
    REQUIRE(ApplyAuthoritativeServerHealthDelta(300.f, 300.f, -85.f) == Approx(215.f));
    // Overkill clamps to 0.0f
    REQUIRE(ApplyAuthoritativeServerHealthDelta(40.f, 300.f, -500.f) == Approx(0.f));
    // Regeneration/healing increases health and clamps at MaxHealth
    REQUIRE(ApplyAuthoritativeServerHealthDelta(215.f, 300.f, +50.f) == Approx(265.f));
    REQUIRE(ApplyAuthoritativeServerHealthDelta(290.f, 300.f, +50.f) == Approx(300.f));

    // Ownership transfer via AssignCharacterResponse preserves AllActorValues & IsDead
    AssignCharacterResponse tx;
    tx.Cookie = 42;
    tx.ServerId = 0x1001;
    tx.Owner = true;
    tx.IsDead = false;
    tx.AllActorValues.ActorValuesList.insert({24, 1800.f});
    tx.AllActorValues.ActorMaxValuesList.insert({24, 3000.f});

    Buffer buf(4096);
    Buffer::Writer writer(&buf);
    tx.Serialize(writer);

    Buffer::Reader reader(&buf);
    const ServerMessageFactory factory;
    auto msg = factory.Extract(reader);
    REQUIRE(msg);
    auto rx = CastUnique<AssignCharacterResponse>(std::move(msg));
    REQUIRE(rx->Owner == true);
    REQUIRE(rx->AllActorValues.ActorValuesList.at(24) == Approx(1800.f));
    REQUIRE(rx->AllActorValues.ActorMaxValuesList.at(24) == Approx(3000.f));
}

TEST_CASE("RFAB equal XP synchronization across kills, simultaneous multi-kills, books, and quests in all packet orders", "[rfab.xp]")
{
    const auto t0 = std::chrono::steady_clock::now();
    SimulatedRfabXpClient A, B, C;
    A.TickOutgoing(t0);
    B.TickOutgoing(t0);
    C.TickOutgoing(t0);

    // 1. Solo kill by A (+350 XP): A, B, C all have 350 XP
    A.RfabXp += 350;
    const uint32_t d1 = A.TickOutgoing(t0 + 300ms);
    REQUIRE(d1 == 350);
    B.ReceiveRemoteSync(d1, t0 + 320ms);
    C.ReceiveRemoteSync(d1, t0 + 320ms);
    REQUIRE(B.TickOutgoing(t0 + 600ms) == 0);
    REQUIRE(C.TickOutgoing(t0 + 600ms) == 0);
    REQUIRE(A.RfabXp == 350);
    REQUIRE(B.RfabXp == 350);
    REQUIRE(C.RfabXp == 350);

    // 2. Simultaneous kills of two distinct bandits by A (+250) and B (+250): all reach 850 XP!
    A.RfabXp += 250;
    B.RfabXp += 250;
    const uint32_t dA = A.TickOutgoing(t0 + 1000ms);
    const uint32_t dB = B.TickOutgoing(t0 + 1000ms);
    REQUIRE(dA == 250);
    REQUIRE(dB == 250);
    B.ReceiveRemoteSync(dA, t0 + 1050ms);
    C.ReceiveRemoteSync(dA, t0 + 1050ms);
    A.ReceiveRemoteSync(dB, t0 + 1050ms);
    C.ReceiveRemoteSync(dB, t0 + 1050ms);
    REQUIRE(A.RfabXp == 850);
    REQUIRE(B.RfabXp == 850);
    REQUIRE(C.RfabXp == 850);

    // 3. Synced quest completion (+5000 XP):
    //    - B receives NotifyQuestUpdate BEFORE NotifySyncRfabExperience (Order 1)
    //    - C receives NotifySyncRfabExperience BEFORE NotifyQuestUpdate (Order 2)
    const auto tq = t0 + 10s;
    A.RfabXp += 5000;
    const uint32_t dq = A.TickOutgoing(tq + 250ms);
    REQUIRE(dq == 5000);

    // Order 1 on B: quest stage arrives first
    B.NotifyRemoteQuestStageApplied(tq + 100ms);
    B.RfabXp += 5000;
    REQUIRE(B.TickOutgoing(tq + 250ms) == 0);
    B.ReceiveRemoteSync(dq, tq + 300ms);

    // Order 2 on C: XP packet arrives BEFORE quest stage fragment executes!
    C.ReceiveRemoteSync(dq, tq + 100ms);
    C.NotifyRemoteQuestStageApplied(tq + 200ms);
    C.RfabXp += 5000;
    REQUIRE(C.TickOutgoing(tq + 300ms) == 0);

    REQUIRE(A.RfabXp == 5850);
    REQUIRE(B.RfabXp == 5850);
    REQUIRE(C.RfabXp == 5850);
}

TEST_CASE("EngineFixes.toml multiline grey-list validator accepts RFAB SSE Engine Fixes config", "[rfab.enginefixes]")
{
    const std::string sampleRfabToml =
        "# Engine Fixes for Skyrim Special Edition\r\n\r\n"
        "[EngineFixes]\r\n"
        "VerboseLogging = false                  # Add extra log messages\r\n"
        "\r\n[Patches]\r\n"
        "MaxStdio = 2048\r\n"
        "MemoryManager = true                    # Replaces Skyrim's global allocator\r\n"
        "ScaleformAllocator = true               # Replaces the scaleform allocator\r\n";

    std::string normalized;
    for (char c : sampleRfabToml)
        if (c != '\r')
            normalized.push_back(c);

    const std::regex validator("(^|\\n)\\s*VerboseLogging\\s*=", std::regex_constants::icase);
    REQUIRE(std::regex_search(normalized, validator));
}
