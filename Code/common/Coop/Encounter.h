#pragma once
#include <Structs/CoopProtocol.h>
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <limits>

namespace Coop
{
inline bool SameSpace(const Pose& a, const Pose& b) noexcept
{
    return a.Valid() && b.Valid() && (a.World ? a.World == b.World : !b.World && a.Cell == b.Cell);
}
inline double DistanceSquared(const Pose& a, const Pose& b) noexcept
{
    if (!SameSpace(a, b))
        return std::numeric_limits<double>::infinity();
    const double x = static_cast<double>(a.X) - b.X, y = static_cast<double>(a.Y) - b.Y, z = static_cast<double>(a.Z) - b.Z;
    return x*x + y*y + z*z;
}

// Per-party state machine. Wall-clock time is supplied by the server (monotonic milliseconds),
// never by a packet. This same class executes in production and in deterministic fault tests.
class EncounterRuntime
{
public:
    struct Member
    {
        uint32_t Actor{}, Flags{}, Revision{}, HitSerial{};
        uint64_t Seen{}, SafeSince{};
        bool Connected{true}, Downed{};
        Pose Location, SafeOrigin;
        std::optional<Pose> Safe, Frozen;
    };
    struct Hold
    {
        uint32_t Reviver{}, Target{}, TargetRevision{}, HitSerial{};
        uint64_t Id{}, Began{}, Heartbeat{};
        Pose Start;
    };

