#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

using TiltedPhoques::Buffer;

struct ServerSettings
{
    bool operator==(const ServerSettings& acRhs) const noexcept;
    bool operator!=(const ServerSettings& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    // Computes effective enemy max-HP multiplier:
    // - 0..1 players -> BaseMultiplier * 1.0
    // - N >= 2 players -> BaseMultiplier * (N + ExtraBase)  (default ExtraBase=1.0 => 2p=3x, 3p=4x, 4p=5x)
    [[nodiscard]] static float ComputeEffectiveHealthMultiplier(
        float aBaseMultiplier,
        bool aAutoScaleByPlayerCount,
        float aExtraBase,
        size_t aConnectedPlayers) noexcept
    {
        constexpr float kMinMultiplier = 0.1f;
        constexpr float kMaxMultiplier = 20.0f;

        float base = std::isfinite(aBaseMultiplier) ? std::clamp(aBaseMultiplier, kMinMultiplier, kMaxMultiplier) : 1.0f;
        if (aAutoScaleByPlayerCount && aConnectedPlayers >= 2)
        {
            const float extra = std::isfinite(aExtraBase) ? std::clamp(aExtraBase, 0.0f, 10.0f) : 1.0f;
            base *= (static_cast<float>(aConnectedPlayers) + extra);
        }

        return std::clamp(base, kMinMultiplier, kMaxMultiplier);
    }

    uint32_t Difficulty{};
    bool GreetingsEnabled{};
    bool PvpEnabled{};
    bool SyncPlayerHomes{};
    bool DeathSystemEnabled{};
    bool SyncPlayerCalendar{};
    bool AutoPartyJoin{};
    float HealthMultiplier{1.0f};

    // Stage 3 & 4 RFAB multiplayer settings
    float HealthExtraBase{1.0f};
    bool DownedSystemEnabled{true};
    float ReviveDurationSec{4.0f};
    float ReviveHealthFraction{0.30f};
    float PostReviveGraceSec{3.0f};
    float CombatLeashRadiusMeters{120.0f};
    bool CoordinatedSavesEnabled{true};
    uint32_t PostCombatAutosaveIntervalSec{180};
};
