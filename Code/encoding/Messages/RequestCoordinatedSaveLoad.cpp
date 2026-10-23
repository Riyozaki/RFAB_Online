#include <Messages/RequestCoordinatedSaveLoad.h>

void RequestCoordinatedSaveLoad::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, static_cast<uint64_t>(Action));
    Serialization::WriteString(aWriter, SlotName);
}

void RequestCoordinatedSaveLoad::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Action = static_cast<ActionType>(Serialization::ReadVarInt(aReader) & 0xFF);
    SlotName = Serialization::ReadString(aReader);
}
