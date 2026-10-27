#pragma once

#include "Message.h"

struct RequestHealthChangeBroadcast final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestHealthChangeBroadcast;

    RequestHealthChangeBroadcast()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestHealthChangeBroadcast& acRhs) const noexcept
    {
        return Id == acRhs.Id &&
               DeltaHealth == acRhs.DeltaHealth &&
               SourceServerId == acRhs.SourceServerId &&
               StaggerMagnitude == acRhs.StaggerMagnitude &&
               StaggerDirection == acRhs.StaggerDirection &&
               GetOpcode() == acRhs.GetOpcode();
    }

    uint32_t Id{};
    float DeltaHealth{};
    uint32_t SourceServerId{};
    float StaggerMagnitude{};
    float StaggerDirection{};
};
