#pragma once

#include "Message.h"

struct CombatEncounterStateReport final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kCombatEncounterStateReport;

    CombatEncounterStateReport()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const CombatEncounterStateReport& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               InCombat == acRhs.InCombat &&
               NearbyHostileCount == acRhs.NearbyHostileCount &&
               NearestHostileDistanceMeters == acRhs.NearestHostileDistanceMeters &&
               BossDefeatedInEncounter == acRhs.BossDefeatedInEncounter &&
               PositionX == acRhs.PositionX &&
               PositionY == acRhs.PositionY &&
               PositionZ == acRhs.PositionZ;
    }

    bool InCombat{false};
    uint32_t NearbyHostileCount{};
    float NearestHostileDistanceMeters{9999.0f};
    bool BossDefeatedInEncounter{false};
    float PositionX{};
    float PositionY{};
    float PositionZ{};
};
