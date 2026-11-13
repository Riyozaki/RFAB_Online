#include <Messages/RequestResync.h>

void RequestResync::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, RequestId);
    aWriter.WriteBits(static_cast<uint8_t>(RequestScope), 8);
    Serialization::WriteVarInt(aWriter, ServerId);
    aWriter.WriteBits(static_cast<uint8_t>(GapReason), 8);
}

void RequestResync::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    RequestId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    uint64_t rawScope = 0;
    aReader.ReadBits(rawScope, 8);
    RawScopeKnown = Session::Resync::IsKnownScope(static_cast<uint8_t>(rawScope & 0xFF));
    RequestScope = Session::Resync::SanitizeScope(static_cast<uint8_t>(rawScope & 0xFF));

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    uint64_t rawReason = 0;
    aReader.ReadBits(rawReason, 8);
    GapReason = Session::Resync::SanitizeReason(static_cast<uint8_t>(rawReason & 0xFF));
}
