#pragma once

#include "Message.h"

struct SyncPlayerDownedState final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kSyncPlayerDownedState;

    SyncPlayerDownedState()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const SyncPlayerDownedState& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               IsDowned == acRhs.IsDowned &&
               PositionX == acRhs.PositionX &&
               PositionY == acRhs.PositionY &&
               PositionZ == acRhs.PositionZ &&
               NearbyHostileCount == acRhs.NearbyHostileCount;
    }

    bool IsDowned{false};
    float PositionX{};
    float PositionY{};
    float PositionZ{};
    uint32_t NearbyHostileCount{};
};
