#include <Messages/ServerReferencesMoveRequest.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>
#include <algorithm>

void ServerReferencesMoveRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Tick);
    const size_t count = std::min(Updates.size(), CoopPacketSafety::kMaxMoveUpdates);
    Serialization::WriteVarInt(aWriter, count);

    size_t written = 0;
    for (const auto& kvp : Updates)
    {
        if (written++ >= count)
            break;
        Serialization::WriteVarInt(aWriter, kvp.first);
        kvp.second.Serialize(aWriter);
    }
}

void ServerReferencesMoveRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Tick = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxMoveUpdates);

    for (size_t i = 0; i < count && !aReader.Eof(); ++i)
    {
        const uint32_t cServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        Updates[cServerId].Deserialize(aReader);
    }
}
