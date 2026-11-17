#pragma once
#include <Coop/Encounter.h>
#include <optional>

namespace Coop
{
// Client-side half of reset A. A transport retry is not a second native operation. Completion
// requires an explicit post-native readiness receipt; entering Commit alone proves nothing.
class ResetParticipant
{
public:
    enum class Decision { Reject, Prepare, Commit, Duplicate, Complete };
    Decision Accept(const ResetPlan& aPlan, uint32_t aLocalActor)
    {
        if (!aPlan.Encounter || aPlan.Encounter < Encounter)
            return Decision::Reject;
        if (aPlan.Phase == ResetPhase::Prepare)
        {
            if (aPlan.Encounter == Encounter)
                return Stage == ResetPhase::Prepare ? Decision::Duplicate : Decision::Reject;
            if (Pending || (Encounter && Stage != ResetPhase::Complete))
                return Decision::Reject;
            if (!PlayersValid(aPlan, aLocalActor))
                return Decision::Reject;
            Encounter = aPlan.Encounter;
            Stage = ResetPhase::Prepare;
            Prepared = aPlan;
            LastReceipt.reset();
            return Decision::Prepare;
        }
        if (aPlan.Encounter != Encounter)
            return Decision::Reject;
        if (aPlan.Phase == ResetPhase::Commit)
        {
            if (Stage == ResetPhase::Commit)
                return Decision::Duplicate;
            if (Stage != ResetPhase::Prepare || !LastReceipt || !LastReceipt->Success ||
                !PlayersValid(aPlan, aLocalActor))
                return Decision::Reject;
            size_t previousPlayers{}, nextPlayers{};
            for (const auto& actor : Prepared.Actors) previousPlayers += actor.IsPlayer;
            for (const auto& actor : aPlan.Actors) nextPlayers += actor.IsPlayer;
            if (previousPlayers != nextPlayers)
                return Decision::Reject;
            for (const auto& prior : Prepared.Actors)
                if (prior.IsPlayer)
                {
                    bool matches{};
                    for (const auto& next : aPlan.Actors)
                        if (next.IsPlayer && prior.Actor == next.Actor && next.Epoch && next.Health > 0.f &&
                            DistanceSquared(prior.Destination, next.Destination) < 0.01 &&
                            prior.Destination.Heading == next.Destination.Heading)
                            matches = true;
                    if (!matches)
                        return Decision::Reject;
                }
            Stage = ResetPhase::Commit;
            Pending = true;
            LastReceipt.reset();
            return Decision::Commit;
        }
        if (aPlan.Phase == ResetPhase::Complete && Stage == ResetPhase::Commit &&
            !Pending && LastReceipt && LastReceipt->Success)
        {
            Stage = ResetPhase::Complete;
            return Decision::Complete;
        }
        return Decision::Reject;
    }
    ResetReceipt Receipt(bool aSuccess)
    {
        Pending = false;
        LastReceipt = ResetReceipt{Encounter, Stage, aSuccess};
        return *LastReceipt;
    }
    void Clear() { *this = ResetParticipant{}; }
    uint64_t Encounter{};
    ResetPhase Stage{ResetPhase::Complete};
    bool Pending{};
    std::optional<ResetReceipt> LastReceipt;
private:
    static bool PlayersValid(const ResetPlan& aPlan, uint32_t aLocalActor)
    {
        std::set<uint32_t> seen;
        size_t players{}, local{};
        for (const auto& actor : aPlan.Actors)
        {
            if (!actor.Actor || !seen.insert(actor.Actor).second || !std::isfinite(actor.Health))
                return false;
            if (actor.IsPlayer)
            {
                ++players;
                local += actor.Actor == aLocalActor;
                if (!actor.Destination.Valid())
                    return false;
            }
        }
        return players && players <= 2 && local == 1;
    }
    ResetPlan Prepared;
};
} // namespace Coop
