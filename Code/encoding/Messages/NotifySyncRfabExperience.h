#pragma once

#include "Message.h"

struct NotifySyncRfabExperience final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifySyncRfabExperience;

    NotifySyncRfabExperience()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifySyncRfabExperience& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Experience == acRhs.Experience; }

    uint32_t Experience{};
};
