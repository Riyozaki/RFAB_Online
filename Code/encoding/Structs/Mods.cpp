#include <Structs/Mods.h>
#include <TiltedCore/Serialization.hpp>
#include <algorithm>

using TiltedPhoques::Serialization;

bool Mods::operator==(const Mods& acRhs) const noexcept
{
    return ModList == acRhs.ModList;
}

bool Mods::operator!=(const Mods& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void Mods::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const uint16_t modCount = std::min(ModList.size(), size_t(4096)) & 0xFFFF;
    aWriter.WriteBits(modCount, 13);

    for (auto& entry : ModList)
    {
        aWriter.WriteBits(entry.Id, 16);
        Serialization::WriteBool(aWriter, entry.IsLite);
        Serialization::WriteString(aWriter, entry.Filename);
    }
}

void Mods::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t data = 0;
    aReader.ReadBits(data, 13);

    const size_t modCount = std::min<size_t>(data & 0xFFFF, 4096);
    ModList.clear();
    for (size_t i = 0; i < modCount && !aReader.Eof(); ++i)
    {
        Entry entry{};
        uint64_t idBits = 0;
        aReader.ReadBits(idBits, 16);
        entry.Id = idBits & 0xFFFF;
        entry.IsLite = Serialization::ReadBool(aReader);
        entry.Filename = Serialization::ReadString(aReader);
        ModList.push_back(std::move(entry));
    }
}
