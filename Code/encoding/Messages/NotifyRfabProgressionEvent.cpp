#include <Messages/NotifyRfabProgressionEvent.h>

void NotifyRfabProgressionEvent::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, static_cast<uint64_t>(Kind));
    Serialization::WriteVarInt(aWriter, EpochId);
    Serialization::WriteString(aWriter, SourceKey);
    Serialization::WriteString(aWriter, EditorId);
    BookId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, static_cast<uint32_t>(XpDelta));
    Serialization::WriteVarInt(aWriter, static_cast<uint64_t>(AuthoritativeTotalXp));
    Serialization::WriteFloat(aWriter, GlobalValue);
}

void NotifyRfabProgressionEvent::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Kind = static_cast<EventKind>(Serialization::ReadVarInt(aReader) & 0xFF);
    EpochId = Serialization::ReadVarInt(aReader);
    SourceKey = Serialization::ReadString(aReader);
    EditorId = Serialization::ReadString(aReader);
    BookId.Deserialize(aReader);
    XpDelta = static_cast<int32_t>(static_cast<uint32_t>(Serialization::ReadVarInt(aReader)));
    AuthoritativeTotalXp = static_cast<int64_t>(Serialization::ReadVarInt(aReader));
    GlobalValue = Serialization::ReadFloat(aReader);
}
