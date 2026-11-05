#include <Messages/NotifySpawnData.h>

void NotifySpawnData::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    NewActorData.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, HealthRevision);
}

void NotifySpawnData::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    NewActorData.Deserialize(aReader);
    HealthRevision = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
