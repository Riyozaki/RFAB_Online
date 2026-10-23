#pragma once

#include "Message.h"

struct CoordinatedSaveAck final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kCoordinatedSaveAck;

    CoordinatedSaveAck()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const CoordinatedSaveAck& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               EpochId == acRhs.EpochId &&
               SlotName == acRhs.SlotName &&
               Success == acRhs.Success &&
               Reason == acRhs.Reason;
    }

    uint64_t EpochId{};
    String SlotName{};
    bool Success{false};
    String Reason{};
};
