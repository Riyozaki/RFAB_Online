#include <catch2/catch.hpp>

#include <common/Session/Lifecycle.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// v4.1.0 architecture step 2: one owner of the client's session and world lifecycle
// (common/Session/Lifecycle.h). The machine and the dispatcher are portable and tested directly here;
// the client wiring (World.cpp registration, the transport / save-load feeds, the removed ad-hoc
// resets) is audited against the sources.

namespace
{
using namespace Session;

struct Delivery
{
    ParticipantId Who;
    Transition What;
};

// Records every transition into a log shared by all participants, so the test sees the global
// delivery order. An optional hook lets a participant feed the lifecycle from inside a handler.
class Recorder final : public IParticipant
{
public:
    Recorder(ParticipantId aId, std::vector<Delivery>& aLog) noexcept
        : m_id(aId)
        , m_log(aLog)
    {
    }

    std::function<void(const Transition&)> Hook;

    void OnSessionBegin(const Transition& acTransition) noexcept override { Record(acTransition); }
    void OnSessionEnd(const Transition& acTransition) noexcept override { Record(acTransition); }
    void OnWorldLoadBegin(const Transition& acTransition) noexcept override { Record(acTransition); }
    void OnWorldLoadEnd(const Transition& acTransition) noexcept override { Record(acTransition); }

private:
    void Record(const Transition& acTransition) noexcept
    {
        m_log.push_back({m_id, acTransition});
        if (Hook)
            Hook(acTransition);
    }

    ParticipantId m_id;
    std::vector<Delivery>& m_log;
};

struct Rig
{
    Lifecycle Cycle;
    std::vector<Delivery> Log;
    std::vector<std::unique_ptr<Recorder>> Participants;

    Rig()
    {
        for (const auto& info : kParticipantTable)
        {
            Participants.push_back(std::make_unique<Recorder>(info.Id, Log));
            REQUIRE(Cycle.Register(info.Id, *Participants.back()));
        }
    }

    Recorder& Of(ParticipantId aId) { return *Participants[static_cast<size_t>(aId)]; }

    void Online(uint32_t aPlayerId = 7)
    {
        Cycle.OnTransportUp();
        Cycle.OnAuthenticated(aPlayerId);
        Log.clear();
    }

    // The signals delivered, collapsed to one entry per transition (each reaches every participant).
    [[nodiscard]] std::vector<Signal> Signals() const
    {
        std::vector<Signal> out;
        for (size_t i = 0; i < Log.size(); i += kParticipantCount)
            out.push_back(Log[i].What.Kind);
        return out;
    }

    [[nodiscard]] std::vector<ParticipantId> OrderOf(size_t aTransitionIndex) const
    {
        std::vector<ParticipantId> out;
        for (size_t i = 0; i < kParticipantCount; ++i)
            out.push_back(Log[aTransitionIndex * kParticipantCount + i].Who);
        return out;
    }

    [[nodiscard]] const Transition& Nth(size_t aTransitionIndex) const { return Log[aTransitionIndex * kParticipantCount].What; }
};

[[nodiscard]] size_t Index(ParticipantId aId) noexcept
{
    return static_cast<size_t>(aId);
}

std::vector<ParticipantId> Ascending()
{
    std::vector<ParticipantId> out;
    for (const auto& info : kParticipantTable)
        out.push_back(info.Id);
    return out;
}

std::vector<ParticipantId> Descending()
{
    auto out = Ascending();
    std::reverse(out.begin(), out.end());
    return out;
}

std::filesystem::path FindLifecycleRepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "Session" / "Lifecycle.h"))
            return cur;
        if (!cur.has_parent_path() || cur.parent_path() == cur)
            break;
        cur = cur.parent_path();
    }
    return {};
}

