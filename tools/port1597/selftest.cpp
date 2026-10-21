// Standalone smoke test for the portable address library layer.
//
// The repository's unit tests live in Code/tests and run through xmake+catch2.
// This file exists so the parsing/translation code can be verified on any
// machine with a C++20 compiler (no xmake, no Windows, no game required):
//
//   g++ -std=c++20 -I Code/common tools/port1597/selftest.cpp
//       Code/common/address_library/AddressLibrary.cpp
//       Code/common/address_library/SkyrimIdMap.cpp -o /tmp/selftest
//   /tmp/selftest --database /path/to/version-1-5-97-0.bin
//
// Exit code 0 means every check passed.

#include "address_library/AddressLibrary.h"
#include "address_library/SkyrimIdMap.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;
int g_checks = 0;

void Check(bool aCondition, const std::string& acWhat)
{
    ++g_checks;
    if (!aCondition)
    {
        ++g_failures;
        std::printf("FAIL: %s\n", acWhat.c_str());
    }
}

template <class T> void CheckEqual(const T& aActual, const T& aExpected, const std::string& acWhat)
{
    ++g_checks;
    if (!(aActual == aExpected))
    {
        ++g_failures;
        if constexpr (std::is_integral_v<T>)
            std::printf("FAIL: %s (expected %llu, got %llu)\n", acWhat.c_str(),
                        static_cast<unsigned long long>(aExpected), static_cast<unsigned long long>(aActual));
        else
            std::printf("FAIL: %s\n", acWhat.c_str());
    }
}

void Append(std::vector<std::uint8_t>& aBuffer, const void* apData, std::size_t aSize)
{
    const auto* pBytes = static_cast<const std::uint8_t*>(apData);
    aBuffer.insert(aBuffer.end(), pBytes, pBytes + aSize);
}

template <class T> void Append(std::vector<std::uint8_t>& aBuffer, T aValue)
{
    Append(aBuffer, &aValue, sizeof(T));
}

struct Record
{
    std::uint64_t id;
    std::uint64_t offset;
};

// Minimal writer for formats 1/2. `aUseScaleBit` exercises the
// "offset stored divided by pointer size" encoding.
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
        std::uint8_t type = 0;

        // id encoding
        if (record.id == previousId + 1)
        {
            type = 0x1;
        }
        else
        {
            type = 0x7; // absolute u32
        }

        // offset encoding, high nibble
        std::uint8_t offsetKind = 0;
        std::uint64_t scaledValue = 0;
        bool scaled = false;

        if (record.offset == previousOffset + 1)
        {
            offsetKind = 0x1;
        }
        else if (aUseScaleBit && aPointerSize > 0 && record.offset % aPointerSize == 0 &&
                 record.offset / aPointerSize == previousOffset / aPointerSize + 1)
        {
            offsetKind = 0x9; // bit 8 (scaled) + kind 1 (previous + 1)
            scaled = true;
        }
        else
        {
            offsetKind = 0x7; // absolute u32
            scaledValue = record.offset;
        }

        Append<std::uint8_t>(buffer, static_cast<std::uint8_t>(type | (offsetKind << 4)));

        if (type == 0x7)
            Append<std::uint32_t>(buffer, static_cast<std::uint32_t>(record.id));

        if (offsetKind == 0x7)
            Append<std::uint32_t>(buffer, static_cast<std::uint32_t>(scaledValue));

        (void)scaled;

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

void TestVersionParsing()
{
    const auto full = AddressLibrary::Version::FromString("1.5.97.0");
    Check(full.has_value(), "1.5.97.0 parses");
    if (full)
    {
        CheckEqual<int>(full->major, 1, "major");
        CheckEqual<int>(full->minor, 5, "minor");
        CheckEqual<int>(full->revision, 97, "revision");
        CheckEqual<int>(full->build, 0, "build");
        CheckEqual<std::string>(full->ToString(), "1.5.97.0", "version to string");
        CheckEqual<std::string>(full->ToFileNameSuffix(), "1-5-97-0", "version to file suffix");
    }

    const auto shortForm = AddressLibrary::Version::FromString("1.6.1170");
    Check(shortForm.has_value(), "1.6.1170 parses without a build number");
    if (shortForm)
        CheckEqual<int>(shortForm->build, 0, "missing build defaults to 0");

    Check(!AddressLibrary::Version::FromString("").has_value(), "empty version rejected");
    Check(!AddressLibrary::Version::FromString("1").has_value(), "single component version rejected");
    Check(!AddressLibrary::Version::FromString("1.6.1170.0.1").has_value(), "five component version rejected");
    Check(!AddressLibrary::Version::FromString("game").has_value(), "garbage rejected");

    const auto names = AddressLibrary::GetCandidateFileNames(AddressLibrary::Version{1, 5, 97, 0});
    CheckEqual<std::size_t>(names.size(), 2, "two candidate file names");
    CheckEqual<std::string>(names[0], "versionlib-1-5-97-0.bin", "first candidate name");
    CheckEqual<std::string>(names[1], "version-1-5-97-0.bin", "second candidate name");
}

