#include <Messages/NotifyPartyJoined.h>
#include <TiltedCore/Serialization.hpp>

void NotifyPartyJoined::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteBool(aWriter, IsLeader);
    Serialization::WriteVarInt(aWriter, LeaderPlayerId);
    const size_t count = PlayerIds.size() > 255 ? 255 : PlayerIds.size();
    aWriter.WriteBits(count & 0xFF, 8);

    for (size_t i = 0; i < count; ++i)
    {
        Serialization::WriteVarInt(aWriter, PlayerIds[i]);
    }
}

void NotifyPartyJoined::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    IsLeader = Serialization::ReadBool(aReader);
    LeaderPlayerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    uint64_t count = 0;
    aReader.ReadBits(count, 8);

    PlayerIds.clear();
    PlayerIds.reserve(count & 0xFF);

    for (size_t i = 0; i < (count & 0xFF) && !aReader.Eof(); ++i)
    {
        PlayerIds.push_back(Serialization::ReadVarInt(aReader) & 0xFFFFFFFF);
    }
}
