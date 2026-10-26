#include <Messages/RequestFactionsChanges.h>
#include <TiltedCore/Serialization.hpp>
#include <cassert>

void RequestFactionsChanges::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t count = Changes.size() > 255 ? 255 : Changes.size();
    aWriter.WriteBits(count & 0xFF, 8);

    size_t written = 0;
    for (auto& change : Changes)
    {
        if (written++ >= count)
            break;
        Serialization::WriteVarInt(aWriter, change.first);
        change.second.Serialize(aWriter);
    }
}

void RequestFactionsChanges::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t count = 0;
    aReader.ReadBits(count, 8);

    for (size_t i = 0; i < (count & 0xFF) && !aReader.Eof(); ++i)
    {
        auto& change = Changes[Serialization::ReadVarInt(aReader) & 0xFFFFFFFF];
        change.Deserialize(aReader);
    }
}
