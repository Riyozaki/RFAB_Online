#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace CoopPacketSafety
{
    /**
     * Reentrancy-safe RAII frame guard for thread_local ScratchAllocator instances.
     * Resets the linear scratch buffer ONLY when the outermost Send() frame on the current
     * thread exits (`depth == 0`), preventing nested logging/serialization callbacks from
     * invalidating an in-flight outer packet buffer.
     */
    template <class TScratchAllocator>
    class ScopedScratchFrame
    {
    public:
        ScopedScratchFrame(TScratchAllocator& aAllocator, uint32_t& aDepthCounter) noexcept
            : m_allocator(aAllocator)
            , m_depth(aDepthCounter)
        {
            ++m_depth;
        }

        ~ScopedScratchFrame() noexcept
        {
            if (m_depth > 0 && --m_depth == 0)
                m_allocator.Reset();
        }

        ScopedScratchFrame(const ScopedScratchFrame&) = delete;
        ScopedScratchFrame& operator=(const ScopedScratchFrame&) = delete;

    private:
        TScratchAllocator& m_allocator;
        uint32_t& m_depth;
    };

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
    inline constexpr float kWorldCoordinateMaxAbs = 2097152.0f;
    inline constexpr uint32_t kUnmappedServerModId = 0xFFFFFFFEu;
    inline constexpr uint32_t kRuntimeDynamicModId = 0xFFFFFFFFu;

    [[nodiscard]] inline constexpr size_t ClampCollectionCount(uint64_t aRawCount, size_t aMaxAllowed) noexcept
    {
        return aRawCount > static_cast<uint64_t>(aMaxAllowed)
            ? aMaxAllowed
            : static_cast<size_t>(aRawCount);
    }

    [[nodiscard]] inline float SanitizeFiniteFloat(float aValue, float aMin, float aMax, float aFallback = 0.0f) noexcept
    {
        if (!std::isfinite(aValue))
            return aFallback;
        return std::clamp(aValue, aMin, aMax);
    }

    [[nodiscard]] inline bool AreWorldCoordinatesValid(float aX, float aY, float aZ) noexcept
    {
        return std::isfinite(aX) && std::isfinite(aY) && std::isfinite(aZ) &&
               std::abs(aX) <= kWorldCoordinateMaxAbs &&
               std::abs(aY) <= kWorldCoordinateMaxAbs &&
               std::abs(aZ) <= kWorldCoordinateMaxAbs;
    }

    template <class TVec3>
    [[nodiscard]] inline bool IsFiniteVec3(const TVec3& aVec) noexcept
    {
        return AreWorldCoordinatesValid(aVec.x, aVec.y, aVec.z);
    }

    /**
     * Sanitizes Skyrim calendar / time-of-day components so non-finite `NaN`/`Inf` or out-of-range
     * wire values can never poison `Sky::Update` shadow matrices (`GameHour`) or server time ticks.
     */
    inline void SanitizeSkyrimCalendarValues(
        float& aTimeScale,
        float& aGameHour,
        uint32_t& aDay,
        uint32_t& aMonth,
        uint32_t& aYear) noexcept
    {
        aTimeScale = SanitizeFiniteFloat(aTimeScale, 0.0f, 1000.0f, 20.0f);
        if (!std::isfinite(aGameHour))
        {
            aGameHour = 8.0f;
        }
        else
        {
            aGameHour = std::fmod(aGameHour, 24.0f);
            if (aGameHour < 0.0f)
                aGameHour += 24.0f;
        }

        if (aMonth > 11u)
            aMonth = 7u;
        if (aDay > 31u)
            aDay = 17u;
        if (aYear > 9999u)
            aYear = 201u;
    }

    /**
     * Translates a non-ESL (`hiByte != 0xFE`) Skyrim SE 32-bit FormID through `m_standardToServer[0x100]`:
     * - `aGameId == 0` -> null FormID `(0, 0)`, returns `true`.
     * - Unmapped standard slot (`kUnmappedServerModId`) -> `(0, 0)`, returns `false` so local-only `.esp`
     *   forms NEVER alias onto `Skyrim.esm` (`ServerModId == 0`) and cause type-confusion vtable crashes!
     * - Mapped standard slot (`0..0xFD`) or `0xFF` (`kRuntimeDynamicModId`) -> `(mapped, aGameId & 0x00FFFFFFu)`.
     */
    [[nodiscard]] inline constexpr bool TranslateStandardFormToServerModId(
        uint32_t aGameId,
        const uint32_t (&aStandardToServer)[0x100],
        uint32_t& aOutModId,
        uint32_t& aOutBaseId) noexcept
    {
        if (aGameId == 0u)
        {
            aOutModId = 0u;
            aOutBaseId = 0u;
            return true;
        }

        const uint32_t hiByte = (aGameId >> 24) & 0xFFu;
        const uint32_t mapped = aStandardToServer[hiByte];
        if (mapped == kUnmappedServerModId)
        {
            aOutModId = 0u;
            aOutBaseId = 0u;
            return false;
        }

        aOutModId = mapped;
        aOutBaseId = aGameId & 0x00FFFFFFu;
        return true;
    }

    [[nodiscard]] inline int32_t SanitizeItemCountDelta(int32_t aCount, int32_t aMaxAbs = 1'000'000) noexcept
    {
        if (aCount > aMaxAbs)
            return aMaxAbs;
        if (aCount < -aMaxAbs)
            return -aMaxAbs;
        return aCount;
    }

    struct LoadOrderAuditResult
    {
        bool Compatible{true};
        bool ServerHasRfab{false};
        bool ClientHasRfab{false};
        std::vector<std::string> MissingCriticalMods{};
        size_t OrderMismatchCount{0};
        std::string SummaryRu{};
    };

    namespace Detail
    {
        [[nodiscard]] inline std::string ToLowerModName(std::string_view aName)
        {
            std::string out;
            out.reserve(aName.size());
            for (unsigned char ch : aName)
                out.push_back(static_cast<char>(std::tolower(ch)));
            return out;
        }

        [[nodiscard]] inline bool IsCriticalSharedMod(std::string_view aLowerModName) noexcept
        {
            return aLowerModName == "skyrim.esm" ||
                   aLowerModName == "update.esm" ||
                   aLowerModName == "dawnguard.esm" ||
                   aLowerModName == "hearthfires.esm" ||
                   aLowerModName == "dragonborn.esm" ||
                   aLowerModName == "requiem for the indifferent.esp" ||
                   aLowerModName == "rfab.esp" ||
                   aLowerModName == "skyrimtogether.esp";
        }
    } // namespace Detail

    /**
     * SkyMP-inspired Load Order & RFAB Modpack Handshake Verification:
     * Checks that critical ESM/ESP masters present on the server also exist on the client in the same
     * relative order, catching missing `RFAB.esp` / `SkyrimTogether.esp` or major load-order drift
     * immediately on join with an actionable Russian summary.
     */
    template <class TServerModRange, class TClientModRange>
    [[nodiscard]] inline LoadOrderAuditResult AuditModListCompatibility(
        const TServerModRange& acServerMods,
        const TClientModRange& acClientMods)
    {
        LoadOrderAuditResult res{};

        std::vector<std::string> serverNorm;
        for (const auto& raw : acServerMods)
        {
            const std::string lower = Detail::ToLowerModName(std::string_view(raw.data(), raw.size()));
            if (lower.empty())
                continue;
            if (lower == "rfab.esp")
                res.ServerHasRfab = true;
            serverNorm.push_back(lower);
        }

        std::vector<std::string> clientNorm;
        for (const auto& raw : acClientMods)
        {
            const std::string lower = Detail::ToLowerModName(std::string_view(raw.data(), raw.size()));
            if (lower.empty())
                continue;
            if (lower == "rfab.esp")
                res.ClientHasRfab = true;
            clientNorm.push_back(lower);
        }

        if (serverNorm.empty() || clientNorm.empty())
            return res;

        std::vector<size_t> matchedClientIndices;
        for (const auto& sMod : serverNorm)
        {
            auto it = std::find(clientNorm.begin(), clientNorm.end(), sMod);
            if (it == clientNorm.end())
            {
                if (Detail::IsCriticalSharedMod(sMod))
                    res.MissingCriticalMods.push_back(sMod);
                continue;
            }

            if (Detail::IsCriticalSharedMod(sMod))
                matchedClientIndices.push_back(static_cast<size_t>(std::distance(clientNorm.begin(), it)));
        }

        for (size_t i = 1; i < matchedClientIndices.size(); ++i)
        {
            if (matchedClientIndices[i] < matchedClientIndices[i - 1])
                ++res.OrderMismatchCount;
        }

        if (!res.MissingCriticalMods.empty() || (res.ServerHasRfab != res.ClientHasRfab) || res.OrderMismatchCount > 0)
        {
            res.Compatible = false;
            if (res.ServerHasRfab && !res.ClientHasRfab)
            {
                res.SummaryRu = "RFAB Online: Внимание — на сервере включён RFAB.esp, а у клиента он не найден!";
            }
            else if (!res.MissingCriticalMods.empty())
            {
                res.SummaryRu = "RFAB Online: Несовпадение критических плагинов с сервером (" + res.MissingCriticalMods.front() + ")";
            }
            else
            {
                res.SummaryRu = "RFAB Online: Порядок загрузки базовых плагинов отличается от сервера — возможен рассинхрон ID";
            }
        }

        return res;
    }
} // namespace CoopPacketSafety
