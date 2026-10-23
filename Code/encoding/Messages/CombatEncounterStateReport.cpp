#include <Messages/CombatEncounterStateReport.h>

void CombatEncounterStateReport::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, InCombat);
    Serialization::WriteVarInt(aWriter, NearbyHostileCount);
    Serialization::WriteFloat(aWriter, NearestHostileDistanceMeters);
    Serialization::WriteBool(aWriter, BossDefeatedInEncounter);
    Serialization::WriteFloat(aWriter, PositionX);
    Serialization::WriteFloat(aWriter, PositionY);
    Serialization::WriteFloat(aWriter, PositionZ);
}

void CombatEncounterStateReport::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    InCombat = Serialization::ReadBool(aReader);
    NearbyHostileCount = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    NearestHostileDistanceMeters = Serialization::ReadFloat(aReader);
    BossDefeatedInEncounter = Serialization::ReadBool(aReader);
    PositionX = Serialization::ReadFloat(aReader);
    PositionY = Serialization::ReadFloat(aReader);
    PositionZ = Serialization::ReadFloat(aReader);
}