    bool Join(uint32_t aActor)
    {
        if ((Resetting || Members.size() >= 2) && !Members.contains(aActor))
            return false;
        auto& member = Members[aActor];
        member.Actor = aActor;
        member.Connected = true;
        if (Active && !Cohort.contains(aActor))
        {
            Cohort.insert(aActor); // a living party member, even in another cell, prevents a group wipe
            member.Frozen = member.Safe;
        }
        return true;
    }
    void Disconnect(uint32_t aActor)
    {
        if (auto it = Members.find(aActor); it != Members.end())
            it->second.Connected = false;
        CancelHoldsFor(aActor);
        if (Active && Cohort.contains(aActor))
        {
            const bool canResume = !Resetting;
            Interrupt("Encounter participant disconnected; no automatic reset over an unknown client state");
            DisconnectPause = canResume;
        }
    }
    void Rebind(uint32_t aOldActor, uint32_t aNewActor)
    {
        if (aOldActor == aNewActor || !Members.contains(aOldActor) || Members.contains(aNewActor))
            return;
        CancelHoldsFor(aOldActor);
        auto member = Members.at(aOldActor);
        Members.erase(aOldActor);
        EndedHolds.erase(aOldActor);
        member.Actor = aNewActor;
        member.Connected = true;
        member.Seen = 0;
        Members.emplace(aNewActor, member);
        if (Cohort.erase(aOldActor))
            Cohort.insert(aNewActor);
    }
    bool Observe(uint32_t aActor, const Presence& aPresence, uint64_t aNow)
    {
        const auto it = Members.find(aActor);
        if (it == Members.end() || (!aPresence.Location.Valid() && !(aPresence.Flags & Loading)))
            return false;
        auto& member = it->second;
        member.Connected = true;
        member.Seen = aNow;
        if (aPresence.Location.Valid())
            member.Location = aPresence.Location;
        member.Flags = aPresence.Flags;
        member.HitSerial = aPresence.HitSerial;
        const bool safe = (member.Flags & Grounded) &&
            !(member.Flags & (InCombat | Loading | UnsafeEncounter | Bleedout)) && !member.Downed;
        if (!safe)
            member.SafeSince = 0;
        else if (!member.SafeSince || DistanceSquared(member.SafeOrigin, member.Location) > 32.0 * 32.0)
        {
            member.SafeSince = aNow;
            member.SafeOrigin = member.Location;
        }
        if ((member.Flags & (InCombat | Bleedout)) && !(member.Flags & Loading) && !Active)
            Begin(aActor);
        if ((member.Flags & Bleedout) && !(member.Flags & Loading) && !member.Downed && aPresence.DownedRevision == member.Revision)
        {
            member.Downed = true;
            ++member.Revision;
            CancelHoldsFor(aActor);
            EmitDowned(member);
        }
        if (DisconnectPause)
        {
            bool ready = true;
            for (auto id : Cohort)
                ready &= Fresh(Members.at(id), aNow);
            if (ready)
            {
                DisconnectPause = Blocked = false;
                EmitEncounter(EncounterPhase::Started, 0, "Participants reconnected; downed state retained");
            }
        }
        if (Active && (member.Flags & UnsafeEncounter))
            Unsafe = true;
        // Standing observed after health regen does NOT clear the server's downed latch.
        return true;
    }
    bool RegisterAnchor(uint32_t aActor, const Anchor& aAnchor, uint64_t aNow)
    {
        const auto it = Members.find(aActor);
        if (it == Members.end() || Active || !aAnchor.Location.Valid())
            return false;
        auto& member = it->second;
        if (!member.SafeSince || aNow < member.SafeSince || aNow - member.SafeSince < kSafeDwellMs ||
            aAnchor.DwellMs < kSafeDwellMs || !(member.Flags & Stable) || !Fresh(member, aNow) || member.Downed ||
            DistanceSquared(member.Location, aAnchor.Location) > 64.0 * 64.0)
            return false;
        // Ordinary exterior-cell changes do not reach here and never overwrite an anchor.
        member.Safe = aAnchor.Location;
        return true;
    }
    bool ReviveHold(uint32_t aReviver, const Revive& aRequest, uint64_t aNow)
    {
        const auto ri = Members.find(aReviver), ti = Members.find(aRequest.Target);
        if (ri == Members.end() || ti == Members.end() || ri == ti)
            return false;
        auto existing = Holds.find(aRequest.Target);
        if (!aRequest.Holding)
        {
            if (existing != Holds.end() && existing->second.Reviver == aReviver && existing->second.Id == aRequest.Hold)
                Cancel(existing);
            return true;
        }
        const auto& reviver = ri->second;
        const auto& target = ti->second;
        if (!aRequest.Hold || aRequest.Hold <= EndedHolds[aReviver] || target.Revision != aRequest.Revision || !target.Downed || reviver.Downed ||
            !Fresh(reviver, aNow) || !Fresh(target, aNow) || !(reviver.Flags & CanInteract) ||
            (reviver.Flags & Loading) || (target.Flags & Loading) || Resetting || Blocked ||
            DistanceSquared(reviver.Location, target.Location) > 192.0 * 192.0)
            return false;
        if (existing != Holds.end())
        {
            const auto& hold = existing->second;
            if (hold.Reviver != aReviver || hold.Id != aRequest.Hold)
                return false; // first valid reviver owns the reservation
            if (aNow < hold.Heartbeat || aNow - hold.Heartbeat > kHoldTimeoutMs ||
                hold.HitSerial != reviver.HitSerial || DistanceSquared(hold.Start, reviver.Location) > 64.0 * 64.0)
            {
                Cancel(existing);
                return false; // a new uninterrupted hold needs a NEW hold id
            }
            existing->second.Heartbeat = aNow;
            return true;
        }
        for (const auto& [_, hold] : Holds)
            if (hold.Reviver == aReviver)
                return false;
        Holds.emplace(target.Actor, Hold{aReviver, target.Actor, target.Revision, reviver.HitSerial,
            aRequest.Hold, aNow, aNow, reviver.Location});
        EmitDowned(target, aReviver, kReviveMs);
        return true;
    }
    void Tick(uint64_t aNow)
    {
        for (auto it = Holds.begin(); it != Holds.end();)
        {
            const auto hold = it->second;
            auto& reviver = Members.at(hold.Reviver);
            auto& target = Members.at(hold.Target);
            if (!Fresh(reviver, aNow) || !Fresh(target, aNow) || aNow < hold.Heartbeat ||
                aNow - hold.Heartbeat > kHoldTimeoutMs || reviver.Downed || !target.Downed ||
                target.Revision != hold.TargetRevision || reviver.HitSerial != hold.HitSerial ||
                !(reviver.Flags & CanInteract) || (reviver.Flags & Loading) || (target.Flags & Loading) ||
                DistanceSquared(reviver.Location, target.Location) > 192.0 * 192.0 ||
                DistanceSquared(hold.Start, reviver.Location) > 64.0 * 64.0)
            {
                auto old = it++;
                Cancel(old);
                continue;
            }
            if (aNow >= hold.Began && aNow - hold.Began >= kReviveMs)
            {
                EndedHolds[hold.Reviver] = std::max(EndedHolds[hold.Reviver], hold.Id);
                target.Downed = false;
                target.Flags &= ~Bleedout;
                ++target.Revision;
                EmitDowned(target);
                it = Holds.erase(it);
                WipeDeadline = 0;
            }
            else
                ++it;
        }
        if (!Active || Resetting || Blocked || Cohort.empty())
            return;
        bool allDown = true, allQuiet = true;
        for (auto id : Cohort)
        {
            const auto& member = Members.at(id);
            if (!Fresh(member, aNow) || (member.Flags & Loading))
            {
                WipeDeadline = 0;
                return; // absence is not evidence that the whole group died
            }
            allDown &= member.Downed;
            allQuiet &= !member.Downed && !(member.Flags & InCombat);
        }
        if (allQuiet)
        {
            if (!QuietSince)
                QuietSince = aNow;
            if (aNow - QuietSince >= kSafeDwellMs)
            {
                Active = false;
                Cohort.clear();
                EmitEncounter(EncounterPhase::Ended, 0, "");
            }
        }
        else
            QuietSince = 0;
        if (!allDown)
        {
            WipeDeadline = 0;
            return;
        }
        if (!WipeDeadline)
        {
            WipeDeadline = aNow + kWipeMs;
            EmitEncounter(EncounterPhase::WipeCountdown, kWipeMs, "");
        }
        if (aNow < WipeDeadline)
            return;
        ResetPlan plan;
        plan.Encounter = Id;
        plan.Phase = ResetPhase::Prepare;
        for (auto id : Cohort)
        {
            const auto& member = Members.at(id);
            if (!member.Frozen || Unsafe || (member.Flags & UnsafeEncounter))
            {
                Blocked = true;
                EmitEncounter(EncounterPhase::Unavailable, 0, "No validated pre-encounter anchor or arena adapter");
                return;
            }
            ResetActor actor;
            actor.Actor = id;
            actor.IsPlayer = true;
            actor.Destination = *member.Frozen;
            plan.Actors.push_back(actor);
        }
        // Never put two players at the exact same point, nor invent an unvalidated offset.
        for (size_t i = 0; i < plan.Actors.size(); ++i)
            for (size_t j = 0; j < i; ++j)
                if (DistanceSquared(plan.Actors[i].Destination, plan.Actors[j].Destination) < 48.0 * 48.0)
                {
                    Blocked = true;
                    EmitEncounter(EncounterPhase::Unavailable, 0, "Safe anchors overlap; choose separate standing positions before combat");
                    return;
                }
        Resetting = true;
        ResetStage = ResetPhase::Prepare;
        ResetDeadline = aNow + 15000;
        ResetAcks.clear();
        Events.emplace_back(std::move(plan));
    }
    bool ResetAck(uint32_t aActor, const ResetReceipt& aReceipt, uint64_t aNow)
    {
        if (!Resetting || aNow >= ResetDeadline || aReceipt.Encounter != Id || aReceipt.Phase != ResetStage || !Cohort.contains(aActor) || !Members.at(aActor).Connected)
            return false;
        if (!aReceipt.Success)
        {
            Interrupt("Encounter reset failed; the group remains paused for coordinated recovery");
            return false;
        }
        ResetAcks.insert(aActor);
        if (ResetAcks.size() != Cohort.size())
            return true;
        ResetAcks.clear();
        ResetPlan plan;
        plan.Encounter = Id;
        if (ResetStage == ResetPhase::Prepare)
        {
            ResetStage = ResetPhase::Commit;
            ResetDeadline = aNow + 15000;
            plan.Phase = ResetPhase::Commit;
            for (auto id : Cohort)
            {
                ResetActor actor;
                actor.Actor = id;
                actor.IsPlayer = true;
                actor.Destination = *Members.at(id).Frozen;
                plan.Actors.push_back(actor);
            }
        }
        else
        {
            plan.Phase = ResetPhase::Complete;
            for (auto id : Cohort)
            {
                auto& member = Members.at(id);
                member.Downed = false;
                member.Flags &= ~(Bleedout | InCombat);
                ++member.Revision;
                EmitDowned(member);
            }
            Resetting = false;
            Active = false;
            WipeDeadline = 0;
            Holds.clear();
            Cohort.clear();
        }
        Events.emplace_back(std::move(plan));
        return true;
    }
    void CheckTimeout(uint64_t aNow)
    {
        if (Resetting && aNow >= ResetDeadline)
            Interrupt("Encounter reset acknowledgement timed out; no automatic retry over partial engine state");
    }
    std::vector<Body> Drain()
    {
        auto out = std::move(Events);
        Events.clear();
        return out;
    }
    void Snapshot()
    {
        for (const auto& [_, member] : Members)
            EmitDowned(member);
        if (Blocked || Resetting)
            EmitEncounter(EncounterPhase::Interrupted, 0, "Coordinated recovery is paused; do not resume an unknown reset phase");
        else if (Active)
            EmitEncounter(EncounterPhase::Started, 0, "");
    }

