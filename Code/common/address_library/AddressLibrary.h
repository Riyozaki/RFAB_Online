#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Portable reader for the "Address Library for SKSE Plugins" database files.
//
// Historically TiltedEvolution only accepted format 2 databases (used by the
// 1.6.x "All in one" packages).  SkyrimSE 1.5.97 ships format 1, and the most
// recent 1.7.99+ packages use format 5, so all three are supported here.
//
// Layout notes (verified against CommonLibSSE-NG, REL/IDDB.cpp):
//   * formats 1 and 2 share the identical delta-encoded record stream; the only
//     difference is the `format` field itself;
//   * format 5 stores a dense array of 32-bit offsets indexed by ID, with a
//     64-byte fixed-size module name.
//
// This file must stay free of OS specific headers: it is compiled into the
// portable CommonLib target and covered by the unit tests.

namespace AddressLibrary
{
enum class Format : std::uint32_t
{
    Unknown = 0,
    V1 = 1,
    V2 = 2,
    V5 = 5,
};

struct Version
{
    std::uint16_t major{0};
    std::uint16_t minor{0};
    std::uint16_t revision{0};
    std::uint16_t build{0};

    // Accepts "1.5.97.0", "1.6.1170.0" and the shortened "1.6.1170" form.
    static std::optional<Version> FromString(std::string_view aText) noexcept;

    // "1.6.1170.0"
    std::string ToString() const;
    // "1-6-1170-0", the suffix used by both file naming schemes.
    std::string ToFileNameSuffix() const;

    bool operator==(const Version& acOther) const noexcept;
    bool operator!=(const Version& acOther) const noexcept { return !(*this == acOther); }
    bool operator<(const Version& acOther) const noexcept;
};

struct Entry
{
    std::uint64_t id{0};
    std::uint64_t offset{0};
};

// Result of a load attempt. `error` is always populated on failure and is meant
// to be shown to the user verbatim.
struct LoadOutcome
{
    bool success{false};
    std::string error;
};

class Database
{
public:
    Database() = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Reads and parses a version library file. Never throws.
    LoadOutcome Load(const std::filesystem::path& acPath);

    // Parses a database that is already in memory (used by tests and by the
    // file loader).
    LoadOutcome LoadFromMemory(std::vector<std::uint8_t> aData);

    void Clear();

    bool IsLoaded() const noexcept { return m_format != Format::Unknown; }
    Format GetFormat() const noexcept { return m_format; }
    const Version& GetVersion() const noexcept { return m_version; }
    std::uint32_t GetPointerSize() const noexcept { return m_pointerSize; }
    const std::string& GetModuleName() const noexcept { return m_moduleName; }
    std::size_t GetEntryCount() const noexcept { return m_entries.size(); }

    // All entries, ordered by ID when the format has a sparse ID space (1/2),
    // otherwise in ID order as well (5). Returns an empty vector when the
    // database is not loaded.
    const std::vector<Entry>& GetEntries() const noexcept { return m_entries; }

    std::optional<std::uint64_t> FindOffsetById(std::uint64_t aId) const noexcept;
    std::optional<std::uint64_t> FindIdByOffset(std::uint64_t aOffset) const noexcept;

    // Diagnostic helper: how many records point past the end of the image.
    // A correctly parsed database has none; a large number means the file was
    // parsed with the wrong format assumptions.
    std::size_t CountOffsetsAbove(std::uint64_t aImageSize) const noexcept;

    std::uint64_t GetMaxOffset() const noexcept;

    static std::string_view FormatName(Format aFormat) noexcept;

private:
    LoadOutcome ParseSparseV1V2(std::size_t aRecordStart);
    LoadOutcome ParseDenseV5();

    std::vector<std::uint8_t> m_data;
    std::size_t m_recordStart{0};

    Format m_format{Format::Unknown};
    Version m_version{};
    std::uint32_t m_pointerSize{0};
    std::string m_moduleName;

    std::vector<Entry> m_entries;      // ordered by ID
    std::vector<std::uint32_t> m_dense; // format 5 payload (id -> offset, 0 = none)
};

// Builds the file names both ecosystems use for a given game version, in the
// order they should be probed:
//   "versionlib-1-6-1170-0.bin"  (Address Library for SKSE Plugins)
//   "version-1-5-97-0.bin"       (SKSE / Nexus 1.5.97 package)
std::vector<std::string> GetCandidateFileNames(const Version& acVersion);

// Human readable list of versions this build is expected to run on.
std::string GetSupportedVersionsList();
} // namespace AddressLibrary
