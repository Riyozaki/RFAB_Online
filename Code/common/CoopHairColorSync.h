#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace CoopHairColorSync
{
// Skyrim SE 1.5.97 Actor::UpdateHairColor() converts 8-bit BGSColorForm RGB [0..255]
// to BSLightingShaderMaterialHairTint::tintColor floats via channel / 128.0f
// (so 128 -> 1.0x neutral diffuse multiplier, 255 -> ~1.992x bright custom RaceMenu tint).
inline constexpr float kSkyrimHairTintDivisor = 128.0f;
inline constexpr float kDefaultShaderDriftEpsilon = 0.002f;
inline constexpr uint64_t kSteadyHairCheckIntervalMs = 250ULL;
inline constexpr uint64_t kAppearancePollIntervalMs = 1000ULL;
inline constexpr uint64_t kAppearanceResendCooldownMs = 750ULL;

[[nodiscard]] constexpr uint32_t PackHairColorRgb(const uint8_t aRed, const uint8_t aGreen, const uint8_t aBlue) noexcept
{
    return (static_cast<uint32_t>(aRed) << 16u) |
           (static_cast<uint32_t>(aGreen) << 8u) |
           static_cast<uint32_t>(aBlue);
}

constexpr void UnpackHairColorRgb(const uint32_t aPackedRgb, uint8_t& aOutRed, uint8_t& aOutGreen, uint8_t& aOutBlue) noexcept
{
    aOutRed = static_cast<uint8_t>((aPackedRgb >> 16u) & 0xFFu);
    aOutGreen = static_cast<uint8_t>((aPackedRgb >> 8u) & 0xFFu);
    aOutBlue = static_cast<uint8_t>(aPackedRgb & 0xFFu);
}

[[nodiscard]] constexpr float ChannelToHairShaderTint(const uint8_t aChannel) noexcept
{
    return static_cast<float>(aChannel) / kSkyrimHairTintDivisor;
}

[[nodiscard]] inline uint8_t HairShaderTintToChannel(const float aShaderChannel) noexcept
{
    if (!std::isfinite(aShaderChannel) || aShaderChannel <= 0.0f)
        return 0u;

    const float scaled = aShaderChannel * kSkyrimHairTintDivisor;
    if (scaled >= 255.0f)
        return 255u;

    return static_cast<uint8_t>(std::lround(scaled));
}

[[nodiscard]] inline bool IsHairTintDrifted(
    const float aCurRed,
    const float aCurGreen,
    const float aCurBlue,
    const float aTargetRed,
    const float aTargetGreen,
    const float aTargetBlue,
    const float aEpsilon = kDefaultShaderDriftEpsilon) noexcept
{
    if (!std::isfinite(aCurRed) || !std::isfinite(aCurGreen) || !std::isfinite(aCurBlue))
        return true;

    return std::fabs(aCurRed - aTargetRed) > aEpsilon ||
           std::fabs(aCurGreen - aTargetGreen) > aEpsilon ||
           std::fabs(aCurBlue - aTargetBlue) > aEpsilon;
}

[[nodiscard]] constexpr bool ShouldIsolateHairTintPass(
    const void* const apLastFaceNode,
    const void* const apCurFaceNode,
    const void* const apLastRootNode,
    const void* const apCurRootNode,
    const uint32_t aLastGeomCount,
    const uint32_t aCurGeomCount,
    const bool aHairGenerated) noexcept
{
    if (!aHairGenerated)
        return true;
    if (apLastFaceNode != apCurFaceNode || apLastRootNode != apCurRootNode)
        return true;
    if (aLastGeomCount != aCurGeomCount)
        return true;
    return false;
}

class Fnv1a64Hasher
{
public:
    static constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    static constexpr uint64_t kPrime = 1099511628211ULL;

    constexpr void MixByte(const uint8_t aByte) noexcept
    {
        m_hash ^= static_cast<uint64_t>(aByte);
        m_hash *= kPrime;
    }

    constexpr void MixU32(const uint32_t aValue) noexcept
    {
        MixByte(static_cast<uint8_t>(aValue & 0xFFu));
        MixByte(static_cast<uint8_t>((aValue >> 8u) & 0xFFu));
        MixByte(static_cast<uint8_t>((aValue >> 16u) & 0xFFu));
        MixByte(static_cast<uint8_t>((aValue >> 24u) & 0xFFu));
    }

    void MixFloat(const float aValue) noexcept
    {
        uint32_t bits = 0;
        if (std::isfinite(aValue))
            std::memcpy(&bits, &aValue, sizeof(bits));
        MixU32(bits);
    }

    [[nodiscard]] constexpr uint64_t Finish() const noexcept
    {
        return m_hash == 0ULL ? 1ULL : m_hash;
    }

private:
    uint64_t m_hash{kOffsetBasis};
};

// Tracks local player appearance & RaceMenu custom hair color state so deferred skee64 preset loads
// and mid-session showracemenu edits trigger a single clean AssignCharacterRequest update once closed.
class AppearanceResyncTracker
{
public:
    void Reset() noexcept
    {
        m_baselineFingerprint = 0ULL;
        m_lastPollMs = 0ULL;
        m_lastSendMs = 0ULL;
        m_wasInRaceSexMenu = false;
    }

    void RecordSyncedFingerprint(const uint64_t aFingerprint, const uint64_t aNowMs) noexcept
    {
        m_baselineFingerprint = aFingerprint;
        m_lastSendMs = aNowMs;
        m_lastPollMs = aNowMs;
    }

    [[nodiscard]] bool ShouldPollNow(const uint64_t aNowMs, const bool aInRaceSexMenu, const bool aInLoadingMenu) noexcept
    {
        if (aInLoadingMenu)
            return false;

        if (aInRaceSexMenu)
        {
            m_wasInRaceSexMenu = true;
            return false;
        }

        if (m_wasInRaceSexMenu)
        {
            m_wasInRaceSexMenu = false;
            m_lastPollMs = aNowMs;
            return true;
        }

        if (m_lastPollMs != 0ULL && aNowMs < m_lastPollMs + kAppearancePollIntervalMs)
            return false;

        m_lastPollMs = aNowMs;
        return true;
    }

    [[nodiscard]] bool EvaluateChangedFingerprint(const uint64_t aCurrentFingerprint, const uint64_t aNowMs) noexcept
    {
        if (aCurrentFingerprint == 0ULL)
            return false;

        if (m_baselineFingerprint == 0ULL)
        {
            m_baselineFingerprint = aCurrentFingerprint;
            return false;
        }

        if (aCurrentFingerprint == m_baselineFingerprint)
            return false;

        if (m_lastSendMs != 0ULL && aNowMs < m_lastSendMs + kAppearanceResendCooldownMs)
            return false;

        m_baselineFingerprint = aCurrentFingerprint;
        m_lastSendMs = aNowMs;
        return true;
    }

    [[nodiscard]] uint64_t GetBaselineFingerprint() const noexcept { return m_baselineFingerprint; }

private:
    uint64_t m_baselineFingerprint{0ULL};
    uint64_t m_lastPollMs{0ULL};
    uint64_t m_lastSendMs{0ULL};
    bool m_wasInRaceSexMenu{false};
};
} // namespace CoopHairColorSync
