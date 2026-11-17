#pragma once

#include <cstddef>
#include <cstdint>

namespace CoopPacketSafety
{
    inline constexpr size_t kMaxInventoryEntries = 4096;
    inline constexpr size_t kMaxEnchantEffects = 64;
    inline constexpr size_t kMaxActorValues = 256;
    inline constexpr size_t kMaxActionEvents = 64;
    inline constexpr size_t kMaxMoveUpdates = 512;
    inline constexpr size_t kMaxAnimVars = 1024;
    inline constexpr size_t kMaxMods = 4096;
    inline constexpr size_t kMaxAssignObjects = 2048;
    inline constexpr size_t kMaxQuests = 2048;
    inline constexpr size_t kMaxTints = 256;
    inline constexpr size_t kMaxFactions = 512;
    inline constexpr size_t kMaxGridCells = 256;
    inline constexpr size_t kMaxPlayerListEntries = 256;
    inline constexpr size_t kMaxObjectInventoryChanges = 255;
    inline constexpr size_t kMaxFactionChanges = 0x3FF;
    inline constexpr size_t kMaxStringCacheBatch = 0xFFFF;

    [[nodiscard]] inline constexpr size_t ClampCollectionCount(uint64_t aRawCount, size_t aMaxAllowed) noexcept
    {
        return aRawCount > static_cast<uint64_t>(aMaxAllowed)
            ? aMaxAllowed
            : static_cast<size_t>(aRawCount);
    }
} // namespace CoopPacketSafety
