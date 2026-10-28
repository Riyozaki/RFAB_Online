#pragma once

#include "CachedString.h"

using TiltedPhoques::Buffer;
using TiltedPhoques::Vector;

struct Tints
{
    static constexpr uint32_t kHairColorEntryType = 0x72664843u; // 'rfHC'
    static constexpr const char* kHairColorEntryName = "__rfab_hair_color__";

    struct Entry
    {
        CachedString Name;
        float Alpha{};
        uint32_t Color{};
        uint32_t Type{};

        bool operator==(const Entry& acRhs) const noexcept;
        bool operator!=(const Entry& acRhs) const noexcept;
    };

    Tints() = default;
    ~Tints() = default;

    bool operator==(const Tints& acRhs) const noexcept;
    bool operator!=(const Tints& acRhs) const noexcept;

    static bool IsHairColorEntry(const Entry& acEntry) noexcept;
    void SetHairColor(uint8_t aRed, uint8_t aGreen, uint8_t aBlue) noexcept;
    bool TryGetHairColor(uint8_t& aOutRed, uint8_t& aOutGreen, uint8_t& aOutBlue) const noexcept;
    size_t CountFaceTintEntries() const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    Vector<Entry> Entries;
};
