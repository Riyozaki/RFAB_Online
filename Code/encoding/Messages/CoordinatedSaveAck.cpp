#include <Messages/CoordinatedSaveAck.h>

void CoordinatedSaveAck::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, EpochId);
    Serialization::WriteString(aWriter, SlotName);
    Serialization::WriteBool(aWriter, Success);
    Serialization::WriteString(aWriter, Reason);
}

void CoordinatedSaveAck::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    EpochId = Serialization::ReadVarInt(aReader);
    SlotName = Serialization::ReadString(aReader);
    Success = Serialization::ReadBool(aReader);
    Reason = Serialization::ReadString(aReader);
}
