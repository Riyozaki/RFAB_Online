#include <Structs/ActorValues.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>
#include <cmath>
#include <type_traits>

using TiltedPhoques::Serialization;

bool ActorValues::operator==(const ActorValues& acRhs) const noexcept
{
    return HealthRevision == acRhs.HealthRevision && Scale == acRhs.Scale && HealthSequences == acRhs.HealthSequences && ActorValuesList == acRhs.ActorValuesList && ActorMaxValuesList == acRhs.ActorMaxValuesList;
}

bool ActorValues::operator!=(const ActorValues& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

namespace
{
struct ValuesWire
{
    TiltedPhoques::Buffer::Reader* Reader{};
    TiltedPhoques::Buffer::Writer* Writer{};
    bool Valid{true};
    template <class T> void Number(T& value)
    {
        if (!Valid) return;
        if constexpr (std::is_same_v<T, float>)
        {
            if (Reader) Valid = Reader->ReadBytes(reinterpret_cast<uint8_t*>(&value), sizeof(value));
            else Valid = Writer->WriteBytes(reinterpret_cast<const uint8_t*>(&value), sizeof(value));
            Valid = Valid && std::isfinite(value);
        }
        else
        {
            uint64_t bits = static_cast<uint64_t>(value);
            constexpr auto width = std::is_same_v<T, bool> ? 1 : sizeof(T) * 8;
            if (Reader) { Valid = Reader->ReadBits(bits, width); value = static_cast<T>(bits); }
            else Valid = Writer->WriteBits(bits, width);
        }
    }
    template <class Map> void Entries(Map& values, uint16_t limit)
    {
        uint16_t count = static_cast<uint16_t>(values.size());
        if (!Reader && values.size() > limit) Valid = false;
        Number(count);
        if (!Valid || count > limit) { Valid = false; return; }
        if (Reader)
        {
            values.clear();
            for (uint16_t i = 0; i < count && Valid; ++i)
            {
                typename Map::key_type key{};
                typename Map::mapped_type value{};
                Number(key); Number(value);
                if (!values.emplace(key, value).second) Valid = false;
            }
        }
        else for (const auto& [key, value] : values) { auto k = key; auto v = value; Number(k); Number(v); }
    }
    void Values(ActorValues& value)
    {
        Number(value.HealthRevision);
        Number(value.Scale.Life); Number(value.Scale.Base); Number(value.Scale.Maximum);
        Entries(value.HealthSequences, 32);
        Entries(value.ActorValuesList, CoopPacketSafety::kMaxActorValues);
        Entries(value.ActorMaxValuesList, CoopPacketSafety::kMaxActorValues);
        bool valid = Valid && value.WireValid;
        Number(valid);
        Valid = Valid && valid;
    }
};
}
bool ActorValues::IsValid() const noexcept
{
    if (!WireValid || !Scale.IsValid() || HealthSequences.size() > 32 || ActorValuesList.size() > CoopPacketSafety::kMaxActorValues ||
        ActorMaxValuesList.size() > CoopPacketSafety::kMaxActorValues) return false;
    if (Scale.Life)
    {
        const auto maximum = ActorMaxValuesList.find(24);
        if (maximum == ActorMaxValuesList.end() || maximum->second != Scale.Maximum) return false;
    }
    for (const auto& [origin, sequence] : HealthSequences) if (!origin || !sequence) return false;
    for (const auto& [key, value] : ActorValuesList) if (key >= 256 || !std::isfinite(value)) return false;
    for (const auto& [key, value] : ActorMaxValuesList) if (key >= 256 || !std::isfinite(value)) return false;
    return true;
}
void ActorValues::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    auto copy = *this;
    copy.WireValid = IsValid();
    ValuesWire{nullptr, &aWriter}.Values(copy);
}
void ActorValues::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    *this = {};
    ValuesWire codec{&aReader, nullptr};
    codec.Values(*this);
    WireValid = codec.Valid && IsValid();
    if (!WireValid) { HealthSequences.clear(); ActorValuesList.clear(); ActorMaxValuesList.clear(); }
}
