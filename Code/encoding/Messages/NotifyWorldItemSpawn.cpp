#include <Messages/NotifyWorldItemSpawn.h>
#include <TiltedCore/Serialization.hpp>

void NotifyWorldItemSpawn::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Revision);
    State.Serialize(aWriter);
}

void NotifyWorldItemSpawn::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Revision = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    State.Deserialize(aReader);
}
