#pragma once

#include "Message.h"

struct NotifyHealthChangeBroadcast final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyHealthChangeBroadcast;

    NotifyHealthChangeBroadcast()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyHealthChangeBroadcast& acRhs) const noexcept { return Id == acRhs.Id && OwnershipEpoch == acRhs.OwnershipEpoch && Sequence == acRhs.Sequence && DeltaHealth == acRhs.DeltaHealth && Health == acRhs.Health && Revision == acRhs.Revision && SourcePlayerId == acRhs.SourcePlayerId && GetOpcode() == acRhs.GetOpcode(); }

    uint32_t Id{};
    uint32_t OwnershipEpoch{};
    uint64_t Sequence{};
    float DeltaHealth{};
    float Health{};
    uint64_t Revision{};
    uint32_t SourcePlayerId{};
};