std::string ReadLifecycleSource(const std::filesystem::path& acPath)
{
    std::ifstream in(acPath, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Body of `Qualified(...) ... { ... }` (braces balanced), or empty when not found.
std::string FunctionBody(const std::string& acSource, const std::string& acQualifiedName)
{
    size_t pos = 0;
    while ((pos = acSource.find(acQualifiedName + "(", pos)) != std::string::npos)
    {
        // A definition starts a line (return type first) - skip calls such as `x.Foo(`.
        const size_t lineStart = acSource.rfind('\n', pos);
        const std::string prefix = acSource.substr(lineStart == std::string::npos ? 0 : lineStart + 1,
                                                   pos - (lineStart == std::string::npos ? 0 : lineStart + 1));
        const size_t open = acSource.find('{', pos);
        const size_t semicolon = acSource.find(';', pos);
        if (open == std::string::npos)
            return {};
        if (prefix.find_first_of("=.>") == std::string::npos && (semicolon == std::string::npos || open < semicolon))
        {
            int depth = 0;
            for (size_t i = open; i < acSource.size(); ++i)
            {
                if (acSource[i] == '{')
                    ++depth;
                else if (acSource[i] == '}' && --depth == 0)
                    return acSource.substr(open, i - open + 1);
            }
            return {};
        }
        pos += acQualifiedName.size();
    }
    return {};
}

size_t CountOccurrences(const std::string& acHaystack, const std::string& acNeedle)
{
    size_t count = 0;
    for (size_t pos = acHaystack.find(acNeedle); pos != std::string::npos; pos = acHaystack.find(acNeedle, pos + acNeedle.size()))
        ++count;
    return count;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Participant table and the dependencies the order encodes
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 lifecycle: the participant table is complete, ordered and uniquely named", "[rfab][v410][lifecycle]")
{
    STATIC_REQUIRE(ParticipantTableIsOrdered());
    STATIC_REQUIRE(kParticipantTable.size() == kParticipantCount);

    std::set<std::string_view> names;
    for (size_t i = 0; i < kParticipantTable.size(); ++i)
    {
        const auto& info = kParticipantTable[i];
        CHECK(Index(info.Id) == i);
        CHECK(NameOf(info.Id) == info.Name);
        CHECK_FALSE(info.Name.empty());
        CHECK(names.insert(info.Name).second);
        if (i > 0)
            CHECK(kParticipantTable[i - 1].ParticipantStage <= info.ParticipantStage);
    }
    CHECK(NameOf(ParticipantId::kCount) == "?");
}

TEST_CASE("v4.1.0 lifecycle: participant order pins every known dependency", "[rfab][v410][lifecycle]")
{
    // Beginnings run ascending, teardowns descending: "A before B" below means A begins before B and is
    // torn down after B.

    // Identity first: everybody translates mod/form ids and cached strings, also while tearing down.
    CHECK(Index(ParticipantId::kModTable) == 0);
    CHECK(Index(ParticipantId::kStringTable) == 1);
    CHECK(kParticipantTable[Index(ParticipantId::kModTable)].ParticipantStage == Stage::kIdentity);

    // Discovery feeds the entity tables: it re-attaches the world before characters re-process it.
    CHECK(Index(ParticipantId::kDiscovery) < Index(ParticipantId::kCharacters));

    // Actor values: reset on session begin BEFORE characters re-process entities and recreate
    // ActorValuesComponent (the old CharacterService::OnConnected wiped them from inside), and still
    // readable while characters detach actors on a world teardown.
    CHECK(Index(ParticipantId::kActorValues) < Index(ParticipantId::kCharacters));

    // Downed is torn down BEFORE characters: ResetAllStates walks remote players to restore their
    // targetability, so the character purge must not have destroyed them yet.
    CHECK(Index(ParticipantId::kCharacters) < Index(ParticipantId::kDowned));

    // Coordinated saves end before characters: a save interrupted by the disconnect schedules the
    // post-save remote restore on the character service, which then tears down.
    CHECK(Index(ParticipantId::kCharacters) < Index(ParticipantId::kCoordinatedSaves));

    // Respawn mode has one session-level owner, PlayerService, and its begin runs after the downed
    // reset (which used to set it too).
    CHECK(Index(ParticipantId::kDowned) < Index(ParticipantId::kPlayer));

    // The RFAB adapter reads gameplay and world-model state (combat snapshot, party, XP); it applies
    // its co-op rules after them and reverts them first.
    for (const auto id : {ParticipantId::kCharacters, ParticipantId::kDowned, ParticipantId::kParty, ParticipantId::kPlayer})
        CHECK(Index(id) < Index(ParticipantId::kRfabAdapter));

    // Server-owned dropped items: their references are deleted on a teardown BEFORE the character purge
    // and the inventory reset (the pickup path reads both), and they are shown again only once the
    // characters exist (materializing needs the local player's cell). The resync ledger, which they
    // note gaps on, is after them.
    CHECK(Index(ParticipantId::kCharacters) < Index(ParticipantId::kWorldItems));
    CHECK(Index(ParticipantId::kInventory) < Index(ParticipantId::kWorldItems));
    CHECK(Index(ParticipantId::kWorldItems) < Index(ParticipantId::kResync));
    CHECK(kParticipantTable[Index(ParticipantId::kWorldItems)].ParticipantStage == Stage::kWorldModel);

    // UI after the state it shows; the resync ledger observes everything: last to begin (lower layers
    // may note gaps in their begin handlers), first to log its stats on the way down.
    CHECK(Index(ParticipantId::kRfabAdapter) < Index(ParticipantId::kOverlay));
    CHECK(Index(ParticipantId::kResync) == kParticipantCount - 1);
}

// ---------------------------------------------------------------------------------------------
// Machine
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 lifecycle: a session begins on acceptance and ends with the transport", "[rfab][v410][lifecycle]")
{
    Rig rig;
    CHECK(rig.Cycle.GetPhase() == Phase::kOffline);

    rig.Cycle.OnTransportUp();
    CHECK(rig.Cycle.GetPhase() == Phase::kJoining);
    CHECK(rig.Log.empty());  // a socket alone is no session

    rig.Cycle.OnAuthenticated(42);
    REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kSessionBegin});
    CHECK(rig.Nth(0).LocalPlayerId == 42);
    CHECK(rig.Nth(0).SessionGeneration == 1);
    CHECK(rig.Cycle.GetPhase() == Phase::kInWorld);
    CHECK(rig.Cycle.GetMachine().IsOnline());

    rig.Log.clear();
    rig.Cycle.OnTransportDown();
    REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kSessionEnd});
    CHECK(rig.Nth(0).Reason == EndReason::kTransportLost);
    CHECK(rig.Nth(0).SessionGeneration == 1);  // the session that ended
    CHECK_FALSE(rig.Nth(0).DuringWorldLoad);
    CHECK(rig.Cycle.GetPhase() == Phase::kOffline);
    CHECK(rig.Cycle.GetMachine().GetCounters().AnomalyCount() == 0);
}

