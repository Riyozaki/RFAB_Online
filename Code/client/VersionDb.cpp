#include "VersionDb.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <unordered_set>

namespace
{
constexpr std::uint16_t kSESkyrimMinor = 5;
constexpr std::uint16_t kSESkyrimRevision = 97;

std::uint64_t GetMainImageSize()
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0)
        return 0;

    const auto* pDosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    const auto* pNtHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + pDosHeader->e_lfanew);
    if (pNtHeaders->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    return pNtHeaders->OptionalHeader.SizeOfImage;
}
} // namespace

VersionDb& VersionDb::Get()
{
    static VersionDb s_db;
    return s_db;
}

bool VersionDb::Load(const std::filesystem::path& acGamePath, const TiltedPhoques::String& acExeVersion)
{
    return Load(acGamePath, std::string_view(acExeVersion.c_str(), acExeVersion.size()));
}

bool VersionDb::Load(const std::filesystem::path& acGamePath, std::string_view acExeVersion)
{
    const auto version = AddressLibrary::Version::FromString(acExeVersion);
    if (!version)
    {
        m_lastError = "unable to parse the game version \"" + std::string(acExeVersion) + "\"";
        return false;
    }

    return Load(acGamePath, version->major, version->minor, version->revision, version->build);
}

bool VersionDb::Load(const std::filesystem::path& acGamePath, int major, int minor, int revision, int build)
{
    Clear();

    if (major < 0 || minor < 0 || revision < 0 || build < 0 || minor > 0xFFFF || revision > 0xFFFF || build > 0xFFFF)
    {
        m_lastError = "invalid game version";
        return false;
    }

    const AddressLibrary::Version requested{static_cast<std::uint16_t>(major), static_cast<std::uint16_t>(minor),
                                            static_cast<std::uint16_t>(revision), static_cast<std::uint16_t>(build)};

    m_loadedVersion = requested.ToString();

    const auto pluginsDirectory = acGamePath / "Data" / "SKSE" / "Plugins";
    std::string attempts;

    for (const auto& fileName : AddressLibrary::GetCandidateFileNames(requested))
    {
        const auto fullPath = pluginsDirectory / fileName;

        auto outcome = m_database.Load(fullPath);
        if (!outcome.success)
        {
            attempts += "\n  " + fileName + ": " + outcome.error;
            continue;
        }

        const auto& databaseVersion = m_database.GetVersion();
        if (databaseVersion != requested)
        {
            // A file for another game build is present under the expected name:
            // refuse instead of resolving addresses against the wrong image.
            attempts += "\n  " + fileName + ": contains data for " + databaseVersion.ToString() + ", expected " + requested.ToString();
            m_database.Clear();
            continue;
        }

        m_loadedFileName = fileName;
        m_translationEnabled = databaseVersion.minor == kSESkyrimMinor && databaseVersion.revision == kSESkyrimRevision;

        LoadBaseAddress();
        CheckImageBounds();

        if (m_translationEnabled)
        {
            const auto stats = AddressLibrary::GetIdMap1597Stats();
            spdlog::info("SkyrimSE {} detected: using address library format {}, translating {} of {} AE ids to 1.5.97 ids",
                         m_loadedVersion, std::string(AddressLibrary::Database::FormatName(m_database.GetFormat())), stats.mapped,
                         stats.total);
        }
        else
        {
            spdlog::info("SkyrimSE {} detected: using address library format {} ({} entries)", m_loadedVersion,
                         std::string(AddressLibrary::Database::FormatName(m_database.GetFormat())), m_database.GetEntryCount());
        }

        return true;
    }

    m_lastError = "no usable address library database in " + pluginsDirectory.string() + ":" + attempts;
    m_database.Clear();
    return false;
}

void VersionDb::Clear()
{
    m_database.Clear();
    m_lastError.clear();
    m_loadedVersion.clear();
    m_loadedFileName.clear();
    m_base = 0;
    m_translationEnabled = false;
}

void VersionDb::LoadBaseAddress()
{
    m_base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
}

void VersionDb::CheckImageBounds()
{
    const auto imageSize = GetMainImageSize();
    if (imageSize == 0)
    {
        spdlog::warn("Unable to read the image size of the game executable; skipping the address range sanity check");
        return;
    }

    const auto outside = m_database.CountOffsetsAbove(imageSize);
    if (outside == 0)
        return;

    // A correctly parsed database never points outside the module. This catches
    // a wrong format assumption early instead of crashing on a wild pointer.
    spdlog::error("Address library sanity check failed: {} of {} entries point outside the game image ({} bytes). "
                  "The database was probably parsed with the wrong format.",
                  outside, m_database.GetEntryCount(), imageSize);
}

std::uint64_t VersionDb::ResolveId(unsigned long long aAeId) const noexcept
{
    if (!m_translationEnabled)
        return aAeId;

    const auto translated = AddressLibrary::TranslateAeToSe1597(aAeId);
    return translated.has_value() ? *translated : 0;
}

