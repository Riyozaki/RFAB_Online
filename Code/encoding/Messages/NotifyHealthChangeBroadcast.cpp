#include <Messages/NotifyHealthChangeBroadcast.h>

void NotifyHealthChangeBroadcast::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    Serialization::WriteVarInt(aWriter, Sequence);
    Serialization::WriteFloat(aWriter, DeltaHealth);
    Serialization::WriteFloat(aWriter, Health);
    Serialization::WriteVarInt(aWriter, Revision);
    Serialization::WriteVarInt(aWriter, SourcePlayerId);
}

void NotifyHealthChangeBroadcast::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Sequence = Serialization::ReadVarInt(aReader);
    DeltaHealth = Serialization::ReadFloat(aReader);
    Health = Serialization::ReadFloat(aReader);
    Revision = Serialization::ReadVarInt(aReader);
    SourcePlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
