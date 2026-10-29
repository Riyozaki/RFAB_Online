#include <Messages/StringCacheUpdate.h>
#include <common/CoopPacketSafety.h>
#include <algorithm>

void StringCacheUpdate::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t count = std::min(Values.size(), CoopPacketSafety::kMaxStringCacheBatch);
    Serialization::WriteVarInt(aWriter, StartId);
    Serialization::WriteVarInt(aWriter, count);
    for (size_t i = 0; i < count; ++i)
    {
        Serialization::WriteString(aWriter, Values[i]);
    }
}

void StringCacheUpdate::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    StartId = Serialization::ReadVarInt(aReader) & 0xFFFF;

    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader) & 0xFFFF,
        CoopPacketSafety::kMaxStringCacheBatch);
    Values.clear();
    Values.reserve(std::min<size_t>(count, 256));

    for (size_t i = 0; i < count && !aReader.Eof(); ++i)
    {
        Values.push_back(Serialization::ReadString(aReader));
    }
}
