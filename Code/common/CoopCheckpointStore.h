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

    [[nodiscard]] inline uint32_t ComputeCrc32c(const uint8_t* apData, size_t aSize) noexcept
    {
        uint32_t crc = 0xFFFFFFFFu;
        if (!apData)
            return ~crc;

        for (size_t i = 0; i < aSize; ++i)
        {
            crc ^= static_cast<uint32_t>(apData[i]);
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
        }
        return ~crc;
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

    // ============================================================================
    // Versioned TLV (Tag-Length-Value) Extension Container for rfab_checkpoints.bin (v2.1.0)
    // ============================================================================
    // Replaces fragile chained magic trailers with a self-describing TLV container:
    // - Container Header: [Magic = 'RTLV' (0x564C5452)][ContainerVersion = 1][ChunkCount]
    // - Each Chunk:       [Tag (u32)][ChunkVersion (u32)][PayloadBytes (u32)][Payload...]
    // Unknown future tags are safely skipped in O(1) via `PayloadBytes`, guaranteeing both
    // forward and backward compatibility across minor/major releases.
    static constexpr uint32_t kTlvContainerMagic = 0x564C5452u; // 'RTLV' little-endian
    static constexpr uint32_t kTlvContainerVersion = 1u;

    enum class TlvTag : uint32_t
    {
        kNpcSpatial = 0x0001u,
        kPlayerIdentities = 0x0002u,
        kContainerLootLedger = 0x0003u,
        kPersistentPlacedActors = 0x0004u,
        kV210PersistentActors = 0x0004u,
    };

    struct TlvChunkHeader
    {
        uint32_t Tag{0};
        uint32_t Version{1};
        uint32_t PayloadBytes{0};
        uint32_t Crc32c{0};
    };
    static_assert(sizeof(TlvChunkHeader) == 16, "TlvChunkHeader must be 16 bytes");

    struct TlvChunkView
    {
        uint32_t Tag{0};
        uint32_t Version{1};
        const uint8_t* Data{nullptr};
        size_t Size{0};
        size_t ByteLength{0};
    };

    class TlvContainerBuilder
    {
    public:
        void AddChunk(TlvTag aTag, uint32_t aVersion, const std::vector<uint8_t>& acPayload)
        {
            AddChunkRaw(static_cast<uint32_t>(aTag), aVersion, acPayload.data(), acPayload.size());
        }

        void AppendChunk(TlvTag aTag, const uint8_t* apPayload, size_t aSize, uint32_t aVersion = 1u)
        {
            AddChunkRaw(static_cast<uint32_t>(aTag), aVersion, apPayload, aSize);
        }

        void AddChunkRaw(uint32_t aTag, uint32_t aVersion, const uint8_t* apPayload, size_t aSize)
        {
            if (aSize > (32u * 1024u * 1024u))
                return;

            ChunkEntry entry{};
            entry.Header.Tag = aTag;
            entry.Header.Version = aVersion;
            entry.Header.PayloadBytes = static_cast<uint32_t>(aSize);
            entry.Header.Crc32c = (aSize > 0 && apPayload) ? ComputeCrc32c(apPayload, aSize) : 0u;
            if (aSize > 0 && apPayload)
                entry.Payload.assign(apPayload, apPayload + aSize);
            m_chunks.push_back(std::move(entry));
        }

        void AppendToBuffer(std::vector<uint8_t>& aOut) const
        {
            const uint32_t magic = kTlvContainerMagic;
            const uint32_t version = kTlvContainerVersion;
            const uint32_t count = static_cast<uint32_t>(m_chunks.size());

            auto appendPod = [&](const auto& val)
            {
                const auto* p = reinterpret_cast<const uint8_t*>(&val);
                aOut.insert(aOut.end(), p, p + sizeof(val));
            };

            appendPod(magic);
            appendPod(version);
            appendPod(count);

            for (const auto& c : m_chunks)
            {
                appendPod(c.Header);
                if (!c.Payload.empty())
                    aOut.insert(aOut.end(), c.Payload.begin(), c.Payload.end());
            }
        }

        [[nodiscard]] std::vector<uint8_t> Bytes() const
        {
            std::vector<uint8_t> out;
            AppendToBuffer(out);
            return out;
        }

    private:
        struct ChunkEntry
        {
            TlvChunkHeader Header{};
            std::vector<uint8_t> Payload{};
        };
        std::vector<ChunkEntry> m_chunks{};
    };

    [[nodiscard]] inline bool ParseTlvContainer(
        const uint8_t* apData,
        size_t aTotalSize,
        size_t& aInOutOffset,
        std::vector<TlvChunkView>& aOutChunks) noexcept
    {
        aOutChunks.clear();
        if (!apData || aInOutOffset + 12 > aTotalSize)
            return false;

        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t count = 0;
        std::memcpy(&magic, apData + aInOutOffset, 4);
        if (magic != kTlvContainerMagic)
            return false;

        std::memcpy(&version, apData + aInOutOffset + 4, 4);
        std::memcpy(&count, apData + aInOutOffset + 8, 4);
        if (version == 0 || version > kTlvContainerVersion || count > 256u)
            return false;

        size_t cursor = aInOutOffset + 12;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (cursor + sizeof(TlvChunkHeader) > aTotalSize)
                return false;

            TlvChunkHeader hdr{};
            std::memcpy(&hdr, apData + cursor, sizeof(TlvChunkHeader));
            cursor += sizeof(TlvChunkHeader);

            if (hdr.PayloadBytes > (32u * 1024u * 1024u) || cursor + hdr.PayloadBytes > aTotalSize)
            {
                aOutChunks.clear();
                return false;
            }

            const uint32_t actualCrc = (hdr.PayloadBytes > 0) ? ComputeCrc32c(apData + cursor, hdr.PayloadBytes) : 0u;
            if (actualCrc != hdr.Crc32c)
            {
                aOutChunks.clear();
                return false;
            }

            aOutChunks.push_back(TlvChunkView{
                hdr.Tag,
                hdr.Version,
                hdr.PayloadBytes > 0 ? (apData + cursor) : nullptr,
                static_cast<size_t>(hdr.PayloadBytes),
                static_cast<size_t>(hdr.PayloadBytes)});

            cursor += hdr.PayloadBytes;
        }

        aInOutOffset = cursor;
        return true;
    }

    [[nodiscard]] inline bool ParseTlvContainer(
        const uint8_t* apData,
        size_t aTotalSize,
        std::vector<TlvChunkView>& aOutChunks) noexcept
    {
        size_t offset = 0;
        return ParseTlvContainer(apData, aTotalSize, offset, aOutChunks);
    }
} // namespace CoopCheckpoint
