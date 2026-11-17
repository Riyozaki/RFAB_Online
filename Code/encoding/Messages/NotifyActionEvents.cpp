#include <Messages/NotifyActionEvents.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>

void NotifyActionEvents::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Actions.size());

    for (const auto& [serverId, actions] : Actions)
    {
        Serialization::WriteVarInt(aWriter, serverId);
        Serialization::WriteVarInt(aWriter, actions.size());

        for (const auto& action : actions)
            action.GenerateDifferential(ActionEvent{}, aWriter);
    }
}

void NotifyActionEvents::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxMoveUpdates);

    for (size_t i = 0; i < count && !aReader.Eof(); ++i)
    {
        const uint32_t cServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        auto& actions = Actions[cServerId];

        const size_t actionCount = CoopPacketSafety::ClampCollectionCount(
            Serialization::ReadVarInt(aReader),
            CoopPacketSafety::kMaxActionEvents);
        actions.clear();

        for (size_t j = 0; j < actionCount && !aReader.Eof(); ++j)
        {
            ActionEvent action{};
            action.ApplyDifferential(aReader);
            actions.push_back(std::move(action));
        }
    }
}
