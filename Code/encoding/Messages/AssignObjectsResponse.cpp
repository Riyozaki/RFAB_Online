#include <Messages/AssignObjectsResponse.h>
#include <algorithm>

void AssignObjectsResponse::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t count = std::min<size_t>(Objects.size(), 255);
    aWriter.WriteBits(count & 0xFF, 8);

    for (size_t i = 0; i < count; ++i)
    {
        Objects[i].Serialize(aWriter);
    }
}

void AssignObjectsResponse::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    uint64_t count = 0;
    aReader.ReadBits(count, 8);

    Objects.clear();
    Objects.reserve(count & 0xFF);

    for (size_t i = 0; i < (count & 0xFF) && !aReader.Eof(); ++i)
    {
        ObjectData obj{};
        obj.Deserialize(aReader);
        Objects.push_back(std::move(obj));
    }
}
