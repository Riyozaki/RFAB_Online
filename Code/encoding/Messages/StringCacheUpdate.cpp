#include <Messages/StringCacheUpdate.h>

void StringCacheUpdate::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, StartId);
    Serialization::WriteVarInt(aWriter, Values.size() & 0xFFFF);
    for (const auto& value : Values)
    {
        Serialization::WriteString(aWriter, value);
    }
}

void StringCacheUpdate::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    StartId = Serialization::ReadVarInt(aReader) & 0xFFFF;

    const auto count = Serialization::ReadVarInt(aReader) & 0xFFFF;
    Values.clear();

    for (auto i = 0ull; i < count && !aReader.Eof(); ++i)
    {
        auto value = Serialization::ReadString(aReader);
        Values.push_back(std::move(value));
    }
}