void TestSparseFormats()
{
    const std::vector<Record> records{{1, 0x1000}, {2, 0x1008}, {5, 0x2000}, {9, 0x2008}, {10, 0x2009}};

    for (const auto format : {1, 2})
    {
        AddressLibrary::Database database;
        const auto outcome = database.LoadFromMemory(BuildSparseDatabase(format, records));
        Check(outcome.success, "sparse database loads (format " + std::to_string(format) + ")");
        if (!outcome.success)
        {
            std::printf("  error: %s\n", outcome.error.c_str());
            continue;
        }

        CheckEqual<int>(static_cast<int>(database.GetFormat()), format, "format is preserved");
        CheckEqual<unsigned long long>(database.GetVersion().revision, 1170ull, "version is read");
        CheckEqual<std::string>(database.GetModuleName(), "SkyrimSE.exe", "module name is read");
        CheckEqual<unsigned long long>(database.GetEntryCount(), records.size(), "entry count");

        for (const auto& record : records)
        {
            const auto offset = database.FindOffsetById(record.id);
            Check(offset.has_value(), "id " + std::to_string(record.id) + " resolves");
            if (offset)
                CheckEqual<unsigned long long>(*offset, record.offset, "offset of id " + std::to_string(record.id));
        }

        Check(!database.FindOffsetById(3).has_value(), "unknown id does not resolve");
    }

    // The same payload with the scale bit disabled must decode identically.
    AddressLibrary::Database scaled;
    AddressLibrary::Database unscaled;
    Check(scaled.LoadFromMemory(BuildSparseDatabase(2, records, 8, true)).success, "scaled database loads");
    Check(unscaled.LoadFromMemory(BuildSparseDatabase(2, records, 8, false)).success, "unscaled database loads");
    CheckEqual<unsigned long long>(scaled.GetMaxOffset(), unscaled.GetMaxOffset(), "scaled and unscaled agree");
}

void TestDenseFormat()
{
    AddressLibrary::Database database;
    const auto outcome = database.LoadFromMemory(BuildDenseDatabase(10, {{3, 0x1234}, {7, 0x5678}}));
    Check(outcome.success, "format 5 database loads");
    if (!outcome.success)
    {
        std::printf("  error: %s\n", outcome.error.c_str());
        return;
    }

    CheckEqual<int>(static_cast<int>(database.GetFormat()), 5, "format 5 reported");
    CheckEqual<unsigned long long>(database.GetEntryCount(), 2ull, "only non-empty entries are kept");
    CheckEqual<unsigned long long>(*database.FindOffsetById(3), 0x1234ull, "dense offset of id 3");
    Check(!database.FindOffsetById(4).has_value(), "empty dense slot does not resolve");
    Check(!database.FindOffsetById(100).has_value(), "out of range dense id does not resolve");
}

void TestFailures()
{
    AddressLibrary::Database database;

    const auto unsupported = database.LoadFromMemory(BuildSparseDatabase(3, {{1, 0x1000}}));
    Check(!unsupported.success, "format 3 is rejected");
    Check(unsupported.error.find('3') != std::string::npos, "error mentions the unsupported format number");

    auto truncated = BuildSparseDatabase(2, {{1, 0x1000}, {2, 0x1008}, {3, 0x1010}});
    truncated.resize(truncated.size() - 6);
    const auto truncatedOutcome = database.LoadFromMemory(truncated);
    Check(!truncatedOutcome.success, "truncated database is rejected");

    const auto empty = database.LoadFromMemory({});
    Check(!empty.success, "empty buffer is rejected");

    const auto outcome = database.LoadFromMemory(BuildSparseDatabase(2, {}));
    Check(!outcome.success, "database without entries is rejected");
}

