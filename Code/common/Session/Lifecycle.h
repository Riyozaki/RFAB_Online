#pragma once

// Architecture step 2: one owner of the client's session and world lifecycle.
//
// Before v4.1.0 every client service subscribed to ConnectedEvent / DisconnectedEvent on its own and
// CoordinatedSaveService called a hand-maintained list of resets before a save load. The order those
// handlers ran in was the order the services happened to be constructed in - nobody chose it, and
// several resets silently depended on it (the downed reset walks remote players, so it has to run
// before the character purge destroys them; actor values are wiped from inside the character
// service's connect handler, before entities are re-processed; ...). A failed connection attempt or
// a rejected handshake ran all the disconnect handlers although no session had begun (restoring
// "baselines" that were never captured), and the same reset could run two or three times per event.
//
// Both events are gone; World.cpp registers every participant in one place.
//
// Here the lifecycle is explicit:
//
//   * Machine            - the state (offline / joining / in world / paused / reloading) and the only
//                          place that decides which transitions an input causes. Out-of-order input
//                          is normalised and counted: never two sessions at once, never an end
//                          without its begin, never a session end for a session that never began.
//   * ParticipantId      - every subsystem with session- or world-scoped state. The enum order IS the
//                          dispatch order: beginnings run in ascending order, teardowns in descending
//                          order (constructor / destructor discipline), so a layer is always torn down
//                          while the layers it reads are still intact.
//   * Lifecycle          - Machine + participant table + a FIFO so a transition caused while another one
//                          is being delivered (Close() inside a teardown) is delivered after it, never
//                          interleaved with it.
//   * Token              - session/world generations. Work queued in one session (a lambda marshalled
//                          from a game thread, a deferred task) checks its token and dies with the
//                          session instead of leaking into the next one.
//
// Portable (no game or engine headers): Code/tests/rfab_v410_session_lifecycle.cpp drives it directly.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Session
{
// ---------------------------------------------------------------------------------------------
// Participants and their order
// ---------------------------------------------------------------------------------------------

enum class Stage : uint8_t
{
    kIdentity = 0,      // id translation tables everything else reads through
    kWorldModel = 1,    // discovery and the entity tables built from it
    kGameplay = 2,      // rules on top of the world model
    kModpack = 3,       // RFAB adapter: reads gameplay and world model
    kPresentation = 4,  // UI notifications
    kDiagnostics = 5,   // observers of everything above (resync ledger)
};

// Declaration order is dispatch order (ascending for beginnings, descending for teardowns). Moving an
// entry is a behaviour change: Code/tests/rfab_v410_session_lifecycle.cpp pins every dependency that
// relies on the order, with the reason.
enum class ParticipantId : uint8_t
{
    // kIdentity
    kModTable = 0,     // ModSystem: client <-> server plugin index
    kStringTable,      // StringCacheService: shared CachedString table
    // kWorldModel
    kDiscovery,        // DiscoveryService: which actors/cells this client has seen
    kActorValues,      // ActorValueService: health revisions, prediction state, ActorValuesComponent
    kCharacters,       // CharacterService: remote models, network actors, ownership leases
    kObjects,          // ObjectService: object entities
    kInventory,        // InventoryService: pending NPC ammo deltas
    kMagic,            // MagicService: queued effects
    // kGameplay
    kDowned,           // DownedStateService
    kCoordinatedSaves, // CoordinatedSaveService
    kParty,            // PartyService
    kPlayer,           // PlayerService: difficulty, globals, god mode
    kCalendar,         // CalendarService
    kWeather,          // WeatherService
    kQuests,           // QuestService
    // kModpack
    kRfabAdapter,      // RfabAdapterService
    // kPresentation
    kOverlay,          // OverlayService
    // kDiagnostics
    kResync,           // SessionResyncService
    kCount
};

inline constexpr size_t kParticipantCount = static_cast<size_t>(ParticipantId::kCount);

struct ParticipantInfo
{
    ParticipantId Id;
    Stage ParticipantStage;
    std::string_view Name;  // the enumerator without the k: World.cpp registers Session::ParticipantId::k<Name>
};

inline constexpr std::array<ParticipantInfo, kParticipantCount> kParticipantTable{{
    {ParticipantId::kModTable, Stage::kIdentity, "ModTable"},
    {ParticipantId::kStringTable, Stage::kIdentity, "StringTable"},
    {ParticipantId::kDiscovery, Stage::kWorldModel, "Discovery"},
    {ParticipantId::kActorValues, Stage::kWorldModel, "ActorValues"},
    {ParticipantId::kCharacters, Stage::kWorldModel, "Characters"},
    {ParticipantId::kObjects, Stage::kWorldModel, "Objects"},
    {ParticipantId::kInventory, Stage::kWorldModel, "Inventory"},
    {ParticipantId::kMagic, Stage::kWorldModel, "Magic"},
    {ParticipantId::kDowned, Stage::kGameplay, "Downed"},
    {ParticipantId::kCoordinatedSaves, Stage::kGameplay, "CoordinatedSaves"},
    {ParticipantId::kParty, Stage::kGameplay, "Party"},
    {ParticipantId::kPlayer, Stage::kGameplay, "Player"},
    {ParticipantId::kCalendar, Stage::kGameplay, "Calendar"},
    {ParticipantId::kWeather, Stage::kGameplay, "Weather"},
    {ParticipantId::kQuests, Stage::kGameplay, "Quests"},
    {ParticipantId::kRfabAdapter, Stage::kModpack, "RfabAdapter"},
    {ParticipantId::kOverlay, Stage::kPresentation, "Overlay"},
    {ParticipantId::kResync, Stage::kDiagnostics, "Resync"},
}};

[[nodiscard]] constexpr bool ParticipantTableIsOrdered() noexcept
{
    for (size_t i = 0; i < kParticipantTable.size(); ++i)
    {
        if (static_cast<size_t>(kParticipantTable[i].Id) != i)
            return false;
        if (i > 0 && kParticipantTable[i].ParticipantStage < kParticipantTable[i - 1].ParticipantStage)
            return false;
    }
    return true;
}
static_assert(ParticipantTableIsOrdered(), "kParticipantTable must follow ParticipantId order with non-decreasing stages");

[[nodiscard]] constexpr std::string_view NameOf(ParticipantId aId) noexcept
{
    const auto index = static_cast<size_t>(aId);
    return index < kParticipantTable.size() ? kParticipantTable[index].Name : std::string_view{"?"};
}

// ---------------------------------------------------------------------------------------------
// State, transitions, tokens
// ---------------------------------------------------------------------------------------------

enum class Link : uint8_t
{
    kOffline = 0,
    kHandshaking = 1,  // socket up, AuthenticationResponse not yet accepted: no session exists
    kOnline = 2,
};

// What a log line or a UI badge should say. Derived, never stored.
enum class Phase : uint8_t
{
    kOffline = 0,
    kJoining = 1,
    kInWorld = 2,
    kPaused = 3,
    kReloading = 4,
};

enum class Signal : uint8_t
{
    kSessionBegin = 0,    // ascending
    kSessionEnd = 1,      // descending
    kWorldLoadBegin = 2,  // descending: the world store is torn down
    kWorldLoadEnd = 3,    // ascending
};

enum class EndReason : uint8_t
{
    kTransportLost = 0,    // socket closed (server, network, Close() for an uncoordinated load, quit)
    kReauthenticated = 1,  // a second AuthenticationResponse arrived for a live session: restart cleanly
};

[[nodiscard]] constexpr bool IsTeardown(Signal aSignal) noexcept
{
    return aSignal == Signal::kSessionEnd || aSignal == Signal::kWorldLoadBegin;
}

struct Transition
{
    Signal Kind{Signal::kSessionBegin};
    uint32_t SessionGeneration{0};  // session this transition belongs to (0: none)
    uint32_t WorldGeneration{0};    // world in effect after the transition
    uint32_t LocalPlayerId{0};      // kSessionBegin
    EndReason Reason{EndReason::kTransportLost};  // kSessionEnd
    bool DuringWorldLoad{false};    // kSessionEnd: the engine is loading a save right now
    bool Online{false};             // kWorldLoad*: a session is alive at this moment
    bool LiveReload{false};         // kWorldLoadBegin: coordinated reload that keeps the socket open
    bool Repeated{false};           // kWorldLoadBegin: the world was already torn down for this load
    bool Succeeded{true};           // kWorldLoadEnd: false when the load failed or was cancelled
    bool Cancelled{false};          // kWorldLoadEnd: the engine never started loading
};

struct Token
{
    uint32_t Session{0};
    uint32_t World{0};

    [[nodiscard]] constexpr uint64_t Pack() const noexcept { return (static_cast<uint64_t>(Session) << 32) | World; }
    [[nodiscard]] static constexpr Token Unpack(uint64_t aPacked) noexcept
    {
        return Token{static_cast<uint32_t>(aPacked >> 32), static_cast<uint32_t>(aPacked & 0xFFFFFFFFu)};
    }
};

struct Counters
{
    uint32_t AuthenticatedWithoutTransport{0};
    uint32_t ReauthenticatedWhileOnline{0};
    uint32_t TransportUpWhileUp{0};
    uint32_t ConnectAttemptsFailed{0};  // not a bug: unresolvable host, timeout while connecting
    uint32_t HandshakesAborted{0};      // not a bug: wrong password, version mismatch, server full
    uint32_t RepeatedWorldLoadBegin{0};  // not a bug: committed by the reload barrier, then started by the engine
    uint32_t WorldLoadEndWithoutBegin{0};

    [[nodiscard]] uint32_t AnomalyCount() const noexcept
    {
        // Expected outcomes (failed connects, rejected handshakes) are not anomalies.
        return AuthenticatedWithoutTransport + ReauthenticatedWhileOnline + TransportUpWhileUp + WorldLoadEndWithoutBegin;
    }
};

class Machine
{
public:
    // Every input appends the transitions it causes (0..2) to aOut, in delivery order.

    void TransportUp(std::vector<Transition>& aOut) noexcept
    {
        (void)aOut;
        if (m_link != Link::kOffline)
        {
            ++m_counters.TransportUpWhileUp;
            return;
        }
        m_link = Link::kHandshaking;
    }

    // An accepted AuthenticationResponse arrived and is about to be applied (mod table, settings).
    // A live session must end BEFORE that: its teardown would otherwise wipe the new session's state.
    void AuthenticationReceived(std::vector<Transition>& aOut) noexcept
    {
        if (m_link == Link::kOnline)
        {
            // The server accepted us twice. Whatever the reason, two overlapping sessions are worse
            // than one clean restart: end the current one now, begin the new one in Authenticated().
            ++m_counters.ReauthenticatedWhileOnline;
            EndSession(EndReason::kReauthenticated, aOut);
            m_link = Link::kHandshaking;
        }
        else if (m_link == Link::kOffline)
        {
            ++m_counters.AuthenticatedWithoutTransport;
            m_link = Link::kHandshaking;
        }
    }

    void Authenticated(uint32_t aLocalPlayerId, std::vector<Transition>& aOut) noexcept
    {
        // Callers that skipped AuthenticationReceived get the same guarantees, only later.
        AuthenticationReceived(aOut);

        m_link = Link::kOnline;
        ++m_sessionGeneration;
        if (m_sessionGeneration == 0)
            m_sessionGeneration = 1;  // 0 means "no session" in tokens

        Transition t = Make(Signal::kSessionBegin);
        t.LocalPlayerId = aLocalPlayerId;
        aOut.push_back(t);
    }

    void TransportDown(std::vector<Transition>& aOut) noexcept
    {
        switch (m_link)
        {
        case Link::kOffline:
            // TiltedConnect reports a failed connection attempt (cannot resolve, timeout while
            // connecting) through the same callback, without a preceding TransportUp.
            ++m_counters.ConnectAttemptsFailed;
            return;
        case Link::kHandshaking:
            ++m_counters.HandshakesAborted;
            m_link = Link::kOffline;
            return;  // no session began, so none ends
        case Link::kOnline: EndSession(EndReason::kTransportLost, aOut); return;
        }
    }

    // The world this client lives in is about to be replaced. Fed when a reload is committed (the
    // coordinated reload barrier, frames before the engine loads) and again when the engine starts the
    // load. A begin while already loading is delivered again as Repeated: every teardown is idempotent,
    // and whatever accumulated since the first one (packets, discovered forms, queued effects) must not
    // survive into the next world. The world generation does not change - no world was loaded between.
    void WorldLoadBegin(bool aLiveReload, std::vector<Transition>& aOut) noexcept
    {
        const bool repeated = m_worldLoading;
        if (repeated)
            ++m_counters.RepeatedWorldLoadBegin;
        else
        {
            m_worldLoading = true;
            ++m_worldGeneration;
        }

        Transition t = Make(Signal::kWorldLoadBegin);
        t.Online = m_link == Link::kOnline;
        t.LiveReload = aLiveReload && t.Online;
        t.Repeated = repeated;
        aOut.push_back(t);
    }

    // The engine finished loading (aSucceeded: a new world is in place).
    void WorldLoadEnd(bool aSucceeded, std::vector<Transition>& aOut) noexcept
    {
        if (!m_worldLoading)
        {
            ++m_counters.WorldLoadEndWithoutBegin;
            return;
        }
        FinishWorldLoad(aSucceeded, false, aOut);
    }

    // A committed load will not happen after all (the engine refused to start it, the session it was
    // coordinated with ended first). No-op when no load is open, so a failure path that cannot know
    // whether the engine hook already reported the end may always call it.
    void WorldLoadCancel(std::vector<Transition>& aOut) noexcept
    {
        if (m_worldLoading)
            FinishWorldLoad(false, true, aOut);
    }

    // Returns true when the value changed. Pause causes no transition: nothing is torn down while a
    // menu is open, the paused pump keeps the session alive. It is part of the observable phase.
    bool SetPaused(bool aPaused) noexcept
    {
        if (m_paused == aPaused)
            return false;
        m_paused = aPaused;
        return true;
    }

    [[nodiscard]] Link GetLink() const noexcept { return m_link; }
    [[nodiscard]] bool IsOnline() const noexcept { return m_link == Link::kOnline; }
    [[nodiscard]] bool IsWorldLoading() const noexcept { return m_worldLoading; }
    [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }
    [[nodiscard]] uint32_t GetSessionGeneration() const noexcept { return m_sessionGeneration; }
    [[nodiscard]] uint32_t GetWorldGeneration() const noexcept { return m_worldGeneration; }
    [[nodiscard]] const Counters& GetCounters() const noexcept { return m_counters; }

    [[nodiscard]] Phase GetPhase() const noexcept
    {
        switch (m_link)
        {
        case Link::kOffline: return Phase::kOffline;
        case Link::kHandshaking: return Phase::kJoining;
        case Link::kOnline: break;
        }
        if (m_worldLoading)
            return Phase::kReloading;
        return m_paused ? Phase::kPaused : Phase::kInWorld;
    }

    // Session 0 while no session is alive: such a token is never current.
    [[nodiscard]] Token GetToken() const noexcept
    {
        return Token{IsOnline() ? m_sessionGeneration : 0u, m_worldGeneration};
    }

    [[nodiscard]] bool IsCurrentSession(Token aToken) const noexcept
    {
        return aToken.Session != 0 && IsOnline() && aToken.Session == m_sessionGeneration;
    }

    [[nodiscard]] bool IsCurrentWorld(Token aToken) const noexcept
    {
        return IsCurrentSession(aToken) && aToken.World == m_worldGeneration;
    }

private:
    void EndSession(EndReason aReason, std::vector<Transition>& aOut) noexcept
    {
        Transition t = Make(Signal::kSessionEnd);
        t.Reason = aReason;
        t.DuringWorldLoad = m_worldLoading;
        aOut.push_back(t);
        m_link = Link::kOffline;
    }

    void FinishWorldLoad(bool aSucceeded, bool aCancelled, std::vector<Transition>& aOut) noexcept
    {
        m_worldLoading = false;
        Transition t = Make(Signal::kWorldLoadEnd);
        t.Online = m_link == Link::kOnline;
        t.Succeeded = aSucceeded;
        t.Cancelled = aCancelled;
        aOut.push_back(t);
    }

    [[nodiscard]] Transition Make(Signal aKind) const noexcept
    {
        Transition t{};
        t.Kind = aKind;
        t.SessionGeneration = m_link == Link::kOnline ? m_sessionGeneration : 0u;
        t.WorldGeneration = m_worldGeneration;
        return t;
    }

    Link m_link{Link::kOffline};
    bool m_worldLoading{false};
    bool m_paused{false};
    uint32_t m_sessionGeneration{0};
    uint32_t m_worldGeneration{0};
    Counters m_counters{};
};

// ---------------------------------------------------------------------------------------------
// Participants and dispatch
// ---------------------------------------------------------------------------------------------

// A subsystem with session- or world-scoped state. Handlers run on the thread that drives the
// lifecycle (the client's WinMain thread); they must not assume any other participant has already
// handled the same transition unless the participant order says so.
class IParticipant
{
public:
    virtual ~IParticipant() = default;

    virtual void OnSessionBegin(const Transition& acTransition) noexcept { (void)acTransition; }
    virtual void OnSessionEnd(const Transition& acTransition) noexcept { (void)acTransition; }
    virtual void OnWorldLoadBegin(const Transition& acTransition) noexcept { (void)acTransition; }
    virtual void OnWorldLoadEnd(const Transition& acTransition) noexcept { (void)acTransition; }
};

class Lifecycle
{
public:
    using Observer = void (*)(void* apContext, const Transition& acTransition, Phase aPhaseAfter);

    Lifecycle()
    {
        // An input produces at most two transitions and nesting is shallow (a teardown closing the
        // socket): with this reserve Feed() never reallocates in practice.
        m_pending.reserve(8);
        PublishToken();
    }

    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;

    // Returns false (and keeps the first registration) when the slot is taken or the id invalid.
    bool Register(ParticipantId aId, IParticipant& aParticipant) noexcept
    {
        const auto index = static_cast<size_t>(aId);
        if (index >= kParticipantCount || m_participants[index] != nullptr)
            return false;
        m_participants[index] = &aParticipant;
        return true;
    }

    [[nodiscard]] bool IsRegistered(ParticipantId aId) const noexcept
    {
        const auto index = static_cast<size_t>(aId);
        return index < kParticipantCount && m_participants[index] != nullptr;
    }

    [[nodiscard]] std::vector<ParticipantId> MissingParticipants() const
    {
        std::vector<ParticipantId> missing;
        for (const auto& info : kParticipantTable)
        {
            if (!IsRegistered(info.Id))
                missing.push_back(info.Id);
        }
        return missing;
    }

    void SetObserver(Observer aObserver, void* apContext) noexcept
    {
        m_observer = aObserver;
        m_observerContext = apContext;
    }

    // ---- inputs (one thread) ----
    void OnTransportUp() noexcept { Feed([](Machine& m, std::vector<Transition>& out) { m.TransportUp(out); }); }
    void OnAuthenticationReceived() noexcept
    {
        Feed([](Machine& m, std::vector<Transition>& out) { m.AuthenticationReceived(out); });
    }
    void OnAuthenticated(uint32_t aLocalPlayerId) noexcept
    {
        Feed([aLocalPlayerId](Machine& m, std::vector<Transition>& out) { m.Authenticated(aLocalPlayerId, out); });
    }
    void OnTransportDown() noexcept { Feed([](Machine& m, std::vector<Transition>& out) { m.TransportDown(out); }); }
    void OnWorldLoadBegin(bool aLiveReload) noexcept
    {
        Feed([aLiveReload](Machine& m, std::vector<Transition>& out) { m.WorldLoadBegin(aLiveReload, out); });
    }
    void OnWorldLoadEnd(bool aSucceeded) noexcept
    {
        Feed([aSucceeded](Machine& m, std::vector<Transition>& out) { m.WorldLoadEnd(aSucceeded, out); });
    }
    void OnWorldLoadCancel() noexcept { Feed([](Machine& m, std::vector<Transition>& out) { m.WorldLoadCancel(out); }); }
    bool SetPaused(bool aPaused) noexcept { return m_machine.SetPaused(aPaused); }

    // ---- queries (lifecycle thread) ----
    [[nodiscard]] const Machine& GetMachine() const noexcept { return m_machine; }
    [[nodiscard]] Phase GetPhase() const noexcept { return m_machine.GetPhase(); }
    [[nodiscard]] bool IsDispatching() const noexcept { return m_dispatching; }
    [[nodiscard]] uint64_t GetDeliveredCount() const noexcept { return m_delivered; }

    // ---- queries (any thread) ----
    // The token as of the last completed input. Game-thread hooks capture it before marshalling work
    // to the main thread; the work then checks IsStillCurrent* there.
    [[nodiscard]] Token GetTokenAnyThread() const noexcept
    {
        return Token::Unpack(m_publishedToken.load(std::memory_order_acquire));
    }
    // True from the first world load begin until its end/cancel, already while the teardown is being
    // delivered (the machine changes state before dispatch), so every gate closes before any teardown.
    [[nodiscard]] bool IsWorldLoadingAnyThread() const noexcept
    {
        return m_publishedWorldLoading.load(std::memory_order_acquire);
    }

    // ---- staleness checks (lifecycle thread) ----
    [[nodiscard]] bool IsStillCurrentSession(Token aToken) const noexcept { return m_machine.IsCurrentSession(aToken); }
    [[nodiscard]] bool IsStillCurrentWorld(Token aToken) const noexcept { return m_machine.IsCurrentWorld(aToken); }

private:
    template <class TInput> void Feed(TInput&& aInput) noexcept
    {
        // The machine sees every input immediately (queries made by a participant during dispatch
        // reflect the latest state); the transitions it produced are delivered in FIFO order.
        aInput(m_machine, m_pending);
        PublishToken();
        if (m_dispatching)
            return;  // the outer Feed delivers them after the transition in progress

        m_dispatching = true;
        for (size_t next = 0; next < m_pending.size(); ++next)
        {
            const Transition transition = m_pending[next];  // copy: m_pending may grow during delivery
            Deliver(transition);
        }
        m_pending.clear();
        m_dispatching = false;
    }

    void Deliver(const Transition& acTransition) noexcept
    {
        ++m_delivered;
        if (IsTeardown(acTransition.Kind))
        {
            for (size_t i = kParticipantCount; i-- > 0;)
                Call(m_participants[i], acTransition);
        }
        else
        {
            for (size_t i = 0; i < kParticipantCount; ++i)
                Call(m_participants[i], acTransition);
        }

        if (m_observer)
            m_observer(m_observerContext, acTransition, m_machine.GetPhase());
    }

    static void Call(IParticipant* apParticipant, const Transition& acTransition) noexcept
    {
        if (!apParticipant)
            return;
        switch (acTransition.Kind)
        {
        case Signal::kSessionBegin: apParticipant->OnSessionBegin(acTransition); break;
        case Signal::kSessionEnd: apParticipant->OnSessionEnd(acTransition); break;
        case Signal::kWorldLoadBegin: apParticipant->OnWorldLoadBegin(acTransition); break;
        case Signal::kWorldLoadEnd: apParticipant->OnWorldLoadEnd(acTransition); break;
        }
    }

    void PublishToken() noexcept
    {
        m_publishedToken.store(m_machine.GetToken().Pack(), std::memory_order_release);
        m_publishedWorldLoading.store(m_machine.IsWorldLoading(), std::memory_order_release);
    }

    Machine m_machine{};
    std::array<IParticipant*, kParticipantCount> m_participants{};
    std::vector<Transition> m_pending{};
    bool m_dispatching{false};
    uint64_t m_delivered{0};
    std::atomic<uint64_t> m_publishedToken{0};
    std::atomic<bool> m_publishedWorldLoading{false};
    Observer m_observer{nullptr};
    void* m_observerContext{nullptr};
};

[[nodiscard]] constexpr std::string_view ToString(Phase aPhase) noexcept
{
    switch (aPhase)
    {
    case Phase::kOffline: return "offline";
    case Phase::kJoining: return "joining";
    case Phase::kInWorld: return "in world";
    case Phase::kPaused: return "paused";
    case Phase::kReloading: return "reloading";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view ToString(Signal aSignal) noexcept
{
    switch (aSignal)
    {
    case Signal::kSessionBegin: return "session begin";
    case Signal::kSessionEnd: return "session end";
    case Signal::kWorldLoadBegin: return "world load begin";
    case Signal::kWorldLoadEnd: return "world load end";
    }
    return "?";
}
} // namespace Session
