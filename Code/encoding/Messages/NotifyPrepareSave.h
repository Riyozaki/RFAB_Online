#pragma once

#include "Message.h"

struct NotifyPrepareSave final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPrepareSave;

    NotifyPrepareSave()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPrepareSave& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               EpochId == acRhs.EpochId &&
               SlotName == acRhs.SlotName &&
               IsAutoCheckpoint == acRhs.IsAutoCheckpoint;
    }

    uint64_t EpochId{};
    String SlotName{};
    bool IsAutoCheckpoint{false};
};
