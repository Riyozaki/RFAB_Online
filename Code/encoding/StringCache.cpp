#include <StringCache.h>
#include <iostream>

bool StringCache::Contains(const TiltedPhoques::String& acValue) const noexcept
{
    return m_stringToId.contains(acValue);
}

std::optional<uint32_t> StringCache::operator[](const TiltedPhoques::String& acValue) const noexcept
{
    if (const auto itor = m_stringToId.find(acValue); itor != std::end(m_stringToId))
        return itor->second;

    return std::nullopt;
}

std::optional<const TiltedPhoques::String> StringCache::operator[](uint32_t aValue) const noexcept
{
    if (aValue < m_idToString.size())
        return m_idToString[aValue];

    return std::nullopt;
}

uint32_t StringCache::Add(const TiltedPhoques::String& acValue) noexcept
{
    if (auto id = this->operator[](acValue))
        return *id;

    const auto allocatedId = m_idToString.size();
    m_stringToId[acValue] = allocatedId & 0xFFFFFFFF;
    m_idToString.push_back(acValue);

    return allocatedId;
}

void StringCache::AddWanted(const TiltedPhoques::String& acValue) noexcept
{
    // Remote peers decide what lands here, so the table must not be allowed to grow past the
    // range StringCacheUpdate::StartId can carry on the wire.
    if (m_idToString.size() + m_wantedStrings.size() >= kMaxEntries && !m_stringToId.contains(acValue))
    {
        ++m_droppedWantedCount;
        return;
    }

    m_wantedStrings.insert(acValue);
}

size_t StringCache::Size() const noexcept
{
    return m_idToString.size();
}

StringCacheUpdate StringCache::Serialize(uint32_t& aStartId) const noexcept
{
    StringCacheUpdate update;
    // Defensive: kMaxEntries keeps every legal offset inside the uint16_t wire field.
    update.StartId = static_cast<uint16_t>(aStartId <= kMaxEntries ? aStartId : kMaxEntries);

    if (aStartId < m_idToString.size())
    {
        auto itor = std::begin(m_idToString);
        std::advance(itor, aStartId);

        update.Values.insert(std::begin(update.Values), itor, std::end(m_idToString));

        aStartId = m_idToString.size();
    }

    return update;
}

bool StringCache::Deserialize(const StringCacheUpdate& aMessage) noexcept
{
    const auto cLocalSize = m_idToString.size();

    // A gap would shift every following id: the local table can never be repaired by appending,
    // so the update is dropped and the caller is told to resynchronize from scratch.
    if (aMessage.StartId > cLocalSize)
    {
        ++m_rejectedUpdateCount;
        return false;
    }

    // Replay / overlap: the sender restarted from an older offset. The common prefix must match
    // byte for byte, otherwise the two tables disagree on ids that were already handed out.
    size_t skip = cLocalSize - aMessage.StartId;
    if (skip > aMessage.Values.size())
        skip = aMessage.Values.size();

    for (size_t i = 0; i < skip; ++i)
    {
        if (m_idToString[aMessage.StartId + i] != aMessage.Values[i])
        {
            ++m_rejectedUpdateCount;
            return false;
        }
    }

    for (size_t i = skip; i < aMessage.Values.size(); ++i)
    {
        // Append-only: Add() must hand out exactly the id the sender used for this entry.
        const auto expectedId = m_idToString.size();
        const auto id = Add(aMessage.Values[i]);

        if (id != expectedId)
        {
            // The same string already exists under a different id, so the tables have diverged.
            ++m_rejectedUpdateCount;
            return false;
        }
    }

    return true;
}

void StringCache::NotifyUnknownId(uint32_t aId) noexcept
{
    (void)aId;
    ++m_unknownIdCount;
}

void StringCache::Clear() noexcept
{
    m_idToString.clear();
    m_wantedStrings.clear();
    m_stringToId.clear();
    m_unknownIdCount = 0;
    m_rejectedUpdateCount = 0;
    m_droppedWantedCount = 0;
}

bool StringCache::ProcessDirty() noexcept
{
    if (m_wantedStrings.empty())
        return false;

    for (auto& s : m_wantedStrings)
        Add(s);

    ClearDirty();

    return true;
}

void StringCache::ClearDirty() noexcept
{
    m_wantedStrings.clear();
}

StringCache& StringCache::Get() noexcept
{
    TiltedPhoques::ScopedAllocator _{TiltedPhoques::Allocator::GetDefault()};
    {
        static StringCache s_instance;
        return s_instance;
    }
}

StringCache::StringCache()
{
}
