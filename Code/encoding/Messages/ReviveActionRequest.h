#pragma once

#include "Message.h"

struct ReviveActionRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kReviveActionRequest;

    enum class ActionType : uint8_t
    {
        StartChannel = 0,
        CancelChannel = 1,
        HealingSpellRevive = 2,
    };

    ReviveActionRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const ReviveActionRequest& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               Action == acRhs.Action &&
               TargetPlayerId == acRhs.TargetPlayerId;
    }

    ActionType Action{ActionType::StartChannel};
    uint32_t TargetPlayerId{};
};
