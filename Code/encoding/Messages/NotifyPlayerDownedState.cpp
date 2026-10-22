#include <Messages/NotifyPlayerDownedState.h>

void NotifyPlayerDownedState::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, PlayerId);
    Serialization::WriteVarInt(aWriter, CharacterServerId);
    Serialization::WriteBool(aWriter, IsDowned);
    Serialization::WriteFloat(aWriter, PositionX);
    Serialization::WriteFloat(aWriter, PositionY);
    Serialization::WriteFloat(aWriter, PositionZ);
}

void NotifyPlayerDownedState::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    PlayerId = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    CharacterServerId = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    IsDowned = Serialization::ReadBool(aReader);
    PositionX = Serialization::ReadFloat(aReader);
    PositionY = Serialization::ReadFloat(aReader);
    PositionZ = Serialization::ReadFloat(aReader);
}
