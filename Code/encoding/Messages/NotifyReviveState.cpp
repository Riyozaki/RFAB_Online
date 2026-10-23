#include <Messages/NotifyReviveState.h>

void NotifyReviveState::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, static_cast<uint64_t>(Status));
    Serialization::WriteVarInt(aWriter, TargetPlayerId);
    Serialization::WriteVarInt(aWriter, ReviverPlayerId);
    Serialization::WriteFloat(aWriter, DurationSec);
    Serialization::WriteFloat(aWriter, HealthFraction);
    Serialization::WriteFloat(aWriter, GraceSec);
}

void NotifyReviveState::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Status = static_cast<ReviveStatus>(Serialization::ReadVarInt(aReader) & 0xFF);
    TargetPlayerId = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    ReviverPlayerId = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    DurationSec = Serialization::ReadFloat(aReader);
    HealthFraction = Serialization::ReadFloat(aReader);
    GraceSec = Serialization::ReadFloat(aReader);
}
