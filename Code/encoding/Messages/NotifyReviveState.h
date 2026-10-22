#pragma once

#include "Message.h"

struct NotifyReviveState final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyReviveState;

    enum class ReviveStatus : uint8_t
    {
        ChannelStarted = 0,
        ChannelCancelled = 1,
        RevivedByAlly = 2,
        RevivedPostCombat = 3,
        PartyWipeImminent = 4,
    };

    NotifyReviveState()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyReviveState& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               Status == acRhs.Status &&
               TargetPlayerId == acRhs.TargetPlayerId &&
               ReviverPlayerId == acRhs.ReviverPlayerId &&
               DurationSec == acRhs.DurationSec &&
               HealthFraction == acRhs.HealthFraction &&
               GraceSec == acRhs.GraceSec;
    }

    ReviveStatus Status{ReviveStatus::ChannelStarted};
    uint32_t TargetPlayerId{};
    uint32_t ReviverPlayerId{};
    float DurationSec{4.0f};
    float HealthFraction{0.30f};
    float GraceSec{3.0f};
};