TEST_CASE("v4.1.0 lifecycle: failed connects and rejected handshakes end no session", "[rfab][v410][lifecycle]")
{
    Rig rig;

    // TiltedConnect reports "cannot resolve" / "timeout while connecting" through OnDisconnected
    // without a preceding OnConnected. The old DisconnectedEvent ran every disconnect handler here.
    rig.Cycle.OnTransportDown();
    CHECK(rig.Log.empty());
    CHECK(rig.Cycle.GetMachine().GetCounters().ConnectAttemptsFailed == 1);

    // Wrong password / version / server full: the socket was up, no session ever began.
    rig.Cycle.OnTransportUp();
    rig.Cycle.OnTransportDown();
    CHECK(rig.Log.empty());
    CHECK(rig.Cycle.GetMachine().GetCounters().HandshakesAborted == 1);
    CHECK(rig.Cycle.GetPhase() == Phase::kOffline);

    // Expected outcomes, not anomalies.
    CHECK(rig.Cycle.GetMachine().GetCounters().AnomalyCount() == 0);

    // The next attempt works normally and starts generation 1.
    rig.Cycle.OnTransportUp();
    rig.Cycle.OnAuthenticated(3);
    REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kSessionBegin});
    CHECK(rig.Nth(0).SessionGeneration == 1);
}

TEST_CASE("v4.1.0 lifecycle: re-authentication restarts the session cleanly, before the new one is applied", "[rfab][v410][lifecycle]")
{
    Rig rig;
    rig.Online(5);

    // AuthenticationReceived is fed before the mod table and the settings of the new session are
    // applied: the old session's teardown must not wipe them.
    rig.Cycle.OnAuthenticationReceived();
    REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kSessionEnd});
    CHECK(rig.Nth(0).Reason == EndReason::kReauthenticated);
    CHECK(rig.Nth(0).SessionGeneration == 1);
    CHECK(rig.Cycle.GetPhase() == Phase::kJoining);

    rig.Cycle.OnAuthenticated(6);
    REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kSessionEnd, Signal::kSessionBegin});
    CHECK(rig.Nth(1).SessionGeneration == 2);
    CHECK(rig.Nth(1).LocalPlayerId == 6);
    CHECK(rig.Cycle.GetMachine().GetCounters().ReauthenticatedWhileOnline == 1);

    // A caller that skips AuthenticationReceived still never gets two overlapping sessions.
    rig.Log.clear();
    rig.Cycle.OnAuthenticated(7);
    REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kSessionEnd, Signal::kSessionBegin});
    CHECK(rig.Nth(0).SessionGeneration == 2);
    CHECK(rig.Nth(1).SessionGeneration == 3);
    CHECK(rig.Cycle.GetMachine().GetCounters().ReauthenticatedWhileOnline == 2);
}

TEST_CASE("v4.1.0 lifecycle: out-of-order transport input is normalised and counted", "[rfab][v410][lifecycle]")
{
    Rig rig;

    // Accepted without a socket we know of: still exactly one session.
    rig.Cycle.OnAuthenticated(1);
    REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kSessionBegin});
    CHECK(rig.Cycle.GetMachine().GetCounters().AuthenticatedWithoutTransport == 1);

    rig.Log.clear();
    rig.Cycle.OnTransportUp();  // already up
    CHECK(rig.Log.empty());
    CHECK(rig.Cycle.GetMachine().IsOnline());
    CHECK(rig.Cycle.GetMachine().GetCounters().TransportUpWhileUp == 1);
    CHECK(rig.Cycle.GetMachine().GetCounters().AnomalyCount() == 2);
}

