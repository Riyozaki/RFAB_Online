#include <Messages/ReviveActionRequest.h>

void ReviveActionRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, static_cast<uint64_t>(Action));
    Serialization::WriteVarInt(aWriter, TargetPlayerId);
}

void ReviveActionRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Action = static_cast<ActionType>(Serialization::ReadVarInt(aReader) & 0xFF);
    TargetPlayerId = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
}
