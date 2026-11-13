#include <Messages/RequestWorldItemPickup.h>
#include <TiltedCore/Serialization.hpp>

void RequestWorldItemPickup::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ItemId);
    Serialization::WriteVarInt(aWriter, PickerServerId);
    Item.Serialize(aWriter);
}

void RequestWorldItemPickup::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ItemId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    PickerServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Item.Deserialize(aReader);
}
