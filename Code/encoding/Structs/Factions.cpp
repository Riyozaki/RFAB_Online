#include <Structs/Factions.h>
#include <TiltedCore/Serialization.hpp>
#include <CoopPacketSafety.h>
#include <algorithm>

using TiltedPhoques::Serialization;

bool Factions::operator==(const Factions& acRhs) const noexcept
{
    return NpcFactions == acRhs.NpcFactions && ExtraFactions == acRhs.ExtraFactions;
}

bool Factions::operator!=(const Factions& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void Factions::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    // Limit the number of factions to send
    const size_t npcCount = std::min<size_t>(NpcFactions.size(), 0x1FF);
    Serialization::WriteVarInt(aWriter, npcCount);

    for (size_t i = 0; i < npcCount; ++i)
    {
        NpcFactions[i].Serialize(aWriter);
    }

    const size_t extraCount = std::min<size_t>(ExtraFactions.size(), 0x1FF);
    Serialization::WriteVarInt(aWriter, extraCount);

    for (size_t i = 0; i < extraCount; ++i)
    {
        ExtraFactions[i].Serialize(aWriter);
    }
}

void Factions::Deserialize(TiltedPhoques::Buffer::Reader& aReader)
{
    const size_t npcCount = CoopPacketSafety::ClampCollectionCount(Serialization::ReadVarInt(aReader), 0x1FF);
    NpcFactions.clear();
    NpcFactions.reserve(std::min<size_t>(npcCount, 64));
    for (size_t i = 0; i < npcCount && !aReader.Eof(); ++i)
    {
        Faction entry{};
        entry.Deserialize(aReader);
        NpcFactions.push_back(entry);
    }

    const size_t extraCount = CoopPacketSafety::ClampCollectionCount(Serialization::ReadVarInt(aReader), 0x1FF);
    ExtraFactions.clear();
    ExtraFactions.reserve(std::min<size_t>(extraCount, 64));
    for (size_t i = 0; i < extraCount && !aReader.Eof(); ++i)
    {
        Faction entry{};
        entry.Deserialize(aReader);
        ExtraFactions.push_back(entry);
    }
}
