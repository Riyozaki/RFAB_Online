#include <Structs/CoopProtocol.h>
#include <algorithm>
#include <limits>

namespace Coop
{
namespace
{
// Strict, bounded fixed-width codec for the new protocol. In particular, no unbounded varint
// shifting, length masking or clamping a malformed vector and then reading the next field at
// the wrong offset. A failed read makes the ENTIRE action invalid; no partial action executes.
struct Codec
{
    TiltedPhoques::Buffer::Reader* Reader{};
    TiltedPhoques::Buffer::Writer* Writer{};
    bool Valid{true};
    size_t Elements{};

    template <class T> void Number(T& aValue)
    {
        if (!Valid)
            return;
        if constexpr (std::is_same_v<T, float>)
        {
            if (Reader)
                Valid = Reader->ReadBytes(reinterpret_cast<uint8_t*>(&aValue), sizeof(T));
            else
                Valid = Writer->WriteBytes(reinterpret_cast<const uint8_t*>(&aValue), sizeof(T));
            Valid = Valid && std::isfinite(aValue);
        }
        else
        {
            uint64_t value = static_cast<uint64_t>(aValue);
            constexpr size_t bits = std::is_same_v<T, bool> ? 1 : sizeof(T) * 8;
            if (Reader)
            {
                Valid = Reader->ReadBits(value, bits);
                aValue = static_cast<T>(value);
            }
            else
                Valid = Writer->WriteBits(value, bits);
        }
    }

