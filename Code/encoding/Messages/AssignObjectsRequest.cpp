#include <Messages/AssignObjectsRequest.h>

void AssignObjectsRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Objects.size() & 0xFF, 8);

    for (const auto& object : Objects)
    {
        object.Serialize(aWriter);
    }
}

void AssignObjectsRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

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
