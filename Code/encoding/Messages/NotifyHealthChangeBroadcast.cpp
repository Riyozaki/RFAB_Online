#include <Messages/NotifyHealthChangeBroadcast.h>

void NotifyHealthChangeBroadcast::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    Serialization::WriteFloat(aWriter, DeltaHealth);
    Serialization::WriteVarInt(aWriter, SourceServerId);
    Serialization::WriteFloat(aWriter, StaggerMagnitude);
    Serialization::WriteFloat(aWriter, StaggerDirection);
}

void NotifyHealthChangeBroadcast::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    DeltaHealth = Serialization::ReadFloat(aReader);
    SourceServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    StaggerMagnitude = Serialization::ReadFloat(aReader);
    StaggerDirection = Serialization::ReadFloat(aReader);
}
