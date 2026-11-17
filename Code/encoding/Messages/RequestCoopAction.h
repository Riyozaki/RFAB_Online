#pragma once
#include "Message.h"
#include <Structs/CoopProtocol.h>

struct RequestCoopAction final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestCoopAction;
    RequestCoopAction() : ClientMessage(Opcode) {}
    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;
    Coop::Packet Data;
};
