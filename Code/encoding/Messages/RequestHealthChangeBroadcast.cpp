#include <Messages/RequestHealthChangeBroadcast.h>

void RequestHealthChangeBroadcast::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    Serialization::WriteVarInt(aWriter, Sequence);
    Serialization::WriteFloat(aWriter, DeltaHealth);
}

void RequestHealthChangeBroadcast::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Sequence = Serialization::ReadVarInt(aReader);
    DeltaHealth = Serialization::ReadFloat(aReader);
}
