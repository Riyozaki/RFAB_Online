#include <Messages/NotifyResyncComplete.h>

void NotifyResyncComplete::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, RequestId);
    aWriter.WriteBits(static_cast<uint8_t>(RequestScope), 8);
    Serialization::WriteVarInt(aWriter, ServerId);
    aWriter.WriteBits(static_cast<uint8_t>(Result), 8);
    Serialization::WriteVarInt(aWriter, SnapshotsSent);
}

void NotifyResyncComplete::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    RequestId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    uint64_t rawScope = 0;
    aReader.ReadBits(rawScope, 8);
    RequestScope = Session::Resync::SanitizeScope(static_cast<uint8_t>(rawScope & 0xFF));

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    uint64_t rawResult = 0;
    aReader.ReadBits(rawResult, 8);
    Result = Session::Resync::SanitizeOutcome(static_cast<uint8_t>(rawResult & 0xFF));

    SnapshotsSent = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
