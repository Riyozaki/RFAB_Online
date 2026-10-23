#pragma once

#include "Message.h"

struct RequestCoordinatedSaveLoad final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestCoordinatedSaveLoad;

    enum class ActionType : uint8_t
    {
        ManualSave = 0,
        LoadLatestCheckpoint = 1,
        LoadSpecificSlot = 2,
        LoadPreviousCheckpoint = 3,
    };

    RequestCoordinatedSaveLoad()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestCoordinatedSaveLoad& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               Action == acRhs.Action &&
               SlotName == acRhs.SlotName;
    }

    ActionType Action{ActionType::ManualSave};
    String SlotName{};
};
