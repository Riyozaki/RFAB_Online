#include <Messages/SyncRfabProgressionEvent.h>

void SyncRfabProgressionEvent::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, static_cast<uint64_t>(Kind));
    Serialization::WriteVarInt(aWriter, EpochId);
    Serialization::WriteString(aWriter, SourceKey);
    Serialization::WriteString(aWriter, EditorId);
    BookId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, static_cast<uint32_t>(XpDelta));
    Serialization::WriteFloat(aWriter, GlobalValue);
}

void SyncRfabProgressionEvent::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Kind = static_cast<EventKind>(Serialization::ReadVarInt(aReader) & 0xFF);
    EpochId = Serialization::ReadVarInt(aReader);
    SourceKey = Serialization::ReadString(aReader);
    EditorId = Serialization::ReadString(aReader);
    BookId.Deserialize(aReader);
    XpDelta = static_cast<int32_t>(Serialization::ReadVarInt(aReader));
    GlobalValue = Serialization::ReadFloat(aReader);
}
