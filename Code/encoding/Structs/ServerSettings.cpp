#include "ServerSettings.h"
#include <TiltedCore/Serialization.hpp>

using TiltedPhoques::Serialization;

bool ServerSettings::operator==(const ServerSettings& acRhs) const noexcept
{
    return Difficulty == acRhs.Difficulty &&
           GreetingsEnabled == acRhs.GreetingsEnabled &&
           PvpEnabled == acRhs.PvpEnabled &&
           SyncPlayerHomes == acRhs.SyncPlayerHomes &&
           DeathSystemEnabled == acRhs.DeathSystemEnabled &&
           SyncPlayerCalendar == acRhs.SyncPlayerCalendar &&
           AutoPartyJoin == acRhs.AutoPartyJoin &&
           HealthMultiplier == acRhs.HealthMultiplier &&
           HealthExtraBase == acRhs.HealthExtraBase &&
           DownedSystemEnabled == acRhs.DownedSystemEnabled &&
           ReviveDurationSec == acRhs.ReviveDurationSec &&
           ReviveHealthFraction == acRhs.ReviveHealthFraction &&
           PostReviveGraceSec == acRhs.PostReviveGraceSec &&
           CombatLeashRadiusMeters == acRhs.CombatLeashRadiusMeters &&
           CoordinatedSavesEnabled == acRhs.CoordinatedSavesEnabled &&
           PostCombatAutosaveIntervalSec == acRhs.PostCombatAutosaveIntervalSec;
}

bool ServerSettings::operator!=(const ServerSettings& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void ServerSettings::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Difficulty);
    Serialization::WriteBool(aWriter, GreetingsEnabled);
    Serialization::WriteBool(aWriter, PvpEnabled);
    Serialization::WriteBool(aWriter, SyncPlayerHomes);
    Serialization::WriteBool(aWriter, DeathSystemEnabled);
    Serialization::WriteBool(aWriter, SyncPlayerCalendar);
    Serialization::WriteBool(aWriter, AutoPartyJoin);
    Serialization::WriteFloat(aWriter, HealthMultiplier);

    Serialization::WriteFloat(aWriter, HealthExtraBase);
    Serialization::WriteBool(aWriter, DownedSystemEnabled);
    Serialization::WriteFloat(aWriter, ReviveDurationSec);
    Serialization::WriteFloat(aWriter, ReviveHealthFraction);
    Serialization::WriteFloat(aWriter, PostReviveGraceSec);
    Serialization::WriteFloat(aWriter, CombatLeashRadiusMeters);
    Serialization::WriteBool(aWriter, CoordinatedSavesEnabled);
    Serialization::WriteVarInt(aWriter, PostCombatAutosaveIntervalSec);
}

void ServerSettings::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Difficulty = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    GreetingsEnabled = Serialization::ReadBool(aReader);
    PvpEnabled = Serialization::ReadBool(aReader);
    SyncPlayerHomes = Serialization::ReadBool(aReader);
    DeathSystemEnabled = Serialization::ReadBool(aReader);
    SyncPlayerCalendar = Serialization::ReadBool(aReader);
    AutoPartyJoin = Serialization::ReadBool(aReader);
    HealthMultiplier = Serialization::ReadFloat(aReader);

    HealthExtraBase = Serialization::ReadFloat(aReader);
    DownedSystemEnabled = Serialization::ReadBool(aReader);
    ReviveDurationSec = Serialization::ReadFloat(aReader);
    ReviveHealthFraction = Serialization::ReadFloat(aReader);
    PostReviveGraceSec = Serialization::ReadFloat(aReader);
    CombatLeashRadiusMeters = Serialization::ReadFloat(aReader);
    CoordinatedSavesEnabled = Serialization::ReadBool(aReader);
    PostCombatAutosaveIntervalSec = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
