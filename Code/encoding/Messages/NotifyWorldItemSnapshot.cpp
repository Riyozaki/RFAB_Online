#include <Messages/NotifyWorldItemSnapshot.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>
#include <common/Session/WorldItems.h>

void NotifyWorldItemSnapshot::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Revision);
    const size_t count = std::min<size_t>(Items.size(), WorldItems::kMaxSnapshotItems);
    Serialization::WriteVarInt(aWriter, count);
    for (size_t i = 0; i < count; ++i)
        Items[i].Serialize(aWriter);
}

void NotifyWorldItemSnapshot::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Revision = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    const size_t count = CoopPacketSafety::ClampCollectionCount(Serialization::ReadVarInt(aReader), WorldItems::kMaxSnapshotItems);
    Items.clear();
    Items.reserve(count);
    for (size_t i = 0; i < count && !aReader.Eof(); ++i)
    {
        WorldItemState state{};
        state.Deserialize(aReader);
        Items.push_back(std::move(state));
    }
}
