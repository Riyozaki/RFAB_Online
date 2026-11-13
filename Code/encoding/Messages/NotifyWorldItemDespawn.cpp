#include <Messages/NotifyWorldItemDespawn.h>
#include <TiltedCore/Serialization.hpp>

void NotifyWorldItemDespawn::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Revision);
    Serialization::WriteVarInt(aWriter, ItemId);
    aWriter.WriteBits(static_cast<uint8_t>(Reason), 8);
    Serialization::WriteVarInt(aWriter, PickerPlayerId);
}

void NotifyWorldItemDespawn::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Revision = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    ItemId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t rawReason = 0;
    aReader.ReadBits(rawReason, 8);
    Reason = WorldItems::SanitizeDespawnReason(static_cast<uint8_t>(rawReason & 0xFF));
    PickerPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
