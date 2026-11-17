#pragma once
#include "Message.h"
#include <Structs/CoopProtocol.h>

struct NotifyCoopAction final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyCoopAction;
    NotifyCoopAction() : ServerMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    Coop::Packet Data;
};