TEST_CASE("v4.1.0 lifecycle: world loads - commit, engine start, end, failure and cancellation", "[rfab][v410][lifecycle]")
{
    Rig rig;
    rig.Online();
    const uint32_t world0 = rig.Cycle.GetMachine().GetWorldGeneration();

    SECTION("coordinated live reload: committed by the barrier, repeated by the engine hook")
    {
        rig.Cycle.OnWorldLoadBegin(true);
        REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kWorldLoadBegin});
        CHECK(rig.Nth(0).LiveReload);
        CHECK(rig.Nth(0).Online);
        CHECK_FALSE(rig.Nth(0).Repeated);
        CHECK(rig.Nth(0).WorldGeneration == world0 + 1);
        CHECK(rig.Cycle.GetPhase() == Phase::kReloading);

        // Whatever accumulated between the commit and the engine start is torn down again.
        rig.Cycle.OnWorldLoadBegin(true);
        REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kWorldLoadBegin, Signal::kWorldLoadBegin});
        CHECK(rig.Nth(1).Repeated);
        CHECK(rig.Nth(1).WorldGeneration == world0 + 1);  // still no world loaded in between
        CHECK(rig.Cycle.GetMachine().GetCounters().RepeatedWorldLoadBegin == 1);

        rig.Cycle.OnWorldLoadEnd(true);
        REQUIRE(rig.Signals().back() == Signal::kWorldLoadEnd);
        CHECK(rig.Nth(2).Succeeded);
        CHECK_FALSE(rig.Nth(2).Cancelled);
        CHECK(rig.Nth(2).Online);
        CHECK(rig.Cycle.GetPhase() == Phase::kInWorld);
        CHECK(rig.Cycle.GetMachine().GetCounters().AnomalyCount() == 0);

        // The executor's failure path may always cancel: no load is open, nothing happens.
        rig.Log.clear();
        rig.Cycle.OnWorldLoadCancel();
        CHECK(rig.Log.empty());
    }

    SECTION("a failed engine load ends the load as failed")
    {
        rig.Cycle.OnWorldLoadBegin(false);
        rig.Cycle.OnWorldLoadEnd(false);
        REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kWorldLoadBegin, Signal::kWorldLoadEnd});
        CHECK_FALSE(rig.Nth(1).Succeeded);
        CHECK_FALSE(rig.Nth(1).Cancelled);
        CHECK_FALSE(rig.Cycle.GetMachine().IsWorldLoading());
    }

    SECTION("a committed load the engine never started is cancelled")
    {
        rig.Cycle.OnWorldLoadBegin(true);
        rig.Cycle.OnWorldLoadCancel();
        REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kWorldLoadBegin, Signal::kWorldLoadEnd});
        CHECK_FALSE(rig.Nth(1).Succeeded);
        CHECK(rig.Nth(1).Cancelled);
        CHECK(rig.Cycle.GetPhase() == Phase::kInWorld);
    }

    SECTION("an end without a begin is an anomaly and delivers nothing")
    {
        rig.Cycle.OnWorldLoadEnd(true);
        CHECK(rig.Log.empty());
        CHECK(rig.Cycle.GetMachine().GetCounters().WorldLoadEndWithoutBegin == 1);
        CHECK(rig.Cycle.GetMachine().GetCounters().AnomalyCount() == 1);
    }

    SECTION("the session ends while the engine loads (uncoordinated load closes the socket)")
    {
        rig.Cycle.OnWorldLoadBegin(false);
        CHECK_FALSE(rig.Nth(0).LiveReload);
        CHECK(rig.Nth(0).Online);

        rig.Cycle.OnTransportDown();
        REQUIRE(rig.Signals().back() == Signal::kSessionEnd);
        CHECK(rig.Nth(1).DuringWorldLoad);  // CharacterService must not Disable/Delete now (+0x6EDFC2)
        CHECK(rig.Cycle.GetMachine().IsWorldLoading());
        CHECK(rig.Cycle.GetPhase() == Phase::kOffline);

        rig.Cycle.OnWorldLoadEnd(true);
        REQUIRE(rig.Signals().back() == Signal::kWorldLoadEnd);
        CHECK_FALSE(rig.Nth(2).Online);
        CHECK(rig.Nth(2).SessionGeneration == 0);
    }
}

TEST_CASE("v4.1.0 lifecycle: offline loads are teardowns too, never live reloads", "[rfab][v410][lifecycle]")
{
    Rig rig;
    rig.Cycle.OnWorldLoadBegin(true);  // "live" requested without a session
    REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kWorldLoadBegin});
    CHECK_FALSE(rig.Nth(0).Online);
    CHECK_FALSE(rig.Nth(0).LiveReload);
    CHECK(rig.Nth(0).SessionGeneration == 0);
    CHECK(rig.Cycle.GetPhase() == Phase::kOffline);  // offline dominates the phase
    rig.Cycle.OnWorldLoadEnd(true);
    CHECK(rig.Signals().size() == 2);
}

