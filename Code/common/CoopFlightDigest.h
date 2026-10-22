#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace CoopFlight
{
// Order-independent 256-bucket FNV-1a world state digest & low-overhead in-memory flight recorder.
// Inspired by cranebeaker/coop-anything tools/flight (MIT License):
// - Identical logical world states produce the exact same 64-bit digest regardless of insertion/hash-map order.
// - When two states diverge, Diff() immediately pinpoints the exact diverging bucket and key.

enum class StateCategory : uint8_t
{
    kQuestStage = 1,
    kSharedXp = 2,
    kReadBook = 3,
    kSyncedGlobal = 4,
    kActorOwnership = 5,
    kSaveCheckpoint = 6
};

enum class FlightEventKind : uint8_t
{
    kSavePrepared = 1,
    kSaveCommitted = 2,
    kSaveAborted = 3,
    kWorldRollback = 4,
    kPartyWipe = 5,
    kCombatAutoRevive = 6,
    kInteractiveRevive = 7,
    kOwnershipChanged = 8,
    kOwnershipRejected = 9,
    kAmbushDeduplicated = 10,
    kSharedXpAwarded = 11,
    kStaleEpochRejected = 12
};

inline constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
inline constexpr uint64_t kFnvPrime = 1099511628211ULL;

inline constexpr uint64_t HashBytes(const void* apData, size_t aLen, uint64_t aSeed = kFnvOffsetBasis) noexcept
{
    const auto* pBytes = static_cast<const uint8_t*>(apData);
    uint64_t hash = aSeed;
    for (size_t i = 0; i < aLen; ++i)
    {
        hash ^= static_cast<uint64_t>(pBytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

inline constexpr uint64_t HashString(std::string_view aStr, uint64_t aSeed = kFnvOffsetBasis) noexcept
{
    uint64_t hash = aSeed;
    for (unsigned char ch : aStr)
    {
        hash ^= static_cast<uint64_t>(ch);
        hash *= kFnvPrime;
    }
    return hash;
}

inline constexpr uint64_t HashU64(uint64_t aValue, uint64_t aSeed = kFnvOffsetBasis) noexcept
{
    uint64_t hash = aSeed;
    for (int i = 0; i < 8; ++i)
    {
        hash ^= (aValue >> (i * 8)) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

struct DivergenceReport
{
    size_t BucketIndex{0};
    std::string Key{};
    uint64_t LocalValueHash{0};
    uint64_t RemoteValueHash{0};
    bool MissingOnLocal{false};
    bool MissingOnRemote{false};
};

class BucketedStateDigest
{
public:
    static constexpr size_t kBucketCount = 256;

    void Clear() noexcept
    {
        m_entries.clear();
        m_bucketXor.fill(0);
    }

    void Put(StateCategory aCategory, std::string_view aKey, uint64_t aValueHash)
    {
        std::string compositeKey;
        compositeKey.reserve(aKey.size() + 4);
        compositeKey.push_back(static_cast<char>('0' + static_cast<uint8_t>(aCategory)));
        compositeKey.push_back(':');
        compositeKey.append(aKey);

        const uint64_t keyHash = HashString(compositeKey);
        const size_t bucket = static_cast<size_t>(keyHash & 0xFFULL);
        const uint64_t pairHash = HashU64(aValueHash, keyHash);

        auto it = m_entries.find(compositeKey);
        if (it != m_entries.end())
        {
            // Remove old contribution from bucket via self-inverse XOR before applying new value hash
            m_bucketXor[bucket] ^= it->second.PairHash;
            it->second.ValueHash = aValueHash;
            it->second.PairHash = pairHash;
        }
        else
        {
            m_entries.emplace(std::move(compositeKey), Entry{aValueHash, pairHash, bucket});
        }

        m_bucketXor[bucket] ^= pairHash;
    }

    void Erase(StateCategory aCategory, std::string_view aKey) noexcept
    {
        std::string compositeKey;
        compositeKey.reserve(aKey.size() + 4);
        compositeKey.push_back(static_cast<char>('0' + static_cast<uint8_t>(aCategory)));
        compositeKey.push_back(':');
        compositeKey.append(aKey);

        auto it = m_entries.find(compositeKey);
        if (it == m_entries.end())
            return;

        m_bucketXor[it->second.Bucket] ^= it->second.PairHash;
        m_entries.erase(it);
    }

    [[nodiscard]] uint64_t ComputeRootDigest() const noexcept
    {
        uint64_t root = kFnvOffsetBasis;
        for (size_t i = 0; i < kBucketCount; ++i)
            root = HashU64(m_bucketXor[i], root);
        return root;
    }

    [[nodiscard]] std::string ComputeHexDigest() const
    {
        char buf[17]{};
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(ComputeRootDigest()));
        return std::string(buf);
    }

    [[nodiscard]] size_t Size() const noexcept { return m_entries.size(); }

    [[nodiscard]] std::optional<DivergenceReport> FirstDivergence(const BucketedStateDigest& acOther) const
    {
        if (ComputeRootDigest() == acOther.ComputeRootDigest())
            return std::nullopt;

        for (size_t b = 0; b < kBucketCount; ++b)
        {
            if (m_bucketXor[b] == acOther.m_bucketXor[b])
                continue;

            for (const auto& [k, v] : m_entries)
            {
                if (v.Bucket != b)
                    continue;
                const auto rhsIt = acOther.m_entries.find(k);
                if (rhsIt == acOther.m_entries.end())
                    return DivergenceReport{b, k, v.ValueHash, 0, false, true};
                if (rhsIt->second.ValueHash != v.ValueHash)
                    return DivergenceReport{b, k, v.ValueHash, rhsIt->second.ValueHash, false, false};
            }

            for (const auto& [k, v] : acOther.m_entries)
            {
                if (v.Bucket != b)
                    continue;
                if (!m_entries.contains(k))
                    return DivergenceReport{b, k, 0, v.ValueHash, true, false};
            }
        }

        return std::nullopt;
    }

private:
    struct Entry
    {
        uint64_t ValueHash{0};
        uint64_t PairHash{0};
        size_t Bucket{0};
    };

    std::array<uint64_t, kBucketCount> m_bucketXor{};
    std::unordered_map<std::string, Entry> m_entries{};
};

struct FlightRecord
{
    uint64_t Sequence{0};
    uint64_t TimestampMs{0};
    uint64_t WorldEpoch{0};
    FlightEventKind Kind{FlightEventKind::kSaveCommitted};
    uint32_t SubjectId{0};
    uint64_t AuxValue{0};
};

template <size_t Capacity = 512>
class FlightRingBuffer
{
public:
    static_assert(Capacity >= 8, "FlightRingBuffer capacity must be >= 8");

    void Record(uint64_t aNowMs, uint64_t aWorldEpoch, FlightEventKind aKind, uint32_t aSubjectId = 0, uint64_t aAuxValue = 0) noexcept
    {
        const uint64_t seq = ++m_totalRecorded;
        m_ring[m_writeIndex] = FlightRecord{seq, aNowMs, aWorldEpoch, aKind, aSubjectId, aAuxValue};
        m_writeIndex = (m_writeIndex + 1) % Capacity;
        if (m_count < Capacity)
            ++m_count;
    }

    [[nodiscard]] std::vector<FlightRecord> SnapshotRecent(size_t aMaxRecords = Capacity) const
    {
        const size_t n = (aMaxRecords < m_count) ? aMaxRecords : m_count;
        std::vector<FlightRecord> out;
        out.reserve(n);

        const size_t oldestOffset = (m_writeIndex + Capacity - n) % Capacity;
        for (size_t i = 0; i < n; ++i)
            out.push_back(m_ring[(oldestOffset + i) % Capacity]);

        return out;
    }

    [[nodiscard]] size_t Size() const noexcept { return m_count; }
    [[nodiscard]] uint64_t TotalRecorded() const noexcept { return m_totalRecorded; }

    void Clear() noexcept
    {
        m_writeIndex = 0;
        m_count = 0;
        m_totalRecorded = 0;
    }

private:
    std::array<FlightRecord, Capacity> m_ring{};
    size_t m_writeIndex{0};
    size_t m_count{0};
    uint64_t m_totalRecorded{0};
};
} // namespace CoopFlight
