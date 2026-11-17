#include <Structs/Inventory.h>
#include <cmath>
#include <limits>
#include <type_traits>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>

using TiltedPhoques::Serialization;

void Inventory::EffectItem::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteFloat(aWriter, Magnitude);
    Serialization::WriteVarInt(aWriter, Area);
    Serialization::WriteVarInt(aWriter, Duration);
    Serialization::WriteFloat(aWriter, RawCost);
    EffectId.Serialize(aWriter);
}

void Inventory::EffectItem::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Magnitude = Serialization::ReadFloat(aReader);
    Area = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Duration = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    RawCost = Serialization::ReadFloat(aReader);
    EffectId.Deserialize(aReader);
}

namespace
{
// This revision intentionally changes the wire format. Collection overflow invalidates the whole
// inventory; it must not clamp a count then interpret its unread tail as the next field.
struct InventoryWire
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
            constexpr size_t width = std::is_same_v<T, bool> ? 1 : sizeof(T) * 8;
            if (Reader) { Valid = Reader->ReadBits(bits, width); value = static_cast<T>(bits); }
            else Valid = Writer->WriteBits(bits, width);
        }
    }
    void Form(GameId& id) { Number(id.ModId); Number(id.BaseId); }
    void Item(Inventory::Entry& v)
    {
        Form(v.BaseId); Number(v.Count); Number(v.ExtraCharge); Form(v.ExtraEnchantId); Number(v.ExtraEnchantCharge);
        Number(v.EnchantData.IsWeapon);
        uint16_t count = static_cast<uint16_t>(v.EnchantData.Effects.size());
        if (!Reader && v.EnchantData.Effects.size() > CoopPacketSafety::kMaxEnchantEffects) Valid = false;
        Number(count);
        if (!Valid || count > CoopPacketSafety::kMaxEnchantEffects) { Valid = false; return; }
        if (Reader) v.EnchantData.Effects.resize(count);
        for (auto& e : v.EnchantData.Effects)
        { Number(e.Magnitude); Number(e.Area); Number(e.Duration); Number(e.RawCost); Form(e.EffectId); }
        Number(v.ExtraHealth); Form(v.ExtraPoisonId); Number(v.ExtraPoisonCount); Number(v.ExtraSoulLevel);
        Number(v.ExtraEnchantRemoveUnequip); Number(v.ExtraWorn); Number(v.ExtraWornLeft); Number(v.IsQuestItem);
        Number(v.ExtraPresence); Form(v.ExtraOwnerId); Form(v.ExtraUniqueBaseId); Number(v.ExtraUniqueId); Number(v.HasUnsupportedExtra);
        uint16_t length = static_cast<uint16_t>(v.ExtraTextDisplayName.size());
        if (!Reader && v.ExtraTextDisplayName.size() > 160) Valid = false;
        Number(length);
        if (!Valid || length > 160) { Valid = false; return; }
        if (Reader)
        {
            v.ExtraTextDisplayName.resize(length);
            Valid = Reader->ReadBytes(reinterpret_cast<uint8_t*>(v.ExtraTextDisplayName.data()), length);
        }
        else Valid = Writer->WriteBytes(reinterpret_cast<const uint8_t*>(v.ExtraTextDisplayName.data()), length);
        if (v.ExtraTextDisplayName.find('\0') != String::npos) Valid = false;
        bool footer = v.WireValid && Valid;
        Number(footer);
        Valid = Valid && footer;
        if (Reader) v.WireValid = Valid;
    }
    void Items(Inventory& v)
    {
        Number(v.Revision);
        uint16_t count = static_cast<uint16_t>(v.Entries.size());
        if (!Reader && v.Entries.size() > CoopPacketSafety::kMaxInventoryEntries) Valid = false;
        Number(count);
        if (!Valid || count > CoopPacketSafety::kMaxInventoryEntries) { Valid = false; return; }
        if (Reader) v.Entries.resize(count);
        for (auto& entry : v.Entries) { Item(entry); if (!Valid) break; }
        Form(v.CurrentMagicEquipment.LeftHandSpell); Form(v.CurrentMagicEquipment.RightHandSpell); Form(v.CurrentMagicEquipment.Shout);
        bool footer = v.WireValid && Valid;
        Number(footer);
        Valid = Valid && footer;
        if (Reader) v.WireValid = Valid;
    }
};
}
void Inventory::Entry::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    auto copy = *this;
    InventoryWire{nullptr, &aWriter}.Item(copy);
}
void Inventory::Entry::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    *this = {};
    InventoryWire codec{&aReader, nullptr};
    codec.Item(*this);
    WireValid = codec.Valid;
}

