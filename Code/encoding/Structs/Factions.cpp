#include <Structs/Factions.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>

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
    Serialization::WriteVarInt(aWriter, NpcFactions.size() & 0x1FF);

    for (auto& entry : NpcFactions)
    {
        entry.Serialize(aWriter);
    }

    Serialization::WriteVarInt(aWriter, ExtraFactions.size() & 0x1FF);

    for (auto& entry : ExtraFactions)
    {
        entry.Serialize(aWriter);
    }
}

void Factions::Deserialize(TiltedPhoques::Buffer::Reader& aReader)
{
    const size_t npcCount = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        0x1FF);

    NpcFactions.clear();
    for (size_t i = 0; i < npcCount && !aReader.Eof(); ++i)
    {
        Faction entry{};
        entry.Deserialize(aReader);
        NpcFactions.push_back(std::move(entry));
    }

    const size_t extraCount = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        0x1FF);

    ExtraFactions.clear();
    for (size_t i = 0; i < extraCount && !aReader.Eof(); ++i)
    {
        Faction entry{};
        entry.Deserialize(aReader);
        ExtraFactions.push_back(std::move(entry));
    }
}