TEST_CASE("v4.1.0 lifecycle: pause is a phase, not a transition", "[rfab][v410][lifecycle]")
{
    Rig rig;
    rig.Online();

    CHECK(rig.Cycle.SetPaused(true));
    CHECK_FALSE(rig.Cycle.SetPaused(true));  // reports changes only
    CHECK(rig.Cycle.GetPhase() == Phase::kPaused);
    CHECK(rig.Log.empty());

    rig.Cycle.OnWorldLoadBegin(true);
    CHECK(rig.Cycle.GetPhase() == Phase::kReloading);  // the load dominates the menu
    rig.Cycle.OnWorldLoadEnd(true);
    CHECK(rig.Cycle.GetPhase() == Phase::kPaused);

    CHECK(rig.Cycle.SetPaused(false));
    CHECK(rig.Cycle.GetPhase() == Phase::kInWorld);
    CHECK(rig.Signals() == std::vector<Signal>{Signal::kWorldLoadBegin, Signal::kWorldLoadEnd});
}

// ---------------------------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 lifecycle: beginnings run ascending, teardowns descending", "[rfab][v410][lifecycle]")
{
    Rig rig;
    rig.Cycle.OnTransportUp();
    rig.Cycle.OnAuthenticated(1);
    rig.Cycle.OnWorldLoadBegin(true);
    rig.Cycle.OnWorldLoadEnd(true);
    rig.Cycle.OnTransportDown();

    REQUIRE(rig.Signals() ==
            std::vector<Signal>{Signal::kSessionBegin, Signal::kWorldLoadBegin, Signal::kWorldLoadEnd, Signal::kSessionEnd});
    CHECK(rig.OrderOf(0) == Ascending());
    CHECK(rig.OrderOf(1) == Descending());
    CHECK(rig.OrderOf(2) == Ascending());
    CHECK(rig.OrderOf(3) == Descending());
    CHECK(rig.Cycle.GetDeliveredCount() == 4);
}

TEST_CASE("v4.1.0 lifecycle: input fed during a delivery is delivered after it, never interleaved", "[rfab][v410][lifecycle]")
{
    Rig rig;
    rig.Online();

    SECTION("a participant closes the socket inside a world teardown")
    {
        bool sawOfflineImmediately = false;
        rig.Of(ParticipantId::kCoordinatedSaves).Hook = [&](const Transition& acTransition) {
            if (acTransition.Kind == Signal::kWorldLoadBegin)
            {
                rig.Cycle.OnTransportDown();
                // The machine already moved on - queries made during dispatch see the latest state...
                sawOfflineImmediately = !rig.Cycle.GetMachine().IsOnline();
            }
        };

        rig.Cycle.OnWorldLoadBegin(false);

        // ...but every participant got the whole teardown before the session end started.
        REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kWorldLoadBegin, Signal::kSessionEnd});
        CHECK(rig.OrderOf(0) == Descending());
        CHECK(rig.OrderOf(1) == Descending());
        CHECK(rig.Nth(1).DuringWorldLoad);
        CHECK(sawOfflineImmediately);
        CHECK_FALSE(rig.Cycle.IsDispatching());
    }

    SECTION("a participant cancels a committed load inside the session end")
    {
        rig.Cycle.OnWorldLoadBegin(true);
        rig.Log.clear();
        rig.Of(ParticipantId::kCoordinatedSaves).Hook = [&](const Transition& acTransition) {
            if (acTransition.Kind == Signal::kSessionEnd)
                rig.Cycle.OnWorldLoadCancel();
        };

        rig.Cycle.OnTransportDown();

        REQUIRE(rig.Signals() == std::vector<Signal>{Signal::kSessionEnd, Signal::kWorldLoadEnd});
        CHECK(rig.Nth(0).DuringWorldLoad);
        CHECK(rig.Nth(1).Cancelled);
        CHECK_FALSE(rig.Nth(1).Online);
        CHECK(rig.OrderOf(1) == Ascending());
        CHECK_FALSE(rig.Cycle.GetMachine().IsWorldLoading());
    }
}

TEST_CASE("v4.1.0 lifecycle: the world-load gate closes before the first teardown and opens before the first end handler", "[rfab][v410][lifecycle]")
{
    Rig rig;
    rig.Online();

    std::vector<bool> gateSeen;
    auto record = [&](const Transition&) { gateSeen.push_back(rig.Cycle.IsWorldLoadingAnyThread()); };
    // kResync tears down first, kModTable handles the end first.
    rig.Of(ParticipantId::kResync).Hook = record;
    rig.Of(ParticipantId::kModTable).Hook = record;

    rig.Cycle.OnWorldLoadBegin(true);
    rig.Cycle.OnWorldLoadEnd(false);

    // Resync (begin), ModTable (begin), ModTable (end), Resync (end).
    REQUIRE(gateSeen.size() == 4);
    CHECK(gateSeen[0]);
    CHECK(gateSeen[1]);
    CHECK_FALSE(gateSeen[2]);  // Discovery re-attaches on a failed end - CharacterService must not gate it
    CHECK_FALSE(gateSeen[3]);
}

