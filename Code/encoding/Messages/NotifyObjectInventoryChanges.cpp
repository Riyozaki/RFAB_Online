#include <Messages/NotifyObjectInventoryChanges.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>
#include <algorithm>

void NotifyObjectInventoryChanges::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t count = std::min(Changes.size(), CoopPacketSafety::kMaxObjectInventoryChanges);
    Serialization::WriteVarInt(aWriter, count);

    size_t written = 0;
    for (auto& change : Changes)
    {
        if (written++ >= count)
            break;
        change.first.Serialize(aWriter);
        change.second.Serialize(aWriter);
    }
}

void NotifyObjectInventoryChanges::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader) & 0xFF,
        CoopPacketSafety::kMaxObjectInventoryChanges);

    for (size_t i = 0; i < count && !aReader.Eof(); ++i)
    {
        GameId gameId;
        gameId.Deserialize(aReader);

        auto& change = Changes[gameId];
        change.Deserialize(aReader);
    }
}
