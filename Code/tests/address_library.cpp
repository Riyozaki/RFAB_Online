// Unit tests for the portable address library layer and the AE -> 1.5.97 id
// translation. See tools/port1597/selftest.cpp for a dependency free runner of
// the same code (used on machines without xmake).

#include <catch2/catch.hpp>

#include "address_library/AddressLibrary.h"
#include "address_library/SkyrimIdMap.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{
struct Record
{
    std::uint64_t id;
    std::uint64_t offset;
};

void Append(std::vector<std::uint8_t>& aBuffer, const void* apData, std::size_t aSize)
{
    const auto* pBytes = static_cast<const std::uint8_t*>(apData);
    aBuffer.insert(aBuffer.end(), pBytes, pBytes + aSize);
}

template <class T> void Append(std::vector<std::uint8_t>& aBuffer, T aValue)
{
    Append(aBuffer, &aValue, sizeof(T));
}

std::vector<std::uint8_t> BuildSparseDatabase(std::int32_t aFormat, const std::vector<Record>& acRecords,
                                              std::int32_t aPointerSize = 8, bool aUseScaleBit = true)
{
    std::vector<std::uint8_t> buffer;

    Append<std::int32_t>(buffer, aFormat);
    for (std::int32_t value : {1, 6, 1170, 0})
        Append<std::int32_t>(buffer, value);

    const std::string name = "SkyrimSE.exe";
    Append<std::int32_t>(buffer, static_cast<std::int32_t>(name.size()));
    Append(buffer, name.data(), name.size());
    Append<std::int32_t>(buffer, aPointerSize);
    Append<std::int32_t>(buffer, static_cast<std::int32_t>(acRecords.size()));

    std::uint64_t previousId = 0;
    std::uint64_t previousOffset = 0;

    for (const auto& record : acRecords)
    {
        std::uint8_t type = record.id == previousId + 1 ? 0x1 : 0x7;
        std::uint8_t offsetKind = 0;
        std::uint64_t absoluteOffset = 0;

        if (record.offset == previousOffset + 1)
        {
            offsetKind = 0x1;
        }
        else if (aUseScaleBit && record.offset % aPointerSize == 0 &&
                 record.offset / aPointerSize == previousOffset / aPointerSize + 1)
        {
            offsetKind = 0x9; // pointer scaled, previous + 1
        }
        else
        {
            offsetKind = 0x7;
            absoluteOffset = record.offset;
        }

        Append<std::uint8_t>(buffer, static_cast<std::uint8_t>(type | (offsetKind << 4)));

        if (type == 0x7)
            Append<std::uint32_t>(buffer, static_cast<std::uint32_t>(record.id));
        if (offsetKind == 0x7)
            Append<std::uint32_t>(buffer, static_cast<std::uint32_t>(absoluteOffset));

        previousId = record.id;
        previousOffset = record.offset;
    }

    return buffer;
}

std::vector<std::uint8_t> BuildDenseDatabase(std::size_t aCount, const std::vector<std::pair<std::size_t, std::uint32_t>>& aEntries)
{
    std::vector<std::uint8_t> buffer;

    Append<std::uint32_t>(buffer, 5);
    for (std::uint32_t value : {1u, 7u, 99u, 0u})
        Append<std::uint32_t>(buffer, value);

    char name[64]{};
    std::memcpy(name, "SkyrimSE.exe", 12);
    Append(buffer, name, sizeof(name));

    Append<std::int32_t>(buffer, 8);
    Append<std::int32_t>(buffer, 0);
    Append<std::int32_t>(buffer, static_cast<std::int32_t>(aCount));

    std::vector<std::uint32_t> offsets(aCount, 0);
    for (const auto& [index, value] : aEntries)
        offsets[index] = value;

    for (auto value : offsets)
        Append<std::uint32_t>(buffer, value);

    return buffer;
}
} // namespace

TEST_CASE("Address library version parsing", "[addresslib]")
{
    const auto version = AddressLibrary::Version::FromString("1.5.97.0");
    REQUIRE(version.has_value());
    CHECK(version->major == 1);
    CHECK(version->minor == 5);
    CHECK(version->revision == 97);
    CHECK(version->build == 0);
    CHECK(version->ToString() == "1.5.97.0");
    CHECK(version->ToFileNameSuffix() == "1-5-97-0");

    CHECK(AddressLibrary::Version::FromString("1.6.1170").has_value());
    CHECK_FALSE(AddressLibrary::Version::FromString("").has_value());
    CHECK_FALSE(AddressLibrary::Version::FromString("1").has_value());
    CHECK_FALSE(AddressLibrary::Version::FromString("1.6.1170.0.1").has_value());
    CHECK_FALSE(AddressLibrary::Version::FromString("game").has_value());
}

TEST_CASE("Address library probes both file naming schemes", "[addresslib]")
{
    const auto names = AddressLibrary::GetCandidateFileNames(AddressLibrary::Version{1, 5, 97, 0});
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "versionlib-1-5-97-0.bin");
    CHECK(names[1] == "version-1-5-97-0.bin");
}

