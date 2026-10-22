#include <Messages/SyncRfabExperienceRequest.h>

void SyncRfabExperienceRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Experience);
    Serialization::WriteVarInt(aWriter, Sequence);
}

void SyncRfabExperienceRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Experience = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    Sequence = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
}
