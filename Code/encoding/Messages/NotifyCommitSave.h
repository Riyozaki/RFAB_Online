#pragma once

#include "Message.h"

struct NotifyCommitSave final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCommitSave;

    NotifyCommitSave()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyCommitSave& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               EpochId == acRhs.EpochId &&
               SlotName == acRhs.SlotName &&
               Committed == acRhs.Committed &&
               Message == acRhs.Message;
    }

    uint64_t EpochId{};
    String SlotName{};
    bool Committed{false};
    String Message{};
};
