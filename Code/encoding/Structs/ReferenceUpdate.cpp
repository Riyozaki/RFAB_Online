#include <Structs/ReferenceUpdate.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>
#include <algorithm>

using TiltedPhoques::Serialization;

bool ReferenceUpdate::operator==(const ReferenceUpdate& acRhs) const noexcept
{
    return UpdatedMovement == acRhs.UpdatedMovement && ActionEvents == acRhs.ActionEvents;
}

bool ReferenceUpdate::operator!=(const ReferenceUpdate& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void ReferenceUpdate::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    UpdatedMovement.Serialize(aWriter);

    const size_t count = std::min(ActionEvents.size(), CoopPacketSafety::kMaxActionEvents);
    Serialization::WriteVarInt(aWriter, count);

    for (size_t i = 0; i < count; ++i)
    {
        ActionEvents[i].GenerateDifferential(ActionEvent{}, aWriter);
    }
}

void ReferenceUpdate::Deserialize(TiltedPhoques::Buffer::Reader& aReader)
{
    UpdatedMovement.Deserialize(aReader);

    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxActionEvents);

    ActionEvents.clear();
    ActionEvents.reserve(count);

    for (size_t i = 0; i < count && !aReader.Eof(); ++i)
    {
        ActionEvent ev{};
        ev.ApplyDifferential(aReader);
        ActionEvents.push_back(std::move(ev));
    }
}