TEST_CASE("v4.1.0 lifecycle: tokens expire with their session or world", "[rfab][v410][lifecycle]")
{
    Rig rig;

    const Token none = rig.Cycle.GetTokenAnyThread();
    CHECK(none.Session == 0);
    CHECK_FALSE(rig.Cycle.IsStillCurrentSession(none));

    rig.Online();
    const Token first = rig.Cycle.GetTokenAnyThread();
    CHECK(first.Session == 1);
    CHECK(rig.Cycle.IsStillCurrentSession(first));
    CHECK(rig.Cycle.IsStillCurrentWorld(first));
    CHECK(Token::Unpack(first.Pack()).Session == first.Session);
    CHECK(Token::Unpack(first.Pack()).World == first.World);

    // A world load keeps the session but invalidates world-scoped work.
    rig.Cycle.OnWorldLoadBegin(true);
    rig.Cycle.OnWorldLoadEnd(true);
    CHECK(rig.Cycle.IsStillCurrentSession(first));
    CHECK_FALSE(rig.Cycle.IsStillCurrentWorld(first));
    const Token afterReload = rig.Cycle.GetTokenAnyThread();
    CHECK(rig.Cycle.IsStillCurrentWorld(afterReload));

    // Work posted in session 1 must not land in session 2 (SessionResyncService's marshalled notes).
    rig.Cycle.OnTransportDown();
    CHECK_FALSE(rig.Cycle.IsStillCurrentSession(first));
    CHECK(rig.Cycle.GetTokenAnyThread().Session == 0);
    rig.Cycle.OnTransportUp();
    rig.Cycle.OnAuthenticated(9);
    CHECK_FALSE(rig.Cycle.IsStillCurrentSession(first));
    CHECK_FALSE(rig.Cycle.IsStillCurrentSession(afterReload));
    CHECK(rig.Cycle.GetTokenAnyThread().Session == 2);
}

TEST_CASE("v4.1.0 lifecycle: registration rejects duplicates and reports gaps; the observer sees every transition", "[rfab][v410][lifecycle]")
{
    Lifecycle cycle;
    std::vector<Delivery> log;
    Recorder a(ParticipantId::kCharacters, log);
    Recorder b(ParticipantId::kCharacters, log);

    CHECK(cycle.Register(ParticipantId::kCharacters, a));
    CHECK_FALSE(cycle.Register(ParticipantId::kCharacters, b));  // the first registration stays
    CHECK_FALSE(cycle.Register(ParticipantId::kCount, b));
    CHECK(cycle.IsRegistered(ParticipantId::kCharacters));
    CHECK(cycle.MissingParticipants().size() == kParticipantCount - 1);

    struct Seen
    {
        std::vector<std::pair<Signal, Phase>> Entries;
    } seen;
    cycle.SetObserver(
        [](void* apContext, const Transition& acTransition, Phase aPhaseAfter) {
            static_cast<Seen*>(apContext)->Entries.emplace_back(acTransition.Kind, aPhaseAfter);
        },
        &seen);

    cycle.OnTransportUp();
    cycle.OnAuthenticated(1);
    cycle.OnWorldLoadBegin(true);
    cycle.OnTransportDown();

    REQUIRE(seen.Entries.size() == 3);
    CHECK(seen.Entries[0] == std::make_pair(Signal::kSessionBegin, Phase::kInWorld));
    CHECK(seen.Entries[1] == std::make_pair(Signal::kWorldLoadBegin, Phase::kReloading));
    CHECK(seen.Entries[2] == std::make_pair(Signal::kSessionEnd, Phase::kOffline));
    CHECK(log.size() == 3);  // only the registered participant (a) received them
}

// ---------------------------------------------------------------------------------------------
// Client wiring (source audit)
// ---------------------------------------------------------------------------------------------

TEST_CASE("v4.1.0 lifecycle: World registers every participant exactly once, in one place", "[rfab][v410][lifecycle][audit]")
{
    const auto root = FindLifecycleRepoRoot();
    if (root.empty())
    {
        WARN("repository root not found from the test working directory; skipping source wiring audit");
        return;
    }

    const std::string world = ReadLifecycleSource(root / "Code" / "client" / "World.cpp");
    REQUIRE_FALSE(world.empty());
    const std::string registration = FunctionBody(world, "World::RegisterLifecycleParticipants");
    REQUIRE_FALSE(registration.empty());

    for (const auto& info : kParticipantTable)
    {
        const std::string call = "add(ParticipantId::k" + std::string(info.Name) + ",";
        INFO(call);
        CHECK(CountOccurrences(registration, call) == 1);
    }
    CHECK(CountOccurrences(registration, "add(ParticipantId::") == kParticipantCount);
    CHECK(CountOccurrences(world, "RegisterLifecycleParticipants();") == 1);
    CHECK(registration.find("MissingParticipants()") != std::string::npos);
}

