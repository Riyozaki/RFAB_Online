#include <Messages/NotifyCommitSave.h>

void NotifyCommitSave::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, EpochId);
    Serialization::WriteString(aWriter, SlotName);
    Serialization::WriteBool(aWriter, Committed);
    Serialization::WriteString(aWriter, Message);
}

void NotifyCommitSave::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    EpochId = Serialization::ReadVarInt(aReader);
    SlotName = Serialization::ReadString(aReader);
    Committed = Serialization::ReadBool(aReader);
    Message = Serialization::ReadString(aReader);
}
