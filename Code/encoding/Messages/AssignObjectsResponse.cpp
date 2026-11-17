#include <Messages/AssignObjectsResponse.h>

void AssignObjectsResponse::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Objects.size() & 0xFF, 8);

    for (const auto& object : Objects)
    {
        object.Serialize(aWriter);
    }
}

void AssignObjectsResponse::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    uint64_t count = 0;
    aReader.ReadBits(count, 8);

    Objects.clear();

    for (size_t i = 0; i < (count & 0xFF) && !aReader.Eof(); ++i)
    {
        ObjectData obj{};
        obj.Deserialize(aReader);
        Objects.push_back(std::move(obj));
    }
}
