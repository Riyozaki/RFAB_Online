#pragma once

#include "Message.h"

struct NotifyActorValueChanges final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyActorValueChanges;

    NotifyActorValueChanges()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyActorValueChanges& acRhs) const noexcept { return Id == acRhs.Id && Values == acRhs.Values && HealthRevision == acRhs.HealthRevision && GetOpcode() == acRhs.GetOpcode(); }

    uint32_t Id{};
    TiltedPhoques::Map<uint32_t, float> Values;

    // The version this value belongs to; clients echo it back with their own readings.
    uint32_t HealthRevision{};
};
