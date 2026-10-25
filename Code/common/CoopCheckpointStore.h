#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace CoopCheckpoint
{
    inline constexpr uint64_t kCheckpointFileMagic = 0x32504B4342414652ULL; // "RFABCKP2" little-endian
    inline constexpr uint32_t kCheckpointSchemaVersion = 1u;

    struct EnvelopeHeader
    {
        uint64_t Magic{kCheckpointFileMagic};
        uint32_t SchemaVersion{kCheckpointSchemaVersion};
        uint32_t PayloadBytes{0};
        uint64_t PayloadFnv1a64{0};
    };
    static_assert(sizeof(EnvelopeHeader) == 24);

    [[nodiscard]] inline uint64_t ComputeFnv1a64(const uint8_t* apData, size_t aSize) noexcept
    {
        uint64_t hash = 14695981039346656037ULL;
        if (!apData)
            return hash;

        for (size_t i = 0; i < aSize; ++i)
        {
            hash ^= static_cast<uint64_t>(apData[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    /**
     * Atomically writes a binary checkpoint payload wrapped in a checksummed RFABCKP2 envelope
     * via `<path>.tmp` -> `<path>` rename so partial writes or server crashes never corrupt checkpoints.
     */
    [[nodiscard]] inline bool WriteAtomicCheckpointFile(
        const std::filesystem::path& acTargetPath,
        const uint8_t* apPayload,
        size_t aPayloadSize) noexcept
    {
        if (acTargetPath.empty() || (!apPayload && aPayloadSize > 0) || aPayloadSize > (64u * 1024u * 1024u))
            return false;

        try
        {
            std::error_code ec;
            const auto parentDir = acTargetPath.parent_path();
            if (!parentDir.empty())
                std::filesystem::create_directories(parentDir, ec);

            std::filesystem::path tmpPath = acTargetPath;
            tmpPath += ".tmp";
            {
                std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
                if (!out.is_open())
                    return false;

                EnvelopeHeader hdr{};
                hdr.Magic = kCheckpointFileMagic;
                hdr.SchemaVersion = kCheckpointSchemaVersion;
                hdr.PayloadBytes = static_cast<uint32_t>(aPayloadSize);
                hdr.PayloadFnv1a64 = ComputeFnv1a64(apPayload, aPayloadSize);

                out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
                if (aPayloadSize > 0)
                    out.write(reinterpret_cast<const char*>(apPayload), static_cast<std::streamsize>(aPayloadSize));
                out.flush();
                if (!out.good())
                    return false;
            }

            std::filesystem::rename(tmpPath, acTargetPath, ec);
            if (ec)
            {
                std::error_code rmEc;
                std::filesystem::remove(acTargetPath, rmEc);
                ec.clear();
                std::filesystem::rename(tmpPath, acTargetPath, ec);
                if (ec)
                    return false;
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    /**
     * Reads and validates a checksummed RFABCKP2 checkpoint file from disk.
     * Returns false on missing file, truncated header/payload, wrong schema, or bit-flip corruption.
     */
    [[nodiscard]] inline bool ReadValidatedCheckpointFile(
        const std::filesystem::path& acSourcePath,
        std::vector<uint8_t>& aOutPayload) noexcept
    {
        aOutPayload.clear();
        if (acSourcePath.empty())
            return false;

        try
        {
            std::error_code ec;
            if (!std::filesystem::exists(acSourcePath, ec) || ec)
                return false;

            const auto totalSize = std::filesystem::file_size(acSourcePath, ec);
            if (ec || totalSize < sizeof(EnvelopeHeader) || totalSize > (64u * 1024u * 1024u + sizeof(EnvelopeHeader)))
                return false;

            std::ifstream in(acSourcePath, std::ios::binary);
            if (!in.is_open())
                return false;

            EnvelopeHeader hdr{};
            in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
            if (!in.good())
                return false;

            if (hdr.Magic != kCheckpointFileMagic || hdr.SchemaVersion != kCheckpointSchemaVersion)
                return false;

            if (static_cast<uint64_t>(hdr.PayloadBytes) + sizeof(EnvelopeHeader) != totalSize)
                return false;

            aOutPayload.resize(hdr.PayloadBytes);
            if (hdr.PayloadBytes > 0)
            {
                in.read(reinterpret_cast<char*>(aOutPayload.data()), static_cast<std::streamsize>(hdr.PayloadBytes));
                if (!in.good())
                {
                    aOutPayload.clear();
                    return false;
                }
            }

            if (ComputeFnv1a64(aOutPayload.data(), aOutPayload.size()) != hdr.PayloadFnv1a64)
            {
                aOutPayload.clear();
                return false;
            }

            return true;
        }
        catch (...)
        {
            aOutPayload.clear();
            return false;
        }
    }

    /**
     * MMO / SkyMP Rollback Policy:
     * - Party wipes ALWAYS execute seamless in-session rollback whenever the server holds a valid
     *   player checkpoint or session-entry baseline (zero .ess reload, zero m_transport.Close()!).
     * - Empty slot names (pre-checkpoint rollbacks) also execute seamless in-session rollback.
     * - Explicit manual slot loads (`!aIsPartyWipe && !acSlotName.empty()`) execute coordinated .ess
     *   load on clients that possess the current-session .ess slot, with automatic in-session fallback
     *   for any peer lacking the file on disk.
     */
    [[nodiscard]] inline constexpr bool ShouldServerPreferInSessionRollback(
        bool aIsPartyWipe,
        bool aHasInSessionPlayerState,
        std::string_view acSlotName) noexcept
    {
        if (aIsPartyWipe && aHasInSessionPlayerState)
            return true;
        if (acSlotName.empty() && aHasInSessionPlayerState)
            return true;
        return false;
    }

    /**
     * Prevents loading stale `RFAB_MP_PreJoin_Backup.ess` files left on disk from an older solo/co-op
     * session ("грузило сохранения вне сервера"):
     * - A PreJoin backup on disk is ONLY eligible if it was freshly written during THIS session AND
     *   no in-session server transform rollback is available.
     */
    [[nodiscard]] inline constexpr bool ShouldAllowPreJoinDiskFallback(
        bool aAllowPreJoinFallback,
        bool aPreJoinWrittenInCurrentSession,
        bool aHasInSessionRestoreTransform) noexcept
    {
        if (!aAllowPreJoinFallback)
            return false;
        if (aHasInSessionRestoreTransform)
            return false;
        return aPreJoinWrittenInCurrentSession;
    }
} // namespace CoopCheckpoint
