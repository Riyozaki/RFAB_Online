#include <Messages/NotifyOwnershipTransfer.h>

void NotifyOwnershipTransfer::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    Serialization::WriteVarInt(aWriter, HealthRevision);
}

void NotifyOwnershipTransfer::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader);
    HealthRevision = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
