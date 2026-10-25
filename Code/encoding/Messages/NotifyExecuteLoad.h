#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <Structs/Inventory.h>

struct NotifyExecuteLoad final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyExecuteLoad;

    NotifyExecuteLoad()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyExecuteLoad& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               EpochId == acRhs.EpochId &&
               SlotName == acRhs.SlotName &&
               IsPartyWipe == acRhs.IsPartyWipe &&
               AllowPreJoinFallback == acRhs.AllowPreJoinFallback &&
               Message == acRhs.Message &&
               UseInSessionRollback == acRhs.UseInSessionRollback &&
               HasRestoreTransform == acRhs.HasRestoreTransform &&
               TargetCellId == acRhs.TargetCellId &&
               TargetWorldSpaceId == acRhs.TargetWorldSpaceId &&
               PositionX == acRhs.PositionX &&
               PositionY == acRhs.PositionY &&
               PositionZ == acRhs.PositionZ &&
               RotationZ == acRhs.RotationZ &&
               HasRestoreInventory == acRhs.HasRestoreInventory &&
               RestoreInventory == acRhs.RestoreInventory &&
               RestoreHealthFraction == acRhs.RestoreHealthFraction;
    }

    uint64_t EpochId{};
    String SlotName{};
    bool IsPartyWipe{false};
    bool AllowPreJoinFallback{true};
    String Message{};

    // Stage 3 (MMO / SkyMP Model): Seamless in-session server checkpoint rollback without .ess reload or disconnect
    bool UseInSessionRollback{false};
    bool HasRestoreTransform{false};
    GameId TargetCellId{};
    GameId TargetWorldSpaceId{};
    float PositionX{0.0f};
    float PositionY{0.0f};
    float PositionZ{0.0f};
    float RotationZ{0.0f};
    bool HasRestoreInventory{false};
    Inventory RestoreInventory{};
    float RestoreHealthFraction{1.0f};
};
