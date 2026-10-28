#include <Structs/Tints.h>
#include <common/CoopHairColorSync.h>
#include <TiltedCore/Serialization.hpp>
#include <cstring>
#include <stdexcept>

using TiltedPhoques::Serialization;

bool Tints::IsHairColorEntry(const Entry& acEntry) noexcept
{
    if (acEntry.Type == kHairColorEntryType)
        return true;

    const char* pName = acEntry.Name.c_str();
    return pName && std::strcmp(pName, kHairColorEntryName) == 0;
}

void Tints::SetHairColor(const uint8_t aRed, const uint8_t aGreen, const uint8_t aBlue) noexcept
{
    const uint32_t packedRgb = CoopHairColorSync::PackHairColorRgb(aRed, aGreen, aBlue);

    for (size_t i = 0; i < Entries.size(); ++i)
    {
        if (IsHairColorEntry(Entries[i]))
        {
            Entries[i].Type = kHairColorEntryType;
            Entries[i].Color = packedRgb;
            Entries[i].Alpha = 1.0f;
            Entries[i].Name = kHairColorEntryName;
            // Ensure the hair color entry sits within the 255-entry wire serialization window.
            if (i >= 255u)
                std::swap(Entries[254], Entries[i]);
            return;
        }
    }

    Entry hairEntry{};
    hairEntry.Name = kHairColorEntryName;
    hairEntry.Alpha = 1.0f;
    hairEntry.Color = packedRgb;
    hairEntry.Type = kHairColorEntryType;

    if (Entries.size() >= 255u)
        Entries[254] = std::move(hairEntry);
    else
        Entries.push_back(std::move(hairEntry));
}

bool Tints::TryGetHairColor(uint8_t& aOutRed, uint8_t& aOutGreen, uint8_t& aOutBlue) const noexcept
{
    for (const auto& entry : Entries)
    {
        if (IsHairColorEntry(entry))
        {
            CoopHairColorSync::UnpackHairColorRgb(entry.Color, aOutRed, aOutGreen, aOutBlue);
            return true;
        }
    }
    return false;
}

size_t Tints::CountFaceTintEntries() const noexcept
{
    size_t count = 0;
    for (const auto& entry : Entries)
    {
        if (!IsHairColorEntry(entry))
            ++count;
    }
    return count;
}

bool Tints::Entry::operator==(const Entry& acRhs) const noexcept
{
    return Alpha == acRhs.Alpha && Type == acRhs.Type && Color == acRhs.Color && Name == acRhs.Name;
}

bool Tints::Entry::operator!=(const Entry& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

bool Tints::operator==(const Tints& acRhs) const noexcept
{
    return Entries == acRhs.Entries;
}

bool Tints::operator!=(const Tints& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void Tints::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t count = std::min<size_t>(Entries.size(), 255);
    aWriter.WriteBits(count & 0xFF, 8);

    for (size_t i = 0; i < count; ++i)
    {
        const auto& entry = Entries[i];
        Serialization::WriteVarInt(aWriter, entry.Type);
        aWriter.WriteBits(entry.Color, 32);
        entry.Name.Serialize(aWriter);
        aWriter.WriteBits(*reinterpret_cast<const uint32_t*>(&entry.Alpha), 32);
    }
}

void Tints::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t tmp = 0;
    aReader.ReadBits(tmp, 8);

    const auto cCount = tmp & 0xFF;
    Entries.clear();
    Entries.reserve(cCount);

    for (auto i = 0u; i < cCount && !aReader.Eof(); ++i)
    {
        uint64_t buffer = 0;

        Entry entry{};
        entry.Type = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

        aReader.ReadBits(buffer, 32);
        entry.Color = buffer & 0xFFFFFFFF;
        entry.Name.Deserialize(aReader);

        buffer = 0;
        aReader.ReadBits(buffer, 32);
        uint32_t alphaBits = static_cast<uint32_t>(buffer & 0xFFFFFFFF);
        entry.Alpha = *reinterpret_cast<float*>(&alphaBits);

        Entries.push_back(entry);
    }
}