bool Inventory::operator==(const Inventory& acRhs) const noexcept
{
    return Entries == acRhs.Entries;
}

bool Inventory::operator!=(const Inventory& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

bool Inventory::Entry::operator==(const Inventory::Entry& acRhs) const noexcept
{
    return BaseId == acRhs.BaseId && Count == acRhs.Count && IsExtraDataEquals(acRhs);
}

bool Inventory::Entry::operator!=(const Inventory::Entry& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void Inventory::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    auto copy = *this;
    InventoryWire{nullptr, &aWriter}.Items(copy);
}
void Inventory::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    *this = {};
    InventoryWire codec{&aReader, nullptr};
    codec.Items(*this);
    WireValid = codec.Valid;
    if (!WireValid) Entries.clear();
}

std::optional<Inventory::Entry> Inventory::GetEntryById(GameId& aItemId) const noexcept
{
    auto entry = std::find_if(Entries.begin(), Entries.end(), [aItemId](auto entry) { return entry.BaseId == aItemId; });
    if (entry == Entries.end())
        return std::nullopt;

    return {*entry};
}

int32_t Inventory::GetEntryCountById(GameId& aItemId) const noexcept
{
    auto entry = GetEntryById(aItemId);
    if (!entry)
        return 0;

    return entry->Count;
}

bool Inventory::AddOrRemoveEntry(const Entry& acEntry) noexcept
{
    if (!WireValid || !acEntry.WireValid || acEntry.ExtraTextDisplayName.size() > 160 || acEntry.EnchantData.Effects.size() > CoopPacketSafety::kMaxEnchantEffects || acEntry.Count == 0 || acEntry.BaseId == GameId{} || !std::isfinite(acEntry.ExtraCharge) || !std::isfinite(acEntry.ExtraHealth))
        return false;
    auto duplicate = std::find_if(Entries.begin(), Entries.end(), [&acEntry](const Entry& entry) { return entry.CanBeMerged(acEntry); });
    const int64_t oldCount = duplicate != Entries.end() ? duplicate->Count : 0;
    const int64_t newCount = oldCount + static_cast<int64_t>(acEntry.Count);
    if (newCount < 0 || newCount > std::numeric_limits<int32_t>::max())
        return false;
    if (duplicate != Entries.end())
    {
        if (newCount == 0)
            Entries.erase(duplicate);
        else
            duplicate->Count = static_cast<int32_t>(newCount);
    }
    else if (newCount > 0)
        Entries.push_back(acEntry);
    return true;
}

bool Inventory::IsValid() const noexcept
{
    if (!WireValid || Entries.size() > CoopPacketSafety::kMaxInventoryEntries) return false;
    for (const auto& entry : Entries)
    {
        if (!entry.WireValid || entry.ExtraTextDisplayName.size() > 160 || entry.EnchantData.Effects.size() > CoopPacketSafety::kMaxEnchantEffects || entry.ExtraSoulLevel < 0 || entry.ExtraSoulLevel > 5 || entry.BaseId == GameId{} || entry.Count <= 0 || !std::isfinite(entry.ExtraCharge) || !std::isfinite(entry.ExtraHealth))
            return false;
        for (const auto& effect : entry.EnchantData.Effects)
            if (!std::isfinite(effect.Magnitude) || !std::isfinite(effect.RawCost))
                return false;
    }
    return true;
}

void Inventory::UpdateEquipment(const Inventory& acNewInventory) noexcept
{
    while (true)
    {
        auto wornEntry = std::find_if(Entries.begin(), Entries.end(), [](auto& aEntry) { return aEntry.IsWorn(); });
        if (wornEntry == Entries.end())
            break;

        wornEntry->ExtraWorn = wornEntry->ExtraWornLeft = false;
    }

    for (const auto& newEntry : acNewInventory.Entries)
    {
        if (!newEntry.IsWorn())
            continue;

        auto entry = std::find_if(Entries.begin(), Entries.end(), [&newEntry](auto& aEntry) { return aEntry.BaseId == newEntry.BaseId; });

        // This shouldn't happen
        if (entry == Entries.end())
            continue;

        entry->ExtraWorn = newEntry.ExtraWorn;
        entry->ExtraWornLeft = newEntry.ExtraWornLeft;
    }

    CurrentMagicEquipment = acNewInventory.CurrentMagicEquipment;
}

void Inventory::RemoveByFilter(std::function<bool(const Entry&)> aFilter) noexcept
{
    Entries.erase(std::remove_if(Entries.begin(), Entries.end(), aFilter), Entries.end());
}

bool Inventory::ContainsQuestItems() const noexcept
{
    for (const auto& entry : Entries)
    {
        if (entry.IsQuestItem)
            return true;
    }
    return false;
}
