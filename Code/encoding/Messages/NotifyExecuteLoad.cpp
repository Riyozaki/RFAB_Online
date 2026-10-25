#include <Messages/NotifyExecuteLoad.h>

void NotifyExecuteLoad::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, EpochId);
    Serialization::WriteString(aWriter, SlotName);
    Serialization::WriteBool(aWriter, IsPartyWipe);
    Serialization::WriteBool(aWriter, AllowPreJoinFallback);
    Serialization::WriteString(aWriter, Message);

    Serialization::WriteBool(aWriter, UseInSessionRollback);
    Serialization::WriteBool(aWriter, HasRestoreTransform);
    if (HasRestoreTransform)
    {
        TargetCellId.Serialize(aWriter);
        TargetWorldSpaceId.Serialize(aWriter);
        Serialization::WriteFloat(aWriter, PositionX);
        Serialization::WriteFloat(aWriter, PositionY);
        Serialization::WriteFloat(aWriter, PositionZ);
        Serialization::WriteFloat(aWriter, RotationZ);
    }
    Serialization::WriteBool(aWriter, HasRestoreInventory);
    if (HasRestoreInventory)
    {
        RestoreInventory.Serialize(aWriter);
    }
    Serialization::WriteFloat(aWriter, RestoreHealthFraction);
}

void NotifyExecuteLoad::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    EpochId = Serialization::ReadVarInt(aReader);
    SlotName = Serialization::ReadString(aReader);
    IsPartyWipe = Serialization::ReadBool(aReader);
    AllowPreJoinFallback = Serialization::ReadBool(aReader);
    Message = Serialization::ReadString(aReader);

    UseInSessionRollback = Serialization::ReadBool(aReader);
    HasRestoreTransform = Serialization::ReadBool(aReader);
    if (HasRestoreTransform)
    {
        TargetCellId.Deserialize(aReader);
        TargetWorldSpaceId.Deserialize(aReader);
        PositionX = Serialization::ReadFloat(aReader);
        PositionY = Serialization::ReadFloat(aReader);
        PositionZ = Serialization::ReadFloat(aReader);
        RotationZ = Serialization::ReadFloat(aReader);
    }
    else
    {
        TargetCellId = {};
        TargetWorldSpaceId = {};
        PositionX = 0.0f;
        PositionY = 0.0f;
        PositionZ = 0.0f;
        RotationZ = 0.0f;
    }

    HasRestoreInventory = Serialization::ReadBool(aReader);
    if (HasRestoreInventory)
    {
        RestoreInventory.Deserialize(aReader);
    }
    else
    {
        RestoreInventory = {};
    }

    RestoreHealthFraction = Serialization::ReadFloat(aReader);
}
