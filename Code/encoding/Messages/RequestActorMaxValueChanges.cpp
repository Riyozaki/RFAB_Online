#include <Messages/RequestActorMaxValueChanges.h>
#include <common/CoopPacketSafety.h>
#include <algorithm>

void RequestActorMaxValueChanges::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);

    const size_t count = std::min(Values.size(), CoopPacketSafety::kMaxActorValues);
    Serialization::WriteVarInt(aWriter, count);
    size_t written = 0;
    for (const auto& value : Values)
    {
        if (written++ >= count)
            break;
        Serialization::WriteVarInt(aWriter, value.first);
        Serialization::WriteFloat(aWriter, value.second);
    }
}

void RequestActorMaxValueChanges::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxActorValues);
    for (size_t i = 0; i < count && !aReader.Eof(); i++)
    {
        const uint32_t key = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        const float value = Serialization::ReadFloat(aReader);
        Values.insert({key, value});
    }
}
