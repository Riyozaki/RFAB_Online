#include <Messages/NotifyExecuteLoad.h>

void NotifyExecuteLoad::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, EpochId);
    Serialization::WriteString(aWriter, SlotName);
    Serialization::WriteBool(aWriter, IsPartyWipe);
    Serialization::WriteBool(aWriter, AllowPreJoinFallback);
    Serialization::WriteString(aWriter, Message);
}

void NotifyExecuteLoad::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    EpochId = Serialization::ReadVarInt(aReader);
    SlotName = Serialization::ReadString(aReader);
    IsPartyWipe = Serialization::ReadBool(aReader);
    AllowPreJoinFallback = Serialization::ReadBool(aReader);
    Message = Serialization::ReadString(aReader);
}
