#include <Messages/SyncPlayerDownedState.h>

void SyncPlayerDownedState::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, IsDowned);
    Serialization::WriteFloat(aWriter, PositionX);
    Serialization::WriteFloat(aWriter, PositionY);
    Serialization::WriteFloat(aWriter, PositionZ);
    Serialization::WriteVarInt(aWriter, NearbyHostileCount);
}

void SyncPlayerDownedState::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    IsDowned = Serialization::ReadBool(aReader);
    PositionX = Serialization::ReadFloat(aReader);
    PositionY = Serialization::ReadFloat(aReader);
    PositionZ = Serialization::ReadFloat(aReader);
    NearbyHostileCount = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
}
