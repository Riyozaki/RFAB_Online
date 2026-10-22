#include <Messages/NotifyPrepareSave.h>

void NotifyPrepareSave::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, EpochId);
    Serialization::WriteString(aWriter, SlotName);
    Serialization::WriteBool(aWriter, IsAutoCheckpoint);
}

void NotifyPrepareSave::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    EpochId = Serialization::ReadVarInt(aReader);
    SlotName = Serialization::ReadString(aReader);
    IsAutoCheckpoint = Serialization::ReadBool(aReader);
}
