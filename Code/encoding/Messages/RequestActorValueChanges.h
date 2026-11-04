#pragma once

#include "Message.h"

struct RequestActorValueChanges final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestActorValueChanges;

    RequestActorValueChanges()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestActorValueChanges& acRhs) const noexcept { return Id == acRhs.Id && Values == acRhs.Values && HealthRevision == acRhs.HealthRevision && GetOpcode() == acRhs.GetOpcode(); }

    uint32_t Id{};
    TiltedPhoques::Map<uint32_t, float> Values;

    // Which version of this actor's health the sender had applied when it read these
    // values. The server needs it to tell a fresh reading from an echo of the past.
    uint32_t HealthRevision{};
};