    std::map<uint32_t, Member> Members;
    std::set<uint32_t> Cohort;
    uint64_t Id{};
    bool Active{}, Resetting{}, Blocked{}, Unsafe{};
    std::map<uint32_t, Hold> Holds;

private:
    static bool Fresh(const Member& aMember, uint64_t aNow)
    {
        return aMember.Connected && aMember.Seen != 0 && aNow >= aMember.Seen && aNow - aMember.Seen <= kHeartbeatTimeoutMs;
    }
    void Begin(uint32_t aInitiator)
    {
        ++Id;
        Active = true;
        Blocked = false;
        Unsafe = false;
        WipeDeadline = 0;
        QuietSince = 0;
        Cohort.clear();
        for (auto& [id, member] : Members)
            if (member.Connected)
            {
                Cohort.insert(id);
                member.Frozen = member.Safe;
            }
        EmitEncounter(EncounterPhase::Started, 0, "");
    }
    using HoldIterator = std::map<uint32_t, Hold>::iterator;
    void Cancel(HoldIterator aIt)
    {
        const auto target = aIt->second.Target;
        EndedHolds[aIt->second.Reviver] = std::max(EndedHolds[aIt->second.Reviver], aIt->second.Id);
        Holds.erase(aIt);
        if (Members.contains(target))
            EmitDowned(Members.at(target));
    }
    void CancelHoldsFor(uint32_t aActor)
    {
        for (auto it = Holds.begin(); it != Holds.end();)
        {
            if (it->second.Target == aActor || it->second.Reviver == aActor)
            {
                auto old = it++;
                Cancel(old);
            }
            else
                ++it;
        }
    }
    void Interrupt(const char* aReason)
    {
        Blocked = true;
        Resetting = false;
        WipeDeadline = 0;
        while (!Holds.empty())
            Cancel(Holds.begin());
        ResetAcks.clear();
        EmitEncounter(EncounterPhase::Interrupted, 0, aReason);
    }
    void EmitDowned(const Member& aMember, uint32_t aReviver = 0, uint32_t aRemaining = 0)
    {
        Events.emplace_back(DownedState{aMember.Actor, aMember.Revision, aReviver, aRemaining, aMember.Downed});
    }
    void EmitEncounter(EncounterPhase aPhase, uint32_t aRemaining, const char* aReason)
    {
        Events.emplace_back(EncounterState{Id, aPhase, aRemaining, aReason});
    }
    uint64_t WipeDeadline{}, ResetDeadline{}, QuietSince{};
    bool DisconnectPause{};
    std::map<uint32_t, uint64_t> EndedHolds;
    ResetPhase ResetStage{};
    std::set<uint32_t> ResetAcks;
    std::vector<Body> Events;
};
} // namespace Coop