void TestTranslationTable()
{
    std::size_t count = 0;
    const auto* pMappings = AddressLibrary::GetIdMappings1597(count);

    Check(count > 3000, "translation table covers the whole id surface");

    bool sorted = true;
    for (std::size_t i = 1; i < count; ++i)
    {
        if (pMappings[i - 1].aeId >= pMappings[i].aeId)
        {
            sorted = false;
            break;
        }
    }
    Check(sorted, "translation table is strictly sorted by AE id");

    const auto stats = AddressLibrary::GetIdMap1597Stats();
    CheckEqual<std::size_t>(stats.total, count, "stats total matches the table");
    std::printf("  translation table: %zu ids, %zu mapped, %zu pending\n", stats.total, stats.mapped, stats.total - stats.mapped);

    // Spot checks that are independently verified by tools/port1597/verify_pairs.py
    const auto tesForm = AddressLibrary::TranslateAeToSe1597(392216);
    Check(tesForm.has_value(), "RTTI_TESForm (ae 392216) is mapped");
    CheckEqual<unsigned int>(tesForm.value_or(0), 513848u, "RTTI_TESForm maps to 513848");

    // 38952 is one AE address used under two names (PerformIdleAction and the
    // stale sub_14063CFB0). docs/phase1/id_pairs_curated.csv resolves it to the
    // PerformIdleAction id 37998; see docs/phase1/IDS_RU.md for the reasoning.
    CheckEqual<unsigned int>(AddressLibrary::TranslateAeToSe1597(38952).value_or(0), 37998u,
                             "aliased id 38952 maps to PerformIdleAction (37998)");

    // Pairs recovered from the fork's own 1.5.97 tree (tools/port1597/history_pairs.py).
    CheckEqual<unsigned int>(AddressLibrary::TranslateAeToSe1597(17201).value_or(0), 16828u,
                             "s_getRefrByHandle maps to 16828");
    CheckEqual<unsigned int>(AddressLibrary::TranslateAeToSe1597(400312).value_or(0), 514164u,
                             "s_nullHandle maps to 514164");

    Check(!AddressLibrary::TranslateAeToSe1597(1).has_value(), "unknown id is not mapped");
    Check(!AddressLibrary::TranslateAeToSe1597(11612).has_value(),
          "still unreversed id 11612 has no invented mapping");
}

void TestAgainstRealDatabase(const std::string& acPath)
{
    AddressLibrary::Database database;
    const auto outcome = database.Load(acPath);
    Check(outcome.success, "real 1.5.97 database loads");
    if (!outcome.success)
    {
        std::printf("  error: %s\n", outcome.error.c_str());
        return;
    }

    std::printf("  %s: format %s, version %s, %zu entries, max offset %#llx\n", acPath.c_str(),
                std::string(AddressLibrary::Database::FormatName(database.GetFormat())).c_str(),
                database.GetVersion().ToString().c_str(), database.GetEntryCount(),
                static_cast<unsigned long long>(database.GetMaxOffset()));

    CheckEqual<int>(static_cast<int>(database.GetFormat()), 1, "real 1.5.97 database uses format 1");
    Check(database.GetVersion() == AddressLibrary::Version{1, 5, 97, 0}, "real database describes 1.5.97.0");

    // SkyrimSE.exe 1.5.97 is just under 57 MiB; anything above this means the
    // parser drifted.
    Check(database.GetMaxOffset() < 0x3900000ull, "no offset points past the end of the image");
    CheckEqual<unsigned long long>(database.CountOffsetsAbove(0x3900000ull), 0ull, "all offsets are inside the image");

    std::size_t count = 0;
    const auto* pMappings = AddressLibrary::GetIdMappings1597(count);

    std::size_t resolved = 0;
    std::size_t missing = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (pMappings[i].seId == 0)
            continue;

        if (database.FindOffsetById(pMappings[i].seId).has_value())
            ++resolved;
        else
            ++missing;
    }

    std::printf("  mapped ids present in the database: %zu, missing: %zu\n", resolved, missing);
    CheckEqual<unsigned long long>(missing, 0ull, "every mapped id exists in the 1.5.97 database");
}

void TestAudit()
{
    // A database that only knows a handful of ids: the audit must classify the
    // rest as missing instead of silently succeeding.
    AddressLibrary::Database database;
    const std::vector<Record> records{{513848, 0x1E10E30}, {513849, 0x1E10E50}};
    Check(database.LoadFromMemory(BuildSparseDatabase(2, records)).success, "audit fixture loads");

    const auto report = AddressLibrary::AuditIds1597(
        [&database](std::uint32_t aId) { return database.FindOffsetById(aId).has_value(); });

    Check(report.total > 3000, "audit iterates the whole table");
    Check(report.resolved < report.total, "audit reports unresolved ids");
    Check(!report.unknown.empty() || report.resolved > 0, "audit produces a classification");
}
} // namespace

int main(int argc, char** argv)
{
    std::string databasePath;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--database") == 0 && i + 1 < argc)
            databasePath = argv[++i];
    }

    std::printf("version parsing\n");
    TestVersionParsing();

    std::printf("sparse formats (1, 2)\n");
    TestSparseFormats();

    std::printf("dense format (5)\n");
    TestDenseFormat();

    std::printf("failure handling\n");
    TestFailures();

    std::printf("translation table\n");
    TestTranslationTable();

    std::printf("audit\n");
    TestAudit();

    if (!databasePath.empty())
    {
        std::printf("real database\n");
        TestAgainstRealDatabase(databasePath);
    }
    else
    {
        std::printf("real database: skipped (pass --database)\n");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