void* VersionDb::FindAddressById(unsigned long long aId) const
{
    if (m_base == 0)
        return nullptr;

    const auto resolved = ResolveId(aId);
    if (resolved == 0)
    {
        spdlog::error("Address id {} has no reverse engineered 1.5.97 counterpart yet (see "
                      "docs/phase1/address_id_inventory.csv)",
                      aId);
        return nullptr;
    }

    const auto offset = m_database.FindOffsetById(resolved);
    if (!offset.has_value())
    {
        spdlog::error("Address id {} (resolved to {}) is missing from the loaded address library", aId, resolved);
        return nullptr;
    }

    return reinterpret_cast<void*>(m_base + *offset);
}

bool VersionDb::FindOffsetById(unsigned long long aId, unsigned long long& aResult) const
{
    const auto resolved = ResolveId(aId);
    if (resolved == 0)
        return false;

    const auto offset = m_database.FindOffsetById(resolved);
    if (!offset.has_value())
        return false;

    aResult = *offset;
    return true;
}

bool VersionDb::FindIdByAddress(void* apPointer, unsigned long long& aResult) const
{
    if (m_base == 0 || apPointer == nullptr)
        return false;

    return FindIdByOffset(reinterpret_cast<std::uint64_t>(apPointer) - m_base, aResult);
}

bool VersionDb::FindIdByOffset(unsigned long long aOffset, unsigned long long& aResult) const
{
    const auto id = m_database.FindIdByOffset(aOffset);
    if (!id.has_value())
        return false;

    aResult = *id;
    return true;
}

bool VersionDb::GetExecutableVersion(int& aMajor, int& aMinor, int& aRevision, int& aBuild) const
{
    wchar_t szVersionFile[MAX_PATH];
    if (GetModuleFileNameW(nullptr, szVersionFile, MAX_PATH) == 0)
        return false;

    DWORD verHandle = 0;
    const DWORD verSize = GetFileVersionInfoSizeW(szVersionFile, &verHandle);
    if (verSize == 0)
        return false;

    std::vector<char> verData(verSize);
    if (!GetFileVersionInfoW(szVersionFile, verHandle, verSize, verData.data()))
        return false;

    char* pVersionString = nullptr;
    UINT versionLength = 0;

    if (VerQueryValueA(verData.data(), "\\StringFileInfo\\040904B0\\ProductVersion", reinterpret_cast<LPVOID*>(&pVersionString),
                       &versionLength) &&
        versionLength != 0 && pVersionString != nullptr)
    {
        const auto parsed = AddressLibrary::Version::FromString(std::string_view(pVersionString, versionLength));
        if (parsed)
        {
            aMajor = parsed->major;
            aMinor = parsed->minor;
            aRevision = parsed->revision;
            aBuild = parsed->build;
            return true;
        }
    }

    if (VerQueryValueA(verData.data(), "\\StringFileInfo\\040904B0\\FileVersion", reinterpret_cast<LPVOID*>(&pVersionString),
                       &versionLength) &&
        versionLength != 0 && pVersionString != nullptr)
    {
        const auto parsed = AddressLibrary::Version::FromString(std::string_view(pVersionString, versionLength));
        if (parsed)
        {
            aMajor = parsed->major;
            aMinor = parsed->minor;
            aRevision = parsed->revision;
            aBuild = parsed->build;
            return true;
        }
    }

    return false;
}

void VersionDb::GetLoadedVersion(int& aMajor, int& aMinor, int& aRevision, int& aBuild) const
{
    const auto version = m_database.GetVersion();
    aMajor = version.major;
    aMinor = version.minor;
    aRevision = version.revision;
    aBuild = version.build;
}

VersionDb::IdAuditResult VersionDb::AuditAddressIds() const
{
    IdAuditResult result{};
    result.databaseLoaded = m_database.IsLoaded();
    result.translationEnabled = m_translationEnabled;

    if (!result.databaseLoaded)
        return result;

    std::size_t mappingCount = 0;
    const auto* pMappings = AddressLibrary::GetIdMappings1597(mappingCount);
    result.totalIds = mappingCount;

    std::unordered_set<std::uint32_t> known;
    known.reserve(mappingCount);
    for (std::size_t i = 0; i < mappingCount; ++i)
    {
        known.insert(pMappings[i].aeId);
    }

    for (std::size_t i = 0; i < mappingCount; ++i)
    {
        const auto& mapping = pMappings[i];

        if (!m_translationEnabled)
        {
            // 1.6.x: the ids in the sources are the ids of the loaded database.
            if (m_database.FindOffsetById(mapping.aeId).has_value())
                ++result.resolvedIds;
            else
                result.notInDatabase.push_back(mapping.aeId);
            continue;
        }

        if (mapping.seId == 0)
        {
            result.notReversed.push_back(mapping.aeId);
            continue;
        }

        if (m_database.FindOffsetById(mapping.seId).has_value())
            ++result.resolvedIds;
        else
            result.notInDatabase.push_back(mapping.aeId);
    }

    (void)known;
    return result;
}

bool VersionDb::DumpToTextFile(const std::string& acPath) const
{
    if (!m_database.IsLoaded())
        return false;

    std::FILE* pFile = nullptr;
    if (fopen_s(&pFile, acPath.c_str(), "w") != 0 || pFile == nullptr)
        return false;

    for (const auto& entry : m_database.GetEntries())
    {
        std::fprintf(pFile, "%llu\t%llx\n", static_cast<unsigned long long>(entry.id),
                     static_cast<unsigned long long>(entry.offset) + 0x140000000ull);
    }

    std::fclose(pFile);
    return true;
}
