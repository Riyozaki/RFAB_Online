#include <Messages/NotifySyncRfabExperience.h>

void NotifySyncRfabExperience::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Experience);
}

void NotifySyncRfabExperience::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Experience = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
}
