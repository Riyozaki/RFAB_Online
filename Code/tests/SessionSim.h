#pragma once

/**
 * A deterministic model of a running session.
 *
 * Unit tests pin one decision at a time. The failures that actually ended sessions were
 * never one decision: a guest claimed a dragon while the host was still loading its save,
 * an unloading client's packet arrived after the server had already reassigned the boss,
 * a checkpoint was committed while someone was mid-disconnect. Those need several peers,
 * a clock, and a network that does not deliver everything instantly.
 *
 * This harness runs one server and N peers over a reliable, ordered, delayed channel -
 * the same guarantees the real transport gives - and drives them through the modules the
 * server and client actually use: Authority::Lease for who simulates what, and
 * Save::Coordinator for checkpoints and world reloads. Nothing here is a mock: the
 * decisions come from the shipping code, only the clock, the queue and the Skyrim side
 * are modelled.
 *
 * Everything is seeded, so a failing soak run reprints the seed and the whole message log.
 */

#include <Authority/Lease.h>
#include <Authority/Health.h>
#include <Authority/Hits.h>
#include <Save/Barrier.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace SessionSim
{
inline constexpr uint64_t kTickMs = 50ull;
inline constexpr uint32_t kServerId = 0u;

enum class MsgKind : uint8_t
{
    kClaimRequest,  // peer -> server: I would like to simulate this entity
    kReleaseRequest,// peer -> server: I am giving it back (cell unload, teleport)
    kLeaseUpdate,   // server -> peer: this is who owns it now, at this epoch
    kSavePrepare,   // server -> peer: write your save
    kSaveAck,       // peer -> server: written / refused
    kReloadBegin,   // server -> peer: load this checkpoint
    kReloadReady,   // peer -> server: loaded
    kDamageRequest, // peer -> server: I hit this for N
    kHealthUpdate,  // server -> peer: this is its health now
};

struct Message
{
    MsgKind Kind{};
    uint32_t From{};
    uint32_t To{};
    uint32_t EntityId{};
    Authority::Lease Lease{};
    uint64_t SaveEpoch{};
    uint64_t SenderEpoch{};
    float Delta{0.0f};
    float Health{0.0f};
    bool Success{true};
    uint64_t DeliverAtMs{};
    uint64_t Order{};
};

struct Peer
{
    uint32_t Id{};
    bool Connected{true};
    bool InRange{true};
    bool Downed{false};
    bool IsHost{false};
    bool RefusesSave{false};
    bool StuckLoading{false};
    Authority::LeaseTable Leases{};      // what this peer believes about the world
    std::map<uint32_t, float> KnownHealth{}; // and how hurt it thinks each entity is
};

struct Entity
{
    uint32_t Id{};
    bool IsPlayerAvatar{false};
    uint32_t PinnedTo{Authority::kNoOwner}; // avatars belong to exactly one peer
};

// Everything the harness noticed going wrong. Empty means the session held together.
struct Violations
{
    std::vector<std::string> Entries{};

    // A broken invariant usually stays broken for every later tick; keep the first few
    // of each kind so a failure prints a diagnosis instead of a wall of text.
    void Add(uint64_t aNowMs, const std::string& acText)
    {
        if (Seen[acText]++ > 0 || Entries.size() >= 20)
            return;

        std::ostringstream out;
        out << "t=" << aNowMs << "ms " << acText;
        Entries.push_back(out.str());
    }

    std::map<std::string, uint32_t> Seen{};

    [[nodiscard]] bool Empty() const noexcept { return Entries.empty(); }
};

class Session
{
public:
    Session(uint32_t aPeerCount, uint32_t aWorldEntities, uint32_t aSeed)
        : m_rng(aSeed | 1u)
    {
        for (uint32_t i = 0; i < aPeerCount; ++i)
        {
            Peer peer{};
            peer.Id = i + 1u;
            peer.IsHost = (i == 0);
            m_peers.push_back(peer);
        }

        uint32_t entityId = 100u;
        for (uint32_t i = 0; i < aWorldEntities; ++i)
        {
            m_health[entityId] = kMaxHealth;
            m_entities.push_back(Entity{entityId++, false, Authority::kNoOwner});
        }

        // One avatar per peer: these must never change hands, whatever happens.
        for (const auto& peer : m_peers)
        {
            m_entities.push_back(Entity{entityId, true, peer.Id});
            m_leases[entityId] = Authority::Lease{peer.Id, 1ull, 0ull, 0ull};
            for (auto& p : m_peers)
                p.Leases.Apply(entityId, m_leases[entityId]);
            ++entityId;
        }
    }

    [[nodiscard]] uint64_t NowMs() const noexcept { return m_nowMs; }
    [[nodiscard]] const Violations& Problems() const noexcept { return m_violations; }
    [[nodiscard]] const std::vector<std::string>& Log() const noexcept { return m_log; }
    [[nodiscard]] std::vector<Peer>& Peers() noexcept { return m_peers; }
    [[nodiscard]] Save::Coordinator& Saves() noexcept { return m_save; }
    [[nodiscard]] uint64_t CommitCount() const noexcept { return m_commits; }
    [[nodiscard]] uint64_t AbortCount() const noexcept { return m_aborts; }

    [[nodiscard]] uint32_t ServerOwnerOf(uint32_t aEntityId) const
    {
        const auto it = m_leases.find(aEntityId);
        return it == m_leases.end() ? Authority::kNoOwner : it->second.OwnerId;
    }

    // ---------------------------------------------------------------- peer actions
    void Claim(uint32_t aPeerId, uint32_t aEntityId) { Send(MsgKind::kClaimRequest, aPeerId, kServerId, aEntityId); }

    void Release(uint32_t aPeerId, uint32_t aEntityId)
    {
        Peer* pPeer = FindPeer(aPeerId);
        if (!pPeer)
            return;

        Message message{};
        message.Kind = MsgKind::kReleaseRequest;
        message.EntityId = aEntityId;
        message.SenderEpoch = pPeer->Leases.EpochOf(aEntityId);
        Send(message, aPeerId, kServerId);
    }

    // The client told the server it cannot take this entity (failed to load the actor).
    void RecordDecline(uint32_t aPeerId, uint32_t aEntityId) { m_declined[aEntityId].push_back(aPeerId); }

    // A peer reports a hit. Whether it is allowed to is the server's problem.
    void Damage(uint32_t aPeerId, uint32_t aEntityId, float aDelta)
    {
        Message message{};
        message.Kind = MsgKind::kDamageRequest;
        message.EntityId = aEntityId;
        message.Delta = aDelta;
        Send(message, aPeerId, kServerId);
    }

    [[nodiscard]] float ServerHealthOf(uint32_t aEntityId) const
    {
        const auto it = m_health.find(aEntityId);
        return it == m_health.end() ? 0.0f : it->second;
    }

    void SetConnected(uint32_t aPeerId, bool aConnected)
    {
        Peer* pPeer = FindPeer(aPeerId);
        if (!pPeer || pPeer->Connected == aConnected)
            return;

        pPeer->Connected = aConnected;
        if (!aConnected)
        {
            // Everything in flight to or from a dropped peer is gone.
            m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                         [aPeerId](const Message& m)
                                         { return m.From == aPeerId || m.To == aPeerId; }),
                          m_queue.end());
            pPeer->Leases.Clear();
            pPeer->KnownHealth.clear();
            for (const auto& entity : m_entities)
                m_peerEpochSeen.erase((static_cast<uint64_t>(aPeerId) << 32) | entity.Id);

            OnPeerLost(aPeerId);
        }

        if (aConnected)
        {
            // Join sync: a peer that just arrived is told who owns what and how hurt
            // everything is - the same state the real spawn packets carry. Without the
            // health half, a reconnecting player keeps fighting a boss the rest of the
            // party already brought down.
            for (const auto& [entityId, lease] : m_leases)
            {
                Message message{};
                message.Kind = MsgKind::kLeaseUpdate;
                message.EntityId = entityId;
                message.Lease = lease;
                Send(message, kServerId, aPeerId);
            }

            for (const auto& [entityId, health] : m_health)
            {
                Message message{};
                message.Kind = MsgKind::kHealthUpdate;
                message.EntityId = entityId;
                message.Health = health;
                Send(message, kServerId, aPeerId);
            }
        }

        Note(aConnected ? "peer %u reconnected" : "peer %u disconnected", aPeerId);
    }

    void SetInRange(uint32_t aPeerId, bool aInRange)
    {
        if (Peer* pPeer = FindPeer(aPeerId))
            pPeer->InRange = aInRange;
    }

    void SetDowned(uint32_t aPeerId, bool aDowned)
    {
        if (Peer* pPeer = FindPeer(aPeerId))
            pPeer->Downed = aDowned;
    }

    // ---------------------------------------------------------------- server actions
    bool RequestSave(bool aIsAuto)
    {
        std::vector<uint32_t> connected;
        for (const auto& peer : m_peers)
        {
            if (peer.Connected)
                connected.push_back(peer.Id);
        }

        const auto started = m_save.BeginBarrier(connected, aIsAuto, m_nowMs);
        if (!started.has_value())
            return false;

        m_expectedAcks = started->ExpectedPlayerIds;
        m_receivedAcks.clear();
        for (const uint32_t id : connected)
        {
            Message message{};
            message.Kind = MsgKind::kSavePrepare;
            message.SaveEpoch = started->EpochId;
            Send(message, kServerId, id);
        }

        Note("save barrier %llu opened", static_cast<unsigned long long>(started->EpochId));
        return true;
    }

    void RollBack(uint64_t aEpochId)
    {
        m_save.NotifyRolledBackTo(aEpochId, "slot", m_nowMs);

        std::vector<uint32_t> connected;
        for (const auto& peer : m_peers)
        {
            if (peer.Connected)
                connected.push_back(peer.Id);
        }

        const uint32_t hostId = m_peers.empty() ? 0u : m_peers.front().Id;
        m_save.BeginReloadBarrier(connected, hostId, aEpochId, "slot", m_nowMs);

        for (const uint32_t id : connected)
        {
            Message message{};
            message.Kind = MsgKind::kReloadBegin;
            message.SaveEpoch = aEpochId;
            Send(message, kServerId, id);
        }

        Note("rollback to %llu, world generation %llu", static_cast<unsigned long long>(aEpochId),
             static_cast<unsigned long long>(m_save.GetCurrentWorldEpoch()));
    }

    // ---------------------------------------------------------------- the loop
    void Tick()
    {
        m_nowMs += kTickMs;
        DeliverDue();
        ServerHousekeeping();
        CheckInvariants();
    }

    // Runs until the wire is empty, no gate is open and nothing has changed hands for
    // longer than the dormant settle window - otherwise "the entity has no owner" would
    // just mean "the server has not got round to it yet".
    void Quiesce(uint32_t aMaxTicks = 4000)
    {
        const uint32_t quietTicksNeeded =
            static_cast<uint32_t>((m_policy.DormantSettleMs / kTickMs) + 4ull);
        uint32_t quietTicks = 0;

        for (uint32_t i = 0; i < aMaxTicks; ++i)
        {
            const bool busy = !m_queue.empty() || m_save.HasPendingBarrier() || m_save.HasActiveReloadBarrier();
            m_changedThisTick = false;
            Tick();

            quietTicks = (busy || m_changedThisTick) ? 0u : quietTicks + 1u;
            if (quietTicks >= quietTicksNeeded)
                return;
        }

        m_violations.Add(m_nowMs, "session never settled: the queue or a barrier stayed busy");
    }

    uint32_t Random(uint32_t aBound) { return aBound == 0 ? 0u : NextRandom() % aBound; }

    [[nodiscard]] const std::vector<Entity>& Entities() const noexcept { return m_entities; }

    // Every peer that believes it is simulating this entity right now.
    [[nodiscard]] std::vector<uint32_t> BelieversFor(uint32_t aEntityId) const
    {
        std::vector<uint32_t> believers;
        for (const auto& peer : m_peers)
        {
            if (peer.Connected && peer.Leases.IsOwnedBy(aEntityId, peer.Id))
                believers.push_back(peer.Id);
        }

        return believers;
    }

