#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace CoopAnimationState
{
// Skyrim SE 1.5.97 ActorState::actorState1 / actorState2 bit layouts
constexpr uint32_t kSitSleepShift = 14u;
constexpr uint32_t kSitSleepMask = 0xFu << kSitSleepShift;      // bits 14..17 (0 = kNormal, 1..4 = Sit, 5..8 = Sleep)
constexpr uint32_t kLifeStateShift = 21u;
constexpr uint32_t kLifeStateMask = 0xFu << kLifeStateShift;    // bits 21..24 (0 = kAlive, 1 = kDying, 2 = kDead, 7 = kEssentialDown, 8 = kBleedout)
constexpr uint32_t kLifeStateAlive = 0u;
constexpr uint32_t kLifeStateDying = 1u;
constexpr uint32_t kLifeStateDead = 2u;
constexpr uint32_t kLifeStateEssentialDown = 7u;
constexpr uint32_t kLifeStateBleedout = 8u;
constexpr uint32_t kKnockStateShift = 25u;
constexpr uint32_t kKnockStateMask = 0x7u << kKnockStateShift;  // bits 25..27
constexpr uint32_t kMeleeAttackShift = 28u;
constexpr uint32_t kMeleeAttackMask = 0xFu << kMeleeAttackShift; // bits 28..31

constexpr uint32_t kWeaponStateShift = 5u;
constexpr uint32_t kWeaponStateMask = 0x7u << kWeaponStateShift; // flags2 bits 5..7 (3 = Drawn)
constexpr uint32_t kRecoilMask = 0x3u << 10u;                    // flags2 bits 10..11
constexpr uint32_t kStaggeredMask = 0x1u << 13u;                 // flags2 bit 13

[[nodiscard]] constexpr uint32_t GetLifeState(uint32_t aFlags1) noexcept
{
    return (aFlags1 & kLifeStateMask) >> kLifeStateShift;
}

// Skyrim's CombatTargetSelector refuses to target actors in kDying (1), kEssentialDown (7), or kBleedout (8).
// Co-op players run with SetEssentialEx(true) + SetNoBleedoutRecovery(true), so lethal hits place them into
// kEssentialDown (7) or kBleedout (8) (or briefly kDying (1)). All three must be recognized and cleared on revive/rollback!
[[nodiscard]] constexpr bool IsDownedOrBleedoutLifeState(uint32_t aFlags1) noexcept
{
    const uint32_t lifeState = GetLifeState(aFlags1);
    return lifeState == kLifeStateDying ||
           lifeState == kLifeStateEssentialDown ||
           lifeState == kLifeStateBleedout;
}

[[nodiscard]] constexpr bool NormalizePostReviveActorState(uint32_t& aInOutFlags1, uint32_t& aInOutFlags2) noexcept
{
    const bool wasDownedLifeState = IsDownedOrBleedoutLifeState(aInOutFlags1);
    const bool hadKnockOrStagger = (aInOutFlags1 & kKnockStateMask) != 0u ||
                                   (aInOutFlags2 & (kRecoilMask | kStaggeredMask)) != 0u;
    if (wasDownedLifeState)
        aInOutFlags1 &= ~kLifeStateMask; // kAlive == 0

    aInOutFlags1 &= ~kKnockStateMask;
    aInOutFlags2 &= ~(kRecoilMask | kStaggeredMask);
    return wasDownedLifeState || hadKnockOrStagger;
}

[[nodiscard]] constexpr uint32_t GetSitSleepState(uint32_t aFlags1) noexcept
{
    return (aFlags1 & kSitSleepMask) >> kSitSleepShift;
}

[[nodiscard]] constexpr uint32_t ClearSitSleepState(uint32_t aFlags1) noexcept
{
    return aFlags1 & ~kSitSleepMask;
}

[[nodiscard]] constexpr uint32_t GetMeleeAttackState(uint32_t aFlags1) noexcept
{
    return (aFlags1 & kMeleeAttackMask) >> kMeleeAttackShift;
}

[[nodiscard]] constexpr uint32_t GetWeaponState(uint32_t aFlags2) noexcept
{
    return (aFlags2 & kWeaponStateMask) >> kWeaponStateShift;
}

[[nodiscard]] constexpr bool IsWeaponDrawn(uint32_t aFlags2) noexcept
{
    return GetWeaponState(aFlags2) >= 3u;
}

[[nodiscard]] inline std::string ToAsciiLower(std::string_view aInput)
{
    std::string out;
    out.reserve(aInput.size());
    for (unsigned char ch : aInput)
        out.push_back(static_cast<char>(std::tolower(ch)));
    return out;
}

// Recognizes all Skyrim & RFAB furniture/chair/bed/idle exit, combat get-up, and graph-reset events,
// including fast combat exits ("IdleChairGetUp", "IdleFurnitureExitFast", "IdleStop", "IdleForceDefaultState")
// that do not end with the literal suffix "exit".
[[nodiscard]] inline bool IsFurnitureOrIdleExitEvent(std::string_view aEventName) noexcept
{
    if (aEventName.empty())
        return false;

    const std::string lower = ToAsciiLower(aEventName);

    if (lower == "idlestop" ||
        lower == "idlestopinstant" ||
        lower == "idleforcedefaultstate" ||
        lower == "returnforcedefaultstate" ||
        lower == "returndefaultstate" ||
        lower == "getupbegin" ||
        lower == "getupend" ||
        lower == "layexit" ||
        lower == "stoolbackexit" ||
        lower == "tablebackexit" ||
        lower == "trapexitstart" ||
        lower == "vehicleexit" ||
        lower == "webidleexit" ||
        lower == "npcdragonmountexitout" ||
        lower == "npchorseexit" ||
        lower == "npchorseexitout")
    {
        return true;
    }

    if (lower.starts_with("idle"))
    {
        if (lower.ends_with("exit") ||
            lower.ends_with("exitstart") ||
            lower.ends_with("exitfast") ||
            lower.ends_with("exitslow") ||
            lower.ends_with("exitinstant") ||
            lower.ends_with("getup") ||
            lower.ends_with("getupfast") ||
            lower.ends_with("wake") ||
            lower.ends_with("stand"))
        {
            return true;
        }
    }

    return false;
}

[[nodiscard]] inline bool IsFurnitureEnterEvent(std::string_view aEventName) noexcept
{
    if (aEventName.empty() || IsFurnitureOrIdleExitEvent(aEventName))
        return false;

    const std::string lower = ToAsciiLower(aEventName);
    if (lower.find("var") != std::string::npos)
        return false;

    return lower.starts_with("idlechair") ||
           lower.starts_with("idlebed") ||
           lower.starts_with("idlefurniture") ||
           lower.starts_with("idlesit") ||
           lower.starts_with("idlesleep") ||
           lower.starts_with("stool") ||
           lower.starts_with("table") ||
           lower.starts_with("throne");
}

struct RemoteSitSleepDecision
{
    uint32_t NormalizedFlags1{0};
    bool ForceDefaultIdleGraph{false};
};

// SkyMP-style explicit SitSleep/Furniture state normalizer for remote actors:
// 1. Prevents "mid-air sitting bandits" when an actor has SitSleepState != 0 (e.g. kIsSitting)
//    without valid nearby/occupied furniture while fighting, drawing weapons, or running.
// 2. Detects missed furniture-exit packets (prevSitSleepState != 0 -> incomingSitSleepState == 0
//    without an exit animation event) and signals ForceDefaultIdleGraph so ChairRootBehavior
//    immediately releases the lower body back to standing locomotion.
[[nodiscard]] inline RemoteSitSleepDecision EvaluateRemoteSitSleepTransition(
    uint32_t aPrevFlags1,
    uint32_t aIncomingFlags1,
    uint32_t aIncomingFlags2,
    bool aHasValidNearbyFurniture,
    std::string_view aIncomingEventName,
    float aHorizontalSpeedUnitsPerSec = 0.0f) noexcept
{
    RemoteSitSleepDecision out{};
    out.NormalizedFlags1 = aIncomingFlags1;

    const uint32_t prevSitSleep = GetSitSleepState(aPrevFlags1);
    const uint32_t incomingSitSleep = GetSitSleepState(aIncomingFlags1);
    const bool inActiveCombatOrMotion =
        IsWeaponDrawn(aIncomingFlags2) ||
        GetMeleeAttackState(aIncomingFlags1) != 0u ||
        aHorizontalSpeedUnitsPerSec > 35.0f;

    if (incomingSitSleep != 0u && !aHasValidNearbyFurniture && inActiveCombatOrMotion &&
        !IsFurnitureEnterEvent(aIncomingEventName))
    {
        out.NormalizedFlags1 = ClearSitSleepState(aIncomingFlags1);
        out.ForceDefaultIdleGraph = true;
        return out;
    }

    if (prevSitSleep != 0u && incomingSitSleep == 0u && !IsFurnitureOrIdleExitEvent(aIncomingEventName))
    {
        out.ForceDefaultIdleGraph = true;
    }

    return out;
}

// Normalizes ActorState bitfields when claiming local ownership of a live NPC:
// clears stuck attack/knock/recoil/stagger bits, resolves intermediate weapon draw/sheathe states,
// and clears stale SitSleepState if the NPC has no valid nearby furniture or already drew weapons.
// Returns true if the animation graph should receive IdleForceDefaultState to exit a stuck chair/bed state.
[[nodiscard]] inline bool NormalizeLocalOwnershipActorState(
    uint32_t& aInOutFlags1,
    uint32_t& aInOutFlags2,
    bool aHasValidNearbyFurniture) noexcept
{
    aInOutFlags1 &= ~(kMeleeAttackMask | kKnockStateMask);
    aInOutFlags2 &= ~(kRecoilMask | kStaggeredMask);

    const uint32_t weaponState = GetWeaponState(aInOutFlags2);
    if (weaponState == 1u || weaponState == 2u)
        aInOutFlags2 = (aInOutFlags2 & ~kWeaponStateMask) | (3u << kWeaponStateShift);
    else if (weaponState == 4u || weaponState == 5u)
        aInOutFlags2 &= ~kWeaponStateMask;

    const uint32_t sitSleep = GetSitSleepState(aInOutFlags1);
    if (sitSleep != 0u && (!aHasValidNearbyFurniture || IsWeaponDrawn(aInOutFlags2)))
    {
        aInOutFlags1 = ClearSitSleepState(aInOutFlags1);
        return true;
    }

    return false;
}

struct ReplayEntryView
{
    std::string_view EventName{};
    uint32_t State1{0};
    uint32_t State2{0};
};

// Scans a replay action chain from newest to oldest: if a later action shows the actor standing in
// normal state (SitSleepState == 0) with weapons drawn or attacking, while an earlier cached action
// had SitSleepState != 0 or entered furniture without a recognized exit in between, returns the index
// of that first standing action so obsolete furniture entries before it are pruned and the graph is reset.
[[nodiscard]] inline int32_t FindImplicitFurnitureResetIndex(const std::vector<ReplayEntryView>& aEntries) noexcept
{
    if (aEntries.size() < 2)
        return -1;

    int32_t earliestStandingAfterSit = -1;
    bool sawEarlierSit = false;

    for (size_t i = 0; i < aEntries.size(); ++i)
    {
        const auto& e = aEntries[i];
        const uint32_t sitSleep = GetSitSleepState(e.State1);
        if (sitSleep != 0u || IsFurnitureEnterEvent(e.EventName))
        {
            sawEarlierSit = true;
            earliestStandingAfterSit = -1;
        }
        else if (sawEarlierSit && sitSleep == 0u &&
                 (IsWeaponDrawn(e.State2) || GetMeleeAttackState(e.State1) != 0u))
        {
            if (earliestStandingAfterSit == -1)
                earliestStandingAfterSit = static_cast<int32_t>(i);
        }
    }

    return earliestStandingAfterSit;
}
} // namespace CoopAnimationState
