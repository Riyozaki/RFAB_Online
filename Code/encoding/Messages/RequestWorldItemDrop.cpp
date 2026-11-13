#include <Messages/RequestWorldItemDrop.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>

void RequestWorldItemDrop::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Cookie);
    Serialization::WriteVarInt(aWriter, ActorServerId);
    Item.Serialize(aWriter);
    CellId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    Serialization::WriteFloat(aWriter, Position.x);
    Serialization::WriteFloat(aWriter, Position.y);
    Serialization::WriteFloat(aWriter, Position.z);
    Serialization::WriteFloat(aWriter, Rotation.x);
    Serialization::WriteFloat(aWriter, Rotation.y);
    Serialization::WriteFloat(aWriter, Rotation.z);
}

void RequestWorldItemDrop::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Cookie = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ActorServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Item.Deserialize(aReader);
    CellId.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);

    constexpr float kMaxAbs = CoopPacketSafety::kWorldCoordinateMaxAbs;
    Position.x = CoopPacketSafety::SanitizeFiniteFloat(Serialization::ReadFloat(aReader), -kMaxAbs, kMaxAbs, 0.0f);
    Position.y = CoopPacketSafety::SanitizeFiniteFloat(Serialization::ReadFloat(aReader), -kMaxAbs, kMaxAbs, 0.0f);
    Position.z = CoopPacketSafety::SanitizeFiniteFloat(Serialization::ReadFloat(aReader), -kMaxAbs, kMaxAbs, 0.0f);
    Rotation.x = CoopPacketSafety::SanitizeFiniteFloat(Serialization::ReadFloat(aReader), -1000.0f, 1000.0f, 0.0f);
    Rotation.y = CoopPacketSafety::SanitizeFiniteFloat(Serialization::ReadFloat(aReader), -1000.0f, 1000.0f, 0.0f);
    Rotation.z = CoopPacketSafety::SanitizeFiniteFloat(Serialization::ReadFloat(aReader), -1000.0f, 1000.0f, 0.0f);
}
