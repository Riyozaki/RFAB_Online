#pragma once
#include <TiltedCore/Buffer.hpp>
#include <cmath>
#include <cstdint>
#include <limits>

// Per-reference LIFE identity, not the NPC base/race or an ownership epoch. Zero means no
// negotiated scaling; an active stamp always records the original permanent HP and exact x2.
struct HealthScale
{
    uint64_t Life{};
    float Base{}, Maximum{};
    bool operator==(const HealthScale&) const = default;
    bool IsValid() const noexcept
    {
        if (!Life) return Base == 0.f && Maximum == 0.f;
        return Life < (1ull << 60) && std::isfinite(Base) && Base > 0.f && std::isfinite(Maximum) &&
            static_cast<double>(Base) * 2.0 == Maximum;
    }
    void Serialize(TiltedPhoques::Buffer::Writer& writer) const noexcept
    {
        writer.WriteBits(Life, 64);
        writer.WriteBytes(reinterpret_cast<const uint8_t*>(&Base), sizeof(Base));
        writer.WriteBytes(reinterpret_cast<const uint8_t*>(&Maximum), sizeof(Maximum));
    }
    void Deserialize(TiltedPhoques::Buffer::Reader& reader) noexcept
    {
        if (!reader.ReadBits(Life, 64) || !reader.ReadBytes(reinterpret_cast<uint8_t*>(&Base), sizeof(Base)) ||
            !reader.ReadBytes(reinterpret_cast<uint8_t*>(&Maximum), sizeof(Maximum)))
            Life = std::numeric_limits<uint64_t>::max(); // invalid, never a valid empty stamp on truncation
    }
};
