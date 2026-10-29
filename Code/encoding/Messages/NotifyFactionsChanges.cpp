#include <Messages/NotifyFactionsChanges.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>
#include <algorithm>

void NotifyFactionsChanges::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t count = std::min(Changes.size(), CoopPacketSafety::kMaxFactionChanges);
    Serialization::WriteVarInt(aWriter, count);

    size_t written = 0;
    for (auto& change : Changes)
    {
        if (written++ >= count)
            break;
        Serialization::WriteVarInt(aWriter, change.first);
        change.second.Serialize(aWriter);
    }
}

void NotifyFactionsChanges::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxFactionChanges);

    for (size_t i = 0; i < count && !aReader.Eof(); ++i)
    {
        auto& change = Changes[Serialization::ReadVarInt(aReader) & 0xFFFFFFFF];
        change.Deserialize(aReader);
    }
}