    template <class T> void Value(T& aValue)
    {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            Number(aValue);
        else
            aValue.Fields([this](auto&... values) { (Value(values), ...); });
    }
    void Value(GameId& aValue) { Value(aValue.ModId); Value(aValue.BaseId); }
    void Value(std::string& aValue)
    {
        if (!Valid)
            return;
        uint16_t length = static_cast<uint16_t>(aValue.size());
        if (!Reader && aValue.size() > 160)
            Valid = false;
        Number(length);
        if (!Valid || length > 160)
        {
            Valid = false;
            return;
        }
        if (Reader)
        {
            aValue.resize(length);
            Valid = Reader->ReadBytes(reinterpret_cast<uint8_t*>(aValue.data()), length);
        }
        else
            Valid = Writer->WriteBytes(reinterpret_cast<const uint8_t*>(aValue.data()), length);
    }
    template <class T> void Value(std::vector<T>& aValue)
    {
        if (!Valid)
            return;
        uint16_t count = static_cast<uint16_t>(aValue.size());
        if (!Reader && aValue.size() > 4096)
            Valid = false;
        Number(count);
        Elements += count;
        if (!Valid || count > 4096 || Elements > 8192)
        {
            Valid = false;
            return;
        }
        if (Reader)
            aValue.resize(count);
        for (auto& entry : aValue)
        {
            Value(entry);
            if (!Valid)
                break;
        }
    }
    void Value(Inventory::EffectItem& aValue)
    {
        Value(aValue.Magnitude); Value(aValue.Area); Value(aValue.Duration);
        Value(aValue.RawCost); Value(aValue.EffectId);
    }
    void Value(Inventory::Entry& aValue)
    {
        Value(aValue.BaseId); Value(aValue.Count); Value(aValue.ExtraCharge);
        Value(aValue.ExtraEnchantId); Value(aValue.ExtraEnchantCharge);
        Value(aValue.EnchantData.IsWeapon);
        // TiltedPhoques::Vector has a different allocator; encode it via a bounded standard vector.
        std::vector<Inventory::EffectItem> effects;
        if (!Reader)
            effects.assign(aValue.EnchantData.Effects.begin(), aValue.EnchantData.Effects.end());
        Value(effects);
        if (effects.size() > 64)
            Valid = false;
        if (Reader && Valid)
            aValue.EnchantData.Effects.assign(effects.begin(), effects.end());
        Value(aValue.ExtraHealth); Value(aValue.ExtraPoisonId); Value(aValue.ExtraPoisonCount);
        Value(aValue.ExtraSoulLevel); Value(aValue.ExtraEnchantRemoveUnequip);
        Value(aValue.ExtraWorn); Value(aValue.ExtraWornLeft); Value(aValue.IsQuestItem);
        Value(aValue.ExtraPresence); Value(aValue.ExtraOwnerId); Value(aValue.ExtraUniqueBaseId); Value(aValue.ExtraUniqueId); Value(aValue.HasUnsupportedExtra);
        std::string name(aValue.ExtraTextDisplayName.data(), aValue.ExtraTextDisplayName.size());
        Value(name);
        if (name.find('\0') != std::string::npos) Valid = false;
        if (Reader && Valid) aValue.ExtraTextDisplayName.assign(name.begin(), name.end());
        Valid = Valid && aValue.WireValid;
    }
};

template <size_t I = 0> void ReadBody(uint8_t aIndex, Body& aBody, Codec& aCodec)
{
    if constexpr (I < std::variant_size_v<Body>)
    {
        if (aIndex == I)
        {
            aBody.emplace<I>();
            aCodec.Value(std::get<I>(aBody));
        }
        else
            ReadBody<I + 1>(aIndex, aBody, aCodec);
    }
    else
        aCodec.Valid = false;
}
bool ValidBody(const Body& aBody)
{
    return std::visit([](const auto& body) {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, EncounterState>)
            return body.Phase <= EncounterPhase::Ended;
        else if constexpr (std::is_same_v<T, ResetPlan> || std::is_same_v<T, ResetReceipt>)
            return body.Phase <= ResetPhase::Blocked;
        else if constexpr (std::is_same_v<T, TransferReply>)
            return body.Result <= TransferResult::Quarantined;
        else if constexpr (std::is_same_v<T, Checkpoint>)
            return body.Phase <= CheckpointPhase::Blocked;
        else if constexpr (std::is_same_v<T, Reward>)
            return body.Nonce && body.Kind <= RewardKind::Custom && body.Source && body.Occurrence && body.BaseXP > 0 && body.BaseXP <= 1000000 &&
                (body.Kind != RewardKind::Kill || (body.Actor && body.Incarnation)) &&
                (body.Kind != RewardKind::Book || (!body.Incarnation && body.Occurrence == 1 && !body.Context));
        else if constexpr (std::is_same_v<T, RewardGrant>)
            return body.RewardId != 0 && body.RewardId < (1ull << 60) && body.BaseXP > 0 && body.BaseXP <= 1000000 &&
                body.Result <= RewardResult::Quarantined && bool(body.BeforeRevision) == bool(body.BeforeDigest) &&
                body.BeforeRevision < std::numeric_limits<uint64_t>::max();
        else if constexpr (std::is_same_v<T, RewardReceipt>)
            return body.RewardId && body.RewardId < (1ull << 60) && body.FinalXP >= 0 && body.FinalXP <= 100000000 &&
                (body.Applied || !body.FinalXP) && bool(body.Revision) == bool(body.Digest);
        else if constexpr (std::is_same_v<T, RewardReport>)
            return body.Nonce && (body.Accepted ? body.RewardId && body.RewardId < (1ull << 60) : !body.RewardId);
        else if constexpr (std::is_same_v<T, RewardSource>)
            return body.Actor && body.Epoch && body.Incarnation && body.Source && body.Source.ModId < 0xFFFFFFFEu;
        else if constexpr (std::is_same_v<T, Hello>)
        {
            if (body.Rewards.size() > 16 || (body.Capabilities & SemanticRewards &&
                (!body.RewardSignature || !body.RewardKinds || (body.RewardKinds & ~15u)))) return false;
            std::vector<std::string> campaigns;
            for (const auto& reward : body.Rewards)
            {
                if (reward.Campaign.empty() || reward.Campaign.size() > 48 || !reward.Revision || !reward.Digest ||
                    std::find(campaigns.begin(), campaigns.end(), reward.Campaign) != campaigns.end()) return false;
                for (auto c : reward.Campaign)
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
                campaigns.push_back(reward.Campaign);
            }
            return true;
        }
        else if constexpr (std::is_same_v<T, Presence>)
            return !(body.Flags & ~uint32_t(Bleedout | InCombat | CanInteract | Grounded | Stable | Loading | UnsafeEncounter)) &&
                ((body.Flags & Loading) || body.Location.Valid());
        else if constexpr (std::is_same_v<T, Transfer>)
        {
            Inventory inventory;
            inventory.Entries.push_back(body.Item);
            return body.Nonce && body.NativeReason <= 5 && body.NativeReason != 2 && body.NativeReason != 3 &&
                body.Source && body.Destination && body.Source != body.Destination && inventory.IsValid();
        }
        else
            return true;
    }, aBody);
}
} // namespace

void Packet::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    // Fields() deliberately has no const-writing overload; this bounded value copy also avoids
    // accidentally giving a writer permission to mutate a live server/client state object.
    Packet copy = *this;
    Codec codec{nullptr, &aWriter};
    codec.Value(copy.Session); codec.Value(copy.Generation); codec.Value(copy.Operation);
    uint8_t index = static_cast<uint8_t>(copy.Content.index());
    codec.Value(index);
    std::visit([&codec](auto& value) { codec.Value(value); }, copy.Content);
    bool valid = codec.Valid && Valid && ValidBody(Content);
    codec.Number(valid);
}

void Packet::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Codec codec{&aReader, nullptr};
    codec.Value(Session); codec.Value(Generation); codec.Value(Operation);
    uint8_t index{};
    codec.Value(index);
    ReadBody(index, Content, codec);
    bool valid{};
    codec.Number(valid);
    Valid = codec.Valid && valid && ValidBody(Content);
}
} // namespace Coop
