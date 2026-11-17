#include <Messages/ClientReferencesMoveRequest.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>

void ClientReferencesMoveRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Tick);
    Serialization::WriteVarInt(aWriter, Updates.size());

    for (const auto& kvp : Updates)
    {
        Serialization::WriteVarInt(aWriter, kvp.first);
        kvp.second.Serialize(aWriter);
    }
}

void ClientReferencesMoveRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Tick = Serialization::ReadVarInt(aReader);
    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxMoveUpdates);

    for (size_t i = 0; i < count && !aReader.Eof(); ++i)
    {
        uint32_t serverId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        Updates[serverId].Deserialize(aReader);
    }
}
