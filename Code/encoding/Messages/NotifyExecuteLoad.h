#pragma once

#include "Message.h"

struct NotifyExecuteLoad final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyExecuteLoad;

    NotifyExecuteLoad()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyExecuteLoad& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               EpochId == acRhs.EpochId &&
               SlotName == acRhs.SlotName &&
               IsPartyWipe == acRhs.IsPartyWipe &&
               AllowPreJoinFallback == acRhs.AllowPreJoinFallback &&
               Message == acRhs.Message;
    }

    uint64_t EpochId{};
    String SlotName{};
    bool IsPartyWipe{false};
    bool AllowPreJoinFallback{true};
    String Message{};
};
