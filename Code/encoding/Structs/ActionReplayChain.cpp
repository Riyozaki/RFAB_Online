#include <Structs/ActionReplayChain.h>

using TiltedPhoques::Serialization;

bool ActionReplayChain::operator==(const ActionReplayChain& acRhs) const noexcept
{
    return ResetAnimationGraph == acRhs.ResetAnimationGraph && Actions == acRhs.Actions;
}

bool ActionReplayChain::operator!=(const ActionReplayChain& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void ActionReplayChain::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, ResetAnimationGraph);
    const size_t count = Actions.size() > 255 ? 255 : Actions.size();
    aWriter.WriteBits(count & 0xFF, 8);
    for (size_t i = 0; i < count; ++i)
    {
        Actions[i].GenerateDifferential(ActionEvent{}, aWriter);
    }
}

void ActionReplayChain::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ResetAnimationGraph = Serialization::ReadBool(aReader);
    uint64_t actionsCount = 0;
    aReader.ReadBits(actionsCount, 8);
    Actions.clear();
    Actions.reserve(actionsCount & 0xFF);
    for (size_t i = 0; i < (actionsCount & 0xFF) && !aReader.Eof(); ++i)
    {
        ActionEvent replayAction{};
        replayAction.ApplyDifferential(aReader);
        Actions.push_back(std::move(replayAction));
    }
}
