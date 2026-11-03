#pragma once

#include <Messages/StringCacheUpdate.h>

struct StringCache
{
    TP_NOCOPYMOVE(StringCache);

    /**
     * @brief Hard ceiling on table entries.
     *
     * StringCacheUpdate::StartId is a uint16_t on the wire, so an offset above this value cannot be
     * represented and would wrap, shifting every id for every peer. Entries arrive from remote
     * literals, so the bound has to be enforced where they enter the table.
     */
    static constexpr size_t kMaxEntries = 0xFFFFu;

    [[nodiscard]] bool Contains(const TiltedPhoques::String&) const noexcept;

    [[nodiscard]] std::optional<uint32_t> operator[](const TiltedPhoques::String&) const noexcept;
    [[nodiscard]] std::optional<const TiltedPhoques::String> operator[](uint32_t) const noexcept;
    uint32_t Add(const TiltedPhoques::String&) noexcept;
    [[nodiscard]] void AddWanted(const TiltedPhoques::String&) noexcept;
    [[nodiscard]] size_t Size() const noexcept;
    [[nodiscard]] StringCacheUpdate Serialize(uint32_t& aStartId) const noexcept;

    /**
     * @brief Applies a replicated string table update.
     *
     * The table is append-only and index-addressed, so an update that does not line up with the
     * local table would silently shift every subsequent id and permanently corrupt every
     * CachedString of the session. Such an update is therefore rejected instead of applied.
     *
     * @return true when the local table is up to date with the sender after this call,
     *         false when the update was rejected (gap or mismatching overlap).
     */
    bool Deserialize(const StringCacheUpdate& aMessage) noexcept;

    /// Records that a peer referenced an id we do not know about (diagnostics only).
    void NotifyUnknownId(uint32_t aId) noexcept;
    [[nodiscard]] uint64_t GetUnknownIdCount() const noexcept { return m_unknownIdCount; }
    [[nodiscard]] uint64_t GetRejectedUpdateCount() const noexcept { return m_rejectedUpdateCount; }
    [[nodiscard]] uint64_t GetDroppedWantedCount() const noexcept { return m_droppedWantedCount; }

    void Clear() noexcept;
    bool ProcessDirty() noexcept;
    void ClearDirty() noexcept;

    static StringCache& Get() noexcept;

private:
    TiltedPhoques::Vector<TiltedPhoques::String> m_idToString;
    mutable TiltedPhoques::Set<TiltedPhoques::String> m_wantedStrings;
    TiltedPhoques::Map<TiltedPhoques::String, uint32_t> m_stringToId;
    uint64_t m_unknownIdCount{0};
    uint64_t m_rejectedUpdateCount{0};
    uint64_t m_droppedWantedCount{0};

    StringCache();
};
