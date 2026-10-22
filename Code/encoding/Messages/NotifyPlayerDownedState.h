#pragma once

#include "Message.h"

struct NotifyPlayerDownedState final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerDownedState;

    NotifyPlayerDownedState()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPlayerDownedState& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               PlayerId == acRhs.PlayerId &&
               CharacterServerId == acRhs.CharacterServerId &&
               IsDowned == acRhs.IsDowned &&
               PositionX == acRhs.PositionX &&
               PositionY == acRhs.PositionY &&
               PositionZ == acRhs.PositionZ;
    }

    uint32_t PlayerId{};
    uint32_t CharacterServerId{};
    bool IsDowned{false};
    float PositionX{};
    float PositionY{};
    float PositionZ{};
};