private:
    uint32_t NextRandom() noexcept
    {
        m_rng ^= m_rng << 13;
        m_rng ^= m_rng >> 17;
        m_rng ^= m_rng << 5;
        return m_rng;
    }

    Peer* FindPeer(uint32_t aPeerId)
    {
        for (auto& peer : m_peers)
        {
            if (peer.Id == aPeerId)
                return &peer;
        }

        return nullptr;
    }

    const Entity* FindEntity(uint32_t aEntityId) const
    {
        for (const auto& entity : m_entities)
        {
            if (entity.Id == aEntityId)
                return &entity;
        }

        return nullptr;
    }

    void Note(const char* apFormat, ...);

    void Send(MsgKind aKind, uint32_t aFrom, uint32_t aTo, uint32_t aEntityId)
    {
        Message message{};
        message.Kind = aKind;
        message.EntityId = aEntityId;
        Send(message, aFrom, aTo);
    }

    // Reliable and ordered per direction, like the real transport: jitter can reorder
    // messages between different peers but never within one channel. Duplicates happen.
    void Send(Message aMessage, uint32_t aFrom, uint32_t aTo)
    {
        aMessage.From = aFrom;
        aMessage.To = aTo;

        const uint64_t latency = 20ull + (NextRandom() % 180ull);
        const uint64_t pairKey = (static_cast<uint64_t>(aFrom) << 32) | aTo;
        uint64_t deliverAt = m_nowMs + latency;
        auto& lastForPair = m_lastDelivery[pairKey];
        if (deliverAt <= lastForPair)
            deliverAt = lastForPair + 1ull;

        lastForPair = deliverAt;
        aMessage.DeliverAtMs = deliverAt;
        aMessage.Order = ++m_sequence;
        m_queue.push_back(aMessage);

        if ((NextRandom() % 50u) == 0u) // ~2%: the same packet twice
        {
            Message duplicate = aMessage;
            duplicate.DeliverAtMs = deliverAt + 1ull;
            duplicate.Order = ++m_sequence;
            m_queue.push_back(duplicate);
        }
    }

    void DeliverDue()
    {
        std::sort(m_queue.begin(), m_queue.end(),
                  [](const Message& a, const Message& b)
                  { return a.DeliverAtMs != b.DeliverAtMs ? a.DeliverAtMs < b.DeliverAtMs : a.Order < b.Order; });

        std::vector<Message> due;
        while (!m_queue.empty() && m_queue.front().DeliverAtMs <= m_nowMs)
        {
            due.push_back(m_queue.front());
            m_queue.erase(m_queue.begin());
        }

        for (const auto& message : due)
        {
            if (message.To == kServerId)
                ServerReceive(message);
            else
                PeerReceive(message);
        }
    }

    void ServerReceive(const Message& acMessage)
    {
        Peer* pSender = FindPeer(acMessage.From);
        if (!pSender || !pSender->Connected)
            return;

        switch (acMessage.Kind)
        {
        case MsgKind::kClaimRequest:
            HandleClaim(*pSender, acMessage.EntityId);
            break;

        case MsgKind::kReleaseRequest:
            HandleRelease(*pSender, acMessage);
            break;

        case MsgKind::kSaveAck:
            HandleSaveAck(*pSender, acMessage);
            break;

        case MsgKind::kDamageRequest:
            HandleDamage(*pSender, acMessage);
            break;

        case MsgKind::kReloadReady:
            if (m_save.RecordReloadPlayerReady(acMessage.From, acMessage.SaveEpoch))
                Note("host gate released by peer %u", acMessage.From);
            break;

        default:
            break;
        }
    }

    void PeerReceive(const Message& acMessage)
    {
        Peer* pPeer = FindPeer(acMessage.To);
        if (!pPeer || !pPeer->Connected)
            return;

        switch (acMessage.Kind)
        {
        case MsgKind::kLeaseUpdate:
            pPeer->Leases.Apply(acMessage.EntityId, acMessage.Lease);
            break;

        case MsgKind::kHealthUpdate:
            pPeer->KnownHealth[acMessage.EntityId] = acMessage.Health;
            break;

        case MsgKind::kSavePrepare:
        {
            Message ack{};
            ack.Kind = MsgKind::kSaveAck;
            ack.SaveEpoch = acMessage.SaveEpoch;
            ack.Success = !pPeer->RefusesSave;
            Send(ack, pPeer->Id, kServerId);
            break;
        }

        case MsgKind::kReloadBegin:
        {
            if (pPeer->StuckLoading)
                break; // models a client wedged loading its .ess

            Message ready{};
            ready.Kind = MsgKind::kReloadReady;
            ready.SaveEpoch = acMessage.SaveEpoch;
            Send(ready, pPeer->Id, kServerId);
            break;
        }

        default:
            break;
        }
    }

    Authority::Subject SubjectOf(uint32_t aEntityId) const
    {
        Authority::Subject subject{};
        if (const Entity* pEntity = FindEntity(aEntityId))
            subject.IsPlayerAvatar = pEntity->IsPlayerAvatar;

        return subject;
    }

    Authority::OwnerState OwnerStateOf(const Authority::Lease& acLease)
    {
        Authority::OwnerState state{};
        const Peer* pOwner = FindPeer(acLease.OwnerId);
        state.Connected = pOwner != nullptr && pOwner->Connected;
        state.InRange = pOwner != nullptr && pOwner->InRange;
        return state;
    }

    static Authority::Candidate CandidateOf(const Peer& acPeer)
    {
        Authority::Candidate candidate{};
        candidate.PlayerId = acPeer.Id;
        candidate.IsDowned = acPeer.Downed;
        candidate.InRange = acPeer.InRange;
        candidate.IsPartyLeader = acPeer.IsHost;
        return candidate;
    }

    void HandleClaim(const Peer& acSender, uint32_t aEntityId)
    {
        // The reload gate: while the host is still loading, a guest must not take
        // anything, or the world is simulated twice from two different checkpoints.
        if (m_save.ShouldHoldNonHostWorldClaims(acSender.Id))
        {
            ++m_claimsHeldByGate;
            return;
        }

        auto& lease = m_leases[aEntityId];
        const auto decision = Authority::Claim(lease, SubjectOf(aEntityId), OwnerStateOf(lease),
                                               CandidateOf(acSender), m_nowMs, m_policy);
        if (!decision.Changed)
            return;

        lease = decision.Next;
        BroadcastLease(aEntityId, lease);
    }

    void HandleRelease(const Peer& acSender, const Message& acMessage)
    {
        auto& lease = m_leases[acMessage.EntityId];
        const auto decision =
            Authority::Release(lease, SubjectOf(acMessage.EntityId), acSender.Id, acMessage.SenderEpoch, m_nowMs);
        if (!decision.Changed)
            return;

        // Handing an entity back is not the same as refusing it: a peer that unloads a
        // cell may well be the right owner again in a second. Only an explicit refusal
        // (the client could not load the actor) goes on the declined list.
        lease = decision.Next;
        BroadcastLease(acMessage.EntityId, lease);
    }

    void HandleSaveAck(const Peer& acSender, const Message& acMessage)
    {
        if (acMessage.Success)
            m_receivedAcks.insert(acSender.Id);

        const auto resolution = m_save.RecordClientAck(acSender.Id, acMessage.SaveEpoch, acMessage.Success,
                                                       acMessage.Success ? "" : "client refused");
        if (resolution == Save::Coordinator::AckResolution::ReadyToCommit)
        {
            CommitSave();
        }
        else if (resolution == Save::Coordinator::AckResolution::Aborted)
        {
            m_save.ConsumeAborted(m_nowMs);
            ++m_aborts;
            Note("save barrier aborted");
        }
    }

    void HandleDamage(const Peer& acSender, const Message& acMessage)
    {
        const auto healthIt = m_health.find(acMessage.EntityId);
        if (healthIt == m_health.end())
            return;

        const auto leaseIt = m_leases.find(acMessage.EntityId);
        const bool senderOwnsIt = leaseIt != m_leases.end() && leaseIt->second.OwnerId == acSender.Id;

        // A peer that does not simulate the target still gets to hit it, but the server
        // decides whether to believe the report.
        if (!senderOwnsIt)
        {
            m_combat.RecordCombatClaim(acSender.Id, acMessage.EntityId,
                                       Authority::HitKind::kMeleeOrDamage, m_nowMs);

            const bool accepted = m_combat.ValidateNonOwnerHealthDelta(
                acSender.Id, acMessage.EntityId, acMessage.Delta, acSender.InRange, acSender.Downed,
                healthIt->second <= 0.0f, /*aAreCoopPartyAllies=*/false, /*aIsPvpEnabled=*/false, m_nowMs);
            if (!accepted)
            {
                ++m_rejectedHits;
                return;
            }
        }

        const auto applied =
            Authority::ApplyHealthDelta(healthIt->second, kMaxHealth, acMessage.Delta);

        if (applied.UpdatedHealth > healthIt->second && acMessage.Delta < 0.0f)
        {
            std::ostringstream out;
            out << "entity " << acMessage.EntityId << " healed from damage: " << healthIt->second << " -> "
                << applied.UpdatedHealth;
            m_violations.Add(m_nowMs, out.str());
        }

        healthIt->second = applied.UpdatedHealth;
        m_changedThisTick = true;

        for (const auto& peer : m_peers)
        {
            if (!peer.Connected)
                continue;

            Message update{};
            update.Kind = MsgKind::kHealthUpdate;
            update.EntityId = acMessage.EntityId;
            update.Health = applied.UpdatedHealth;
            Send(update, kServerId, peer.Id);
        }
    }

    void CommitSave()
    {
        // Before trusting the coordinator, check the thing players care about: the
        // checkpoint must not be recorded unless everyone still here actually wrote it.
        for (const uint32_t expected : m_expectedAcks)
        {
            const Peer* pPeer = nullptr;
            for (const auto& peer : m_peers)
            {
                if (peer.Id == expected)
                    pPeer = &peer;
            }

            if (pPeer && pPeer->Connected && m_receivedAcks.count(expected) == 0)
            {
                std::ostringstream out;
                out << "committed a checkpoint without peer " << expected << "'s save";
                m_violations.Add(m_nowMs, out.str());
            }
        }

        const auto committed = m_save.ConsumeAndCommit(m_nowMs);
        if (committed.EpochId <= m_lastCommittedEpoch)
        {
            std::ostringstream out;
            out << "checkpoint epoch went backwards: " << committed.EpochId << " after " << m_lastCommittedEpoch;
            m_violations.Add(m_nowMs, out.str());
        }

        m_lastCommittedEpoch = committed.EpochId;
        ++m_commits;
        Note("checkpoint %llu committed", static_cast<unsigned long long>(committed.EpochId));
    }

    void BroadcastLease(uint32_t aEntityId, const Authority::Lease& acLease)
    {
        m_changedThisTick = true;

        for (const auto& peer : m_peers)
        {
            if (!peer.Connected)
                continue;

            Message message{};
            message.Kind = MsgKind::kLeaseUpdate;
            message.EntityId = aEntityId;
            message.Lease = acLease;
            Send(message, kServerId, peer.Id);
        }
    }

    void OnPeerLost(uint32_t aPeerId)
    {
        const auto resolution = m_save.HandlePlayerDisconnected(aPeerId);
        if (resolution == Save::Coordinator::AckResolution::ReadyToCommit)
            CommitSave();
        else if (resolution == Save::Coordinator::AckResolution::Aborted)
        {
            m_save.ConsumeAborted(m_nowMs);
            ++m_aborts;
        }

        m_save.HandlePlayerDisconnectedFromReload(aPeerId);
        m_expectedAcks.erase(aPeerId);
        m_receivedAcks.erase(aPeerId);
    }

    void ServerHousekeeping()
    {
        if (m_save.HasBarrierTimedOut(m_nowMs))
        {
            m_save.ConsumeAborted(m_nowMs);
            ++m_aborts;
            Note("save barrier timed out");
        }

        if (m_save.TickReloadBarrier(m_nowMs))
            Note("host gate released by timeout");

        for (const auto& entity : m_entities)
        {
            if (entity.IsPlayerAvatar)
                continue;

            auto& lease = m_leases[entity.Id];

            // An owner that left or walked away is not an owner. Take it back first.
            if (lease.HasOwner())
            {
                const Peer* pOwner = FindPeer(lease.OwnerId);
                const bool canSimulate = pOwner && pOwner->Connected && pOwner->InRange;
                if (!canSimulate)
                {
                    const auto revoked = Authority::Revoke(lease, m_nowMs);
                    if (revoked.Changed)
                    {
                        lease = revoked.Next;
                        BroadcastLease(entity.Id, lease);
                    }
                }
            }

            if (lease.HasOwner() || !Authority::CanAdoptDormant(lease, m_nowMs, m_policy))
                continue;

            // Nobody owns it and it has settled: offer it to whoever can take it.
            std::vector<Authority::Candidate> candidates;
            for (const auto& peer : m_peers)
            {
                if (peer.Connected && !m_save.ShouldHoldNonHostWorldClaims(peer.Id))
                    candidates.push_back(CandidateOf(peer));
            }

            const auto reassignment = Authority::Reassign(lease, SubjectOf(entity.Id), candidates,
                                                          m_declined[entity.Id], m_nowMs, m_policy);
            if (reassignment.Result == Authority::Outcome::kGranted && reassignment.Changed)
            {
                lease = reassignment.Next;
                m_declined[entity.Id].clear();
                BroadcastLease(entity.Id, lease);
            }
        }
    }

    void CheckInvariants()
    {
        for (const auto& entity : m_entities)
        {
            const auto leaseIt = m_leases.find(entity.Id);
            const Authority::Lease serverLease = leaseIt == m_leases.end() ? Authority::Lease{} : leaseIt->second;

            if (entity.IsPlayerAvatar && serverLease.OwnerId != entity.PinnedTo)
            {
                std::ostringstream out;
                out << "avatar " << entity.Id << " changed hands: owner " << serverLease.OwnerId << " instead of "
                    << entity.PinnedTo;
                m_violations.Add(m_nowMs, out.str());
            }

            uint32_t believersAtCurrentEpoch = 0;
            for (const auto& peer : m_peers)
            {
                if (!peer.Connected)
                    continue;

                const uint64_t peerEpoch = peer.Leases.EpochOf(entity.Id);
                if (peerEpoch > serverLease.Epoch)
                {
                    std::ostringstream out;
                    out << "peer " << peer.Id << " is ahead of the server on entity " << entity.Id << ": epoch "
                        << peerEpoch << " > " << serverLease.Epoch;
                    m_violations.Add(m_nowMs, out.str());
                }

                auto& known = m_peerEpochSeen[(static_cast<uint64_t>(peer.Id) << 32) | entity.Id];
                if (peerEpoch < known)
                {
                    std::ostringstream out;
                    out << "peer " << peer.Id << " went backwards on entity " << entity.Id << ": epoch " << peerEpoch
                        << " after " << known;
                    m_violations.Add(m_nowMs, out.str());
                }

                known = peerEpoch;

                if (peer.Leases.IsOwnedBy(entity.Id, peer.Id) && peerEpoch == serverLease.Epoch)
                    ++believersAtCurrentEpoch;
            }

            const auto serverHealthIt = m_health.find(entity.Id);
            if (serverHealthIt != m_health.end())
            {
                if (serverHealthIt->second < 0.0f || serverHealthIt->second > kMaxHealth)
                {
                    std::ostringstream out;
                    out << "entity " << entity.Id << " has impossible health " << serverHealthIt->second;
                    m_violations.Add(m_nowMs, out.str());
                }
            }

            // Two peers simulating the same entity at the same epoch is the bug that
            // makes an NPC fight itself; a stale believer waiting for an update is not.
            if (believersAtCurrentEpoch > 1)
            {
                std::ostringstream out;
                out << "entity " << entity.Id << " has " << believersAtCurrentEpoch
                    << " simulators at epoch " << serverLease.Epoch;
                m_violations.Add(m_nowMs, out.str());
            }
        }
    }

    uint32_t m_rng{1};
    uint64_t m_nowMs{0};
    uint64_t m_sequence{0};
    uint64_t m_lastCommittedEpoch{0};
    uint64_t m_commits{0};
    uint64_t m_aborts{0};
    uint64_t m_claimsHeldByGate{0};
    bool m_changedThisTick{false};

    static constexpr float kMaxHealth = 100.0f;

    Authority::Policy m_policy{};
    Authority::HitLedger<> m_combat{};
    std::map<uint32_t, float> m_health{};
    uint64_t m_rejectedHits{0};
    Save::Coordinator m_save{"SIM", 3};

    std::vector<Peer> m_peers{};
    std::vector<Entity> m_entities{};
    std::vector<Message> m_queue{};
    std::map<uint32_t, Authority::Lease> m_leases{};
    std::map<uint32_t, std::vector<uint32_t>> m_declined{};
    std::map<uint64_t, uint64_t> m_lastDelivery{};
    std::map<uint64_t, uint64_t> m_peerEpochSeen{};
    std::unordered_set<uint32_t> m_expectedAcks{};
    std::unordered_set<uint32_t> m_receivedAcks{};
    Violations m_violations{};
    std::vector<std::string> m_log{};

public:
    [[nodiscard]] uint64_t ClaimsHeldByGate() const noexcept { return m_claimsHeldByGate; }
    [[nodiscard]] uint64_t RejectedHits() const noexcept { return m_rejectedHits; }

    // Everyone who is connected must end up believing the server about how hurt this is.
    [[nodiscard]] bool HealthAgreedOn(uint32_t aEntityId) const
    {
        const auto it = m_health.find(aEntityId);
        if (it == m_health.end())
            return true;

        for (const auto& peer : m_peers)
        {
            const auto known = peer.KnownHealth.find(aEntityId);
            if (!peer.Connected || known == peer.KnownHealth.end())
                continue;

            if (known->second != it->second)
                return false;
        }

        return true;
    }
};

inline void Session::Note(const char* apFormat, ...)
{
    // Kept deliberately simple: the log exists to be printed next to a failing seed.
    char buffer[256];
    va_list args;
    va_start(args, apFormat);
    std::vsnprintf(buffer, sizeof(buffer), apFormat, args);
    va_end(args);

    std::ostringstream out;
    out << "t=" << m_nowMs << "ms " << buffer;
    m_log.push_back(out.str());
}

} // namespace SessionSim