TEST_CASE("v4.1.0 lifecycle: every participant class implements Session::IParticipant", "[rfab][v410][lifecycle][audit]")
{
    const auto root = FindLifecycleRepoRoot();
    if (root.empty())
    {
        WARN("repository root not found from the test working directory; skipping source wiring audit");
        return;
    }

    const std::map<ParticipantId, std::pair<std::string, std::string>> classes = {
        {ParticipantId::kModTable, {"Systems/ModSystem.h", "ModSystem"}},
        {ParticipantId::kStringTable, {"Services/StringCacheService.h", "StringCacheService"}},
        {ParticipantId::kDiscovery, {"Services/DiscoveryService.h", "DiscoveryService"}},
        {ParticipantId::kActorValues, {"Services/ActorValueService.h", "ActorValueService"}},
        {ParticipantId::kCharacters, {"Services/CharacterService.h", "CharacterService"}},
        {ParticipantId::kObjects, {"Services/ObjectService.h", "ObjectService"}},
        {ParticipantId::kInventory, {"Services/InventoryService.h", "InventoryService"}},
        {ParticipantId::kMagic, {"Services/MagicService.h", "MagicService"}},
        {ParticipantId::kDowned, {"Services/DownedStateService.h", "DownedStateService"}},
        {ParticipantId::kCoordinatedSaves, {"Services/CoordinatedSaveService.h", "CoordinatedSaveService"}},
        {ParticipantId::kParty, {"Services/PartyService.h", "PartyService"}},
        {ParticipantId::kPlayer, {"Services/PlayerService.h", "PlayerService"}},
        {ParticipantId::kCalendar, {"Services/CalendarService.h", "CalendarService"}},
        {ParticipantId::kWeather, {"Services/WeatherService.h", "WeatherService"}},
        {ParticipantId::kQuests, {"Services/QuestService.h", "QuestService"}},
        {ParticipantId::kRfabAdapter, {"Services/RfabAdapterService.h", "RfabAdapterService"}},
        {ParticipantId::kOverlay, {"Services/OverlayService.h", "OverlayService"}},
        {ParticipantId::kResync, {"Services/SessionResyncService.h", "SessionResyncService"}},
    };
    REQUIRE(classes.size() == kParticipantCount);

    for (const auto& [id, where] : classes)
    {
        const std::string header = ReadLifecycleSource(root / "Code" / "client" / where.first);
        INFO(where.first);
        REQUIRE_FALSE(header.empty());
        // `struct|class Name final : ... Session::IParticipant` on the class head line.
        const std::regex head("(struct|class) " + where.second + " final : [^\\n{]*Session::IParticipant");
        CHECK(std::regex_search(header, head));
        CHECK(header.find("override;") != std::string::npos);
        (void)id;
    }
}

TEST_CASE("v4.1.0 lifecycle: no ad-hoc connect/disconnect/reload resets remain", "[rfab][v410][lifecycle][audit]")
{
    const auto root = FindLifecycleRepoRoot();
    if (root.empty())
    {
        WARN("repository root not found from the test working directory; skipping source wiring audit");
        return;
    }

    namespace fs = std::filesystem;
    const fs::path client = root / "Code" / "client";
    CHECK_FALSE(fs::exists(client / "Events" / "ConnectedEvent.h"));
    CHECK_FALSE(fs::exists(client / "Events" / "DisconnectedEvent.h"));

    // Identifiers of the removed mechanism. A new subsystem with session state becomes a participant.
    const std::vector<std::string> forbidden = {
        "ConnectedEvent", "DisconnectedEvent",                                // the two events
        "ResetForWorldReload", "NotifySaveLoadReset",                         // hand-called reset lists
        "BeginEngineWorldLoad", "EndEngineWorldLoad", "m_engineWorldLoadInProgress",  // the old load flag
    };
    size_t scanned = 0;
    for (const auto& entry : fs::recursive_directory_iterator(client))
    {
        if (!entry.is_regular_file())
            continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".inl")
            continue;
        ++scanned;
        const std::string text = ReadLifecycleSource(entry.path());
        for (const auto& word : forbidden)
        {
            const std::regex identifier("\\b" + word + "\\b");
            INFO(entry.path().string() << " mentions " << word);
            CHECK_FALSE(std::regex_search(text, identifier));
        }
    }
    CHECK(scanned > 100);
}

