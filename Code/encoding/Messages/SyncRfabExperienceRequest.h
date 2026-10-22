#pragma once

#include "Message.h"

struct SyncRfabExperienceRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kSyncRfabExperienceRequest;

    SyncRfabExperienceRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const SyncRfabExperienceRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Experience == acRhs.Experience; }

    uint32_t Experience{};
};
