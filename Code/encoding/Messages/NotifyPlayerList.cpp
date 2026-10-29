#include <Messages/NotifyPlayerList.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>
#include <algorithm>

void NotifyPlayerList::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t count = std::min(Players.size(), CoopPacketSafety::kMaxPlayerListEntries);
    Serialization::WriteVarInt(aWriter, count);

    size_t written = 0;
    for (auto& player : Players)
    {
        if (written++ >= count)
            break;
        Serialization::WriteVarInt(aWriter, player.first);
        Serialization::WriteString(aWriter, player.second);
    }
}

void NotifyPlayerList::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader) & 0xFFFF,
        CoopPacketSafety::kMaxPlayerListEntries);

    for (size_t i = 0; i < count && !aReader.Eof(); ++i)
    {
        uint32_t id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        Players[id] = Serialization::ReadString(aReader);
    }
}