TEST_CASE("v4.1.0 lifecycle: the transport and the save/load path feed the lifecycle at the right points", "[rfab][v410][lifecycle][audit]")
{
    const auto root = FindLifecycleRepoRoot();
    if (root.empty())
    {
        WARN("repository root not found from the test working directory; skipping source wiring audit");
        return;
    }

    const auto services = root / "Code" / "client" / "Services";

    SECTION("TransportService")
    {
        const std::string transport = ReadLifecycleSource(services / "Generic" / "TransportService.cpp");
        REQUIRE_FALSE(transport.empty());

        CHECK(FunctionBody(transport, "TransportService::OnConnected").find("OnTransportUp()") != std::string::npos);

        const std::string down = FunctionBody(transport, "TransportService::OnDisconnected");
        CHECK(down.find("OnTransportDown()") != std::string::npos);
        // A failed attempt ends no session but must still take the UI out of "connecting".
        CHECK(down.find("NotifyConnectionAttemptEnded()") != std::string::npos);
        CHECK(down.find("IsOnline()") < down.find("OnTransportDown()"));

        const std::string auth = FunctionBody(transport, "TransportService::HandleAuthenticationResponse");
        const size_t received = auth.find("OnAuthenticationReceived()");
        const size_t settingsApplied = auth.find("m_dispatcher.trigger(acMessage.Settings)");
        const size_t modsApplied = auth.find("m_dispatcher.trigger(acMessage.UserMods)");
        const size_t begun = auth.find("OnAuthenticated(acMessage.PlayerId)");
        REQUIRE(received != std::string::npos);
        REQUIRE(begun != std::string::npos);
        // The old session ends before the new mod table / settings land; the new one begins after.
        CHECK(received < modsApplied);
        CHECK(received < settingsApplied);
        CHECK(modsApplied < begun);
        CHECK(settingsApplied < begun);
    }

    SECTION("CoordinatedSaveService")
    {
        const std::string saves = ReadLifecycleSource(services / "CoordinatedSaveService.cpp");
        REQUIRE_FALSE(saves.empty());

        const std::string prepare = FunctionBody(saves, "CoordinatedSaveService::PrepareEngineForWorldLoad");
        CHECK(prepare.find("OnWorldLoadBegin(liveReload)") != std::string::npos);
        // The paused queue is dropped before the teardown, the socket closed after it.
        CHECK(prepare.find("DiscardPausedWorldPacketsForReload()") < prepare.find("OnWorldLoadBegin(liveReload)"));
        CHECK(prepare.find("OnWorldLoadBegin(liveReload)") < prepare.find("m_transport.Close()"));

        const std::string completed = FunctionBody(saves, "CoordinatedSaveService::NotifyEngineLoadCompleted");
        CHECK(completed.find("OnWorldLoadEnd(aSucceeded)") != std::string::npos);

        const std::string executor = FunctionBody(saves, "CoordinatedSaveService::ProcessReadyDeferredSaveOrLoad");
        CHECK(executor.find("OnWorldLoadCancel()") != std::string::npos);

        const std::string barrier = FunctionBody(saves, "CoordinatedSaveService::OnNotifyExecuteLoad");
        CHECK(barrier.find("OnWorldLoadBegin(true)") != std::string::npos);

        const std::string sessionEnd = FunctionBody(saves, "CoordinatedSaveService::OnSessionEnd");
        CHECK(sessionEnd.find("OnWorldLoadCancel()") != std::string::npos);
    }

    SECTION("SaveLoad hooks bracket every engine load")
    {
        const std::string hooks = ReadLifecycleSource(root / "Code" / "client" / "Games" / "Skyrim" / "SaveLoad.cpp");
        REQUIRE_FALSE(hooks.empty());
        const size_t prepare = hooks.find("PrepareEngineForWorldLoad(");
        const size_t completed = hooks.find("NotifyEngineLoadCompleted(");
        REQUIRE(prepare != std::string::npos);
        REQUIRE(completed != std::string::npos);
        CHECK(prepare < completed);
    }

    SECTION("Respawn mode has one session-level owner")
    {
        const std::string downed = ReadLifecycleSource(services / "DownedStateService.cpp");
        const std::string player = ReadLifecycleSource(services / "Generic" / "PlayerService.cpp");
        CHECK(FunctionBody(downed, "DownedStateService::OnSessionBegin").find("SetPlayerRespawnMode") == std::string::npos);
        CHECK(FunctionBody(player, "PlayerService::OnSessionBegin").find("SetPlayerRespawnMode(true)") != std::string::npos);
        CHECK(FunctionBody(player, "PlayerService::OnSessionEnd").find("SetPlayerRespawnMode(false)") != std::string::npos);
    }

    SECTION("The world-load gate is the lifecycle's")
    {
        const std::string characters = ReadLifecycleSource(services / "Generic" / "CharacterService.cpp");
        CHECK(FunctionBody(characters, "CharacterService::IsEngineWorldLoading").find("IsWorldLoadingAnyThread()") != std::string::npos);
        // No Disable/Delete while the engine replaces the world.
        CHECK(FunctionBody(characters, "CharacterService::OnSessionEnd").find("DuringWorldLoad") != std::string::npos);
    }
}