TEST_CASE("Sparse database formats 1 and 2 decode identically", "[addresslib]")
{
    const std::vector<Record> records{{1, 0x1000}, {2, 0x1008}, {5, 0x2000}, {9, 0x2008}, {10, 0x2009}};

    for (const auto format : {1, 2})
    {
        AddressLibrary::Database database;
        const auto outcome = database.LoadFromMemory(BuildSparseDatabase(format, records));
        INFO(outcome.error);
        REQUIRE(outcome.success);

        CHECK(static_cast<int>(database.GetFormat()) == format);
        CHECK(database.GetVersion() == (AddressLibrary::Version{1, 6, 1170, 0}));
        CHECK(database.GetModuleName() == "SkyrimSE.exe");
        CHECK(database.GetEntryCount() == records.size());

        for (const auto& record : records)
        {
            const auto offset = database.FindOffsetById(record.id);
            REQUIRE(offset.has_value());
            CHECK(*offset == record.offset);
        }

        CHECK_FALSE(database.FindOffsetById(3).has_value());
        CHECK(database.CountOffsetsAbove(0x3000) == 0);
    }
}

TEST_CASE("Pointer scaled offsets are handled", "[addresslib]")
{
    AddressLibrary::Database scaled;
    AddressLibrary::Database unscaled;
    const std::vector<Record> records{{1, 0x1000}, {2, 0x1008}, {3, 0x100C}};

    REQUIRE(scaled.LoadFromMemory(BuildSparseDatabase(2, records, 8, true)).success);
    REQUIRE(unscaled.LoadFromMemory(BuildSparseDatabase(2, records, 8, false)).success);

    for (const auto& record : records)
    {
        REQUIRE(scaled.FindOffsetById(record.id).has_value());
        REQUIRE(unscaled.FindOffsetById(record.id).has_value());
        CHECK(*scaled.FindOffsetById(record.id) == *unscaled.FindOffsetById(record.id));
    }
}

TEST_CASE("Dense database format 5", "[addresslib]")
{
    AddressLibrary::Database database;
    const auto outcome = database.LoadFromMemory(BuildDenseDatabase(10, {{3, 0x1234}, {7, 0x5678}}));
    INFO(outcome.error);
    REQUIRE(outcome.success);

    CHECK(static_cast<int>(database.GetFormat()) == 5);
    CHECK(database.GetEntryCount() == 2);
    CHECK(database.FindOffsetById(3).value_or(0) == 0x1234);
    CHECK_FALSE(database.FindOffsetById(4).has_value());
    CHECK_FALSE(database.FindOffsetById(100).has_value());
}

TEST_CASE("Broken databases are rejected with a reason", "[addresslib]")
{
    AddressLibrary::Database database;

    SECTION("unsupported format")
    {
        const auto outcome = database.LoadFromMemory(BuildSparseDatabase(3, {{1, 0x1000}}));
        CHECK_FALSE(outcome.success);
        CHECK(outcome.error.find('3') != std::string::npos);
    }

    SECTION("truncated file")
    {
        auto bytes = BuildSparseDatabase(2, {{1, 0x1000}, {2, 0x1008}, {3, 0x1010}});
        bytes.resize(bytes.size() - 6);
        CHECK_FALSE(database.LoadFromMemory(bytes).success);
    }

    SECTION("empty file")
    {
        CHECK_FALSE(database.LoadFromMemory({}).success);
    }

    SECTION("no entries")
    {
        CHECK_FALSE(database.LoadFromMemory(BuildSparseDatabase(2, {})).success);
    }
}

TEST_CASE("AE to 1.5.97 id translation table", "[addresslib]")
{
    std::size_t count = 0;
    const auto* pMappings = AddressLibrary::GetIdMappings1597(count);
    REQUIRE(count > 3000);

    for (std::size_t i = 1; i < count; ++i)
        REQUIRE(pMappings[i - 1].aeId < pMappings[i].aeId);

    const auto stats = AddressLibrary::GetIdMap1597Stats();
    CHECK(stats.total == count);
    CHECK(stats.mapped > 2900);

    // Verified independently by tools/port1597/verify_pairs.py.
    CHECK(AddressLibrary::TranslateAeToSe1597(392216).value_or(0) == 513848);

    // 38952 was reported as ambiguous by the Phase 0 heuristic. It is one AE
    // address reached through two names (PerformIdleAction and the stale
    // sub_14063CFB0 identifier); docs/phase1/id_pairs_curated.csv resolves it to
    // the PerformIdleAction id, see docs/phase1/IDS_RU.md.
    CHECK(AddressLibrary::TranslateAeToSe1597(38952).value_or(0) == 37998);
    CHECK(AddressLibrary::IsKnownAeId1597(38952));

    CHECK_FALSE(AddressLibrary::TranslateAeToSe1597(1).has_value());      // unknown
    CHECK_FALSE(AddressLibrary::TranslateAeToSe1597(11612).has_value());  // still unreversed
    CHECK(AddressLibrary::IsKnownAeId1597(11612));
    CHECK_FALSE(AddressLibrary::IsKnownAeId1597(1));
}

TEST_CASE("Id audit classifies unresolved ids", "[addresslib]")
{
    AddressLibrary::Database database;
    REQUIRE(database.LoadFromMemory(BuildSparseDatabase(2, {{513848, 0x1E10E30}, {513849, 0x1E10E50}})).success);

    const auto report = AddressLibrary::AuditIds1597(
        [&database](std::uint32_t aId) { return database.FindOffsetById(aId).has_value(); });

    CHECK(report.total > 3000);
    CHECK(report.resolved < report.total);
    CHECK(report.resolved >= 2);
}
