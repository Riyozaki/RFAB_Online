
#include <Structs/QuestLog.h>

#include <TiltedCore/Serialization.hpp>
#include <Structs/Mods.h>
#include <CoopPacketSafety.h>
#include <algorithm>

using TiltedPhoques::Serialization;

bool QuestLog::operator==(const QuestLog& acRhs) const noexcept
{
    return Entries == acRhs.Entries;
}

bool QuestLog::operator!=(const QuestLog& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void QuestLog::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t count = std::min(Entries.size(), CoopPacketSafety::kMaxQuests);
    aWriter.WriteBits(count & 0xFFFF, 16);
    for (size_t i = 0; i < count; ++i)
    {
        const auto& e = Entries[i];
        e.Id.Serialize(aWriter);

        // id's can be in the full 16 bit range
        aWriter.WriteBits(e.Stage, 16);
    }
}

void QuestLog::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t temp = 0;
    aReader.ReadBits(temp, 16);

    const size_t count = CoopPacketSafety::ClampCollectionCount(temp & 0xFFFF, CoopPacketSafety::kMaxQuests);
    Entries.clear();
    Entries.reserve(std::min<size_t>(count, 256));

    for (size_t i = 0; i < count && !aReader.Eof(); ++i)
    {
        Entry e{};
        e.Id.Deserialize(aReader);
        uint64_t stageBits = 0;
        aReader.ReadBits(stageBits, 16);
        e.Stage = stageBits & 0xFFFF;
        Entries.push_back(e);
    }
}
