#pragma once

/**
 * Server-controlled coop feature switches (architecture step 0).
 *
 * Every optional RFAB Online subsystem can be switched off by the server admin, so a playtest bug
 * can be bisected by turning features off one at a time instead of shipping a new build. This
 * header is the single read API for those switches:
 *
 *  - The server builds ONE effective mask from its `Gameplay:*` cvars (`GameServer.cpp`,
 *    `GetSettings()`) and ships it in `ServerSettings::DisabledFeatureMask`.
 *  - The client publishes it once per settings change into a lock-free snapshot
 *    (`World::SetServerSettings` -> `World::IsFeatureEnabled`). Hooks running on Havok / AI /
 *    Papyrus worker threads read that atomic snapshot only - never `ServerSettings` itself.
 *  - The server applies the same mask to the relays it owns (defence in depth: a client running an
 *    older build or ignoring the flag still cannot push a disabled feature onto its peers).
 *
 * Wire semantics are "disabled" bits on purpose: 0 means "everything on", so a bit a peer does not
 * know about yet degrades to the current default behaviour instead of silently switching something
 * off. Bits are append-only - never renumber or reuse a retired bit.
 *
 * `kDownedSystem` and `kCoordinatedSaves` predate the mask and still travel as their own bools in
 * `ServerSettings` (many call sites and tests read them); `ComposeDisabledMask` folds them in so
 * readers have exactly one place to ask.
 */

#include <array>
#include <cstdint>
#include <string_view>

namespace Session
{

enum class Feature : uint32_t
{
    kDownedSystem = 1u << 0,       // 0-HP knee bleedout, E-revive, leash, wipe reload (legacy bool)
    kCoordinatedSaves = 1u << 1,   // host-coordinated saves/loads + post-combat checkpoints (legacy bool)
    kTacticalHud = 1u << 2,        // peer HUD roster, downed beacons, 3D tactical pings
    kHairColorSync = 1u << 3,      // per-player isolated RGB hair colour for remote players
    kSharedRfabXp = 1u << 4,       // RFAB XP wallet sharing inside the party
    kSharedSkillXp = 1u << 5,      // vanilla combat skill XP sharing inside the party
    kRfabCombatRules = 1u << 6,    // ally protection, smart standing-target pick, boss/arena sync
    kSelfHealingResync = 1u << 7,  // client gap detection + server resend (architecture step 1)
};

inline constexpr uint32_t kAllKnownFeatureBits = 0xFFu;

struct FeatureInfo
{
    Feature Bit;
    std::string_view Name;  // stable, used in logs and in the server console listing
};

inline constexpr std::array<FeatureInfo, 8> kFeatureTable{{
    {Feature::kDownedSystem, "DownedSystem"},
    {Feature::kCoordinatedSaves, "CoordinatedSaves"},
    {Feature::kTacticalHud, "TacticalHud"},
    {Feature::kHairColorSync, "HairColorSync"},
    {Feature::kSharedRfabXp, "SharedRfabXp"},
    {Feature::kSharedSkillXp, "SharedSkillXp"},
    {Feature::kRfabCombatRules, "RfabCombatRules"},
    {Feature::kSelfHealingResync, "SelfHealingResync"},
}};

[[nodiscard]] constexpr uint32_t Bit(Feature aFeature) noexcept
{
    return static_cast<uint32_t>(aFeature);
}

[[nodiscard]] constexpr bool IsEnabledIn(uint32_t aDisabledMask, Feature aFeature) noexcept
{
    return (aDisabledMask & Bit(aFeature)) == 0u;
}

[[nodiscard]] constexpr uint32_t WithFeature(uint32_t aDisabledMask, Feature aFeature, bool aEnabled) noexcept
{
    return aEnabled ? (aDisabledMask & ~Bit(aFeature)) : (aDisabledMask | Bit(aFeature));
}

/**
 * Folds the two legacy bools into the wire mask. Unknown bits in `aWireDisabledMask` are kept
 * as-is (a newer server may know more features than this build; they are simply never queried).
 */
[[nodiscard]] constexpr uint32_t ComposeDisabledMask(
    uint32_t aWireDisabledMask,
    bool aDownedSystemEnabled,
    bool aCoordinatedSavesEnabled) noexcept
{
    uint32_t mask = aWireDisabledMask;
    mask = WithFeature(mask, Feature::kDownedSystem, aDownedSystemEnabled);
    mask = WithFeature(mask, Feature::kCoordinatedSaves, aCoordinatedSavesEnabled);
    return mask;
}

[[nodiscard]] constexpr std::string_view NameOf(Feature aFeature) noexcept
{
    for (const auto& info : kFeatureTable)
    {
        if (info.Bit == aFeature)
            return info.Name;
    }
    return "Unknown";
}

// Compile-time guards: the table and the "known bits" constant must describe the same set, and
// every entry must be a distinct single bit.
namespace detail
{
[[nodiscard]] constexpr bool FeatureTableIsConsistent() noexcept
{
    uint32_t seen = 0;
    for (const auto& info : kFeatureTable)
    {
        const uint32_t bit = Bit(info.Bit);
        if (bit == 0u || (bit & (bit - 1u)) != 0u || (seen & bit) != 0u || info.Name.empty())
            return false;
        seen |= bit;
    }
    return seen == kAllKnownFeatureBits;
}
} // namespace detail

static_assert(detail::FeatureTableIsConsistent(), "Session::kFeatureTable must list every feature bit exactly once");

} // namespace Session
