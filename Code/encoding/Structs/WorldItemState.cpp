#include <Structs/WorldItemState.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>

using TiltedPhoques::Serialization;

namespace
{
void WriteVec3(TiltedPhoques::Buffer::Writer& aWriter, const glm::vec3& acValue) noexcept
{
    Serialization::WriteFloat(aWriter, acValue.x);
    Serialization::WriteFloat(aWriter, acValue.y);
    Serialization::WriteFloat(aWriter, acValue.z);
}

glm::vec3 ReadVec3(TiltedPhoques::Buffer::Reader& aReader, float aMaxAbs) noexcept
{
    glm::vec3 value{};
    value.x = CoopPacketSafety::SanitizeFiniteFloat(Serialization::ReadFloat(aReader), -aMaxAbs, aMaxAbs, 0.0f);
    value.y = CoopPacketSafety::SanitizeFiniteFloat(Serialization::ReadFloat(aReader), -aMaxAbs, aMaxAbs, 0.0f);
    value.z = CoopPacketSafety::SanitizeFiniteFloat(Serialization::ReadFloat(aReader), -aMaxAbs, aMaxAbs, 0.0f);
    return value;
}
} // namespace

bool WorldItemState::operator==(const WorldItemState& acRhs) const noexcept
{
    return Id == acRhs.Id && Item == acRhs.Item && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId &&
           Position == acRhs.Position && Rotation == acRhs.Rotation && DropperPlayerId == acRhs.DropperPlayerId &&
           DropperCookie == acRhs.DropperCookie;
}

void WorldItemState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    Item.Serialize(aWriter);
    CellId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    WriteVec3(aWriter, Position);
    WriteVec3(aWriter, Rotation);
    Serialization::WriteVarInt(aWriter, DropperPlayerId);
    Serialization::WriteVarInt(aWriter, DropperCookie);
}

void WorldItemState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Id = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Item.Deserialize(aReader);
    if (Item.Count < 0)
        Item.Count = 0;
    CellId.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);
    Position = ReadVec3(aReader, CoopPacketSafety::kWorldCoordinateMaxAbs);
    Rotation = ReadVec3(aReader, 1000.0f);
    DropperPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    DropperCookie = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
