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
