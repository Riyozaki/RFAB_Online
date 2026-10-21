#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <TiltedCore/Stl.hpp>

#include "address_library/AddressLibrary.h"

#pragma comment(lib, "version.lib")

// Client side wrapper around the Address Library database.
//
// Two responsibilities beyond plain id -> address lookups:
//  * it understands both file naming schemes ("versionlib-1-6-1170-0.bin" and
//    "version-1-5-97-0.bin") and every database format the game shipped so far
//    (see AddressLibrary::Database);
//  * when the game is SkyrimSE 1.5.97 it translates the AE ids that the sources
//    are written with into 1.5.97 ids before the lookup. Without that, every
//    POINTER_SKYRIMSE in the code base would point at a random function.
class VersionDb
{
public:
    struct IdAuditResult
    {
        bool databaseLoaded{false};
        bool translationEnabled{false};
        std::size_t totalIds{0};
        std::size_t resolvedIds{0};
        // Known ids the code references but that are not reversed yet.
        std::vector<std::uint32_t> notReversed;
        // Reversed ids that the installed database does not contain (a wrong
        // mapping, or an address library file for another game build).
        std::vector<std::uint32_t> notInDatabase;
        // Ids the code references that are missing from the mapping table
        // entirely (should always be empty: the table is generated from source).
        std::vector<std::uint32_t> unknown;

        bool IsFullyResolved() const noexcept
        {
            return databaseLoaded && notReversed.empty() && notInDatabase.empty() && unknown.empty();
        }
    };

    VersionDb() = default;

    static VersionDb& Get();

    bool Load(const std::filesystem::path& acGamePath, const TiltedPhoques::String& acExeVersion);
    bool Load(const std::filesystem::path& acGamePath, int major, int minor, int revision, int build);
    bool Load(const std::filesystem::path& acGamePath, std::string_view acExeVersion);

    void Clear();

    const std::string& GetLastError() const noexcept { return m_lastError; }
    const std::string& GetLoadedVersionString() const noexcept { return m_loadedVersion; }
    const std::string& GetLoadedFileName() const noexcept { return m_loadedFileName; }
    const std::string& GetModuleName() const noexcept { return m_database.GetModuleName(); }

    bool IsLoaded() const noexcept { return m_database.IsLoaded(); }
    // True when the loaded database belongs to SkyrimSE 1.5.97, i.e. when the
    // AE -> SE id translation is active.
    bool IsPreAnniversary() const noexcept { return m_translationEnabled; }
    AddressLibrary::Format GetFormat() const noexcept { return m_database.GetFormat(); }
    const AddressLibrary::Database& GetDatabase() const noexcept { return m_database; }

    // Converts an id as written in the sources into the id of the loaded game
    // build. Returns 0 when the id is not mapped.
    std::uint64_t ResolveId(std::uint64_t aAeId) const noexcept;

    void* FindAddressById(unsigned long long aId) const;
    bool FindOffsetById(unsigned long long aId, unsigned long long& aResult) const;

    bool FindIdByAddress(void* apPointer, unsigned long long& aResult) const;
    bool FindIdByOffset(unsigned long long aOffset, unsigned long long& aResult) const;

    bool GetExecutableVersion(int& aMajor, int& aMinor, int& aRevision, int& aBuild) const;
    void GetLoadedVersion(int& aMajor, int& aMinor, int& aRevision, int& aBuild) const;

    IdAuditResult AuditAddressIds() const;

    bool DumpToTextFile(const std::string& acPath) const;

private:
    void LoadBaseAddress();
    void CheckImageBounds();

    AddressLibrary::Database m_database;
    std::string m_lastError;
    std::string m_loadedVersion;
    std::string m_loadedFileName;
    std::uint64_t m_base{0};
    bool m_translationEnabled{false};
};
