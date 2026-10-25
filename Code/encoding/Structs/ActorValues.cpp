#include <Structs/ActorValues.h>
#include <TiltedCore/Serialization.hpp>
#include <CoopPacketSafety.h>
#include <algorithm>

using TiltedPhoques::Serialization;

bool ActorValues::operator==(const ActorValues& acRhs) const noexcept
{
    return ActorValuesList == acRhs.ActorValuesList && ActorMaxValuesList == acRhs.ActorMaxValuesList;
}

bool ActorValues::operator!=(const ActorValues& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void ActorValues::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t valCount = std::min(ActorValuesList.size(), CoopPacketSafety::kMaxActorValues);
    Serialization::WriteVarInt(aWriter, valCount);
    size_t written = 0;
    for (auto& value : ActorValuesList)
    {
        if (written++ >= valCount)
            break;
        Serialization::WriteVarInt(aWriter, value.first);
        Serialization::WriteFloat(aWriter, value.second);
    }

    const size_t maxValCount = std::min(ActorMaxValuesList.size(), CoopPacketSafety::kMaxActorValues);
    Serialization::WriteVarInt(aWriter, maxValCount);
    written = 0;
    for (auto& value : ActorMaxValuesList)
    {
        if (written++ >= maxValCount)
            break;
        Serialization::WriteVarInt(aWriter, value.first);
        Serialization::WriteFloat(aWriter, value.second);
    }
}

void ActorValues::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxActorValues);
    for (size_t i = 0; i < count && !aReader.Eof(); i++)
    {
        const uint32_t key = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        const float value = Serialization::ReadFloat(aReader);
        ActorValuesList.insert({key, value});
    }

    const size_t maxCount = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxActorValues);
    for (size_t i = 0; i < maxCount && !aReader.Eof(); i++)
    {
        const uint32_t key = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        const float value = Serialization::ReadFloat(aReader);
        ActorMaxValuesList.insert({key, value});
    }
}
