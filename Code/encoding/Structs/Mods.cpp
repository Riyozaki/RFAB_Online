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
    const size_t modCount = std::min(ModList.size(), size_t(4096));
    aWriter.WriteBits(modCount & 0x1FFF, 13);

    for (size_t i = 0; i < modCount; ++i)
    {
        const auto& entry = ModList[i];
        aWriter.WriteBits(entry.Id, 16);
        Serialization::WriteBool(aWriter, entry.IsLite);
        Serialization::WriteString(aWriter, entry.Filename);
    }
}

void Mods::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t data = 0;
    aReader.ReadBits(data, 13);

    const size_t modCount = std::min<size_t>(data & 0x1FFF, 4096);
    ModList.clear();
    ModList.reserve(std::min<size_t>(modCount, 256));
    for (size_t i = 0; i < modCount && !aReader.Eof(); ++i)
    {
        uint64_t idData = 0;
        aReader.ReadBits(idData, 16);
        Entry entry{};
        entry.Id = idData & 0xFFFF;
        entry.IsLite = Serialization::ReadBool(aReader);
        entry.Filename = Serialization::ReadString(aReader);
        ModList.push_back(std::move(entry));
    }
}
