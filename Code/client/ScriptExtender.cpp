
#include <ScriptExtender.h>
#include <TiltedOnlinePCH.h>
#include <VersionDb.h>

extern void WriteBootTrace(const char* apMsg, bool aReset);

namespace
{
constexpr wchar_t kScriptExtenderName[] = L"skse64";

constexpr char kScriptExtenderEntrypoint[] = "StartSKSE";

constexpr size_t kScriptExtenderNameLength = sizeof(kScriptExtenderName) / sizeof(wchar_t) - 1;

// RFAB Online targets Skyrim SE 1.5.97 with SKSE64 2.0.20 only.
// SKSE stores its version in VS_FIXEDFILEINFO as 0.2.0.20 for the 2.0.20 release.
constexpr DWORD kExpectedSKSEVersion[4] = {0, 2, 0, 20};

HMODULE g_SKSEModuleHandle{nullptr};

struct FileVersion
{
    static constexpr uint8_t scVersionSize = 4;
    DWORD versions[scVersionSize];
};

int GetFileVersion(const std::filesystem::path& acFilePath, FileVersion& aVersion)
{
    const auto filename = acFilePath.c_str();

    DWORD dwHandle = 0, sz = GetFileVersionInfoSizeW(filename, &dwHandle);
    if (0 == sz)
    {
        return 1;
    }
    std::string buf(sz, '\0');
    if (!GetFileVersionInfoW(filename, dwHandle, sz, &buf[0]))
    {
        return 2;
    }
    VS_FIXEDFILEINFO* pvi;
    sz = sizeof(VS_FIXEDFILEINFO);
    if (!VerQueryValueA(&buf[0], "\\", reinterpret_cast<LPVOID*>(&pvi), reinterpret_cast<unsigned int*>(&sz)))
    {
        return 3;
    }

    aVersion.versions[0] = pvi->dwFileVersionMS >> 16;
    aVersion.versions[1] = pvi->dwFileVersionMS & 0xFFFF;
    aVersion.versions[2] = pvi->dwFileVersionLS >> 16;
    aVersion.versions[3] = pvi->dwFileVersionLS & 0xFFFF;

    return 0;
}

std::wstring GetSKSEStyleExeVersionW()
{
    int major = 0, minor = 0, revision = 0, build = 0;
    VersionDb::Get().GetLoadedVersion(major, minor, revision, build);
    if (build == 0)
        return fmt::format(L"{}_{}_{}", major, minor, revision);

    return fmt::format(L"{}_{}_{}_{}", major, minor, revision, build);
}
} // namespace

bool IsScriptExtenderLoaded()
{
    return g_SKSEModuleHandle;
}

void LoadScriptExender()
{
    const auto exeVersionW{GetSKSEStyleExeVersionW()};

    // Get the path of the game, where the Script Extender dll resides
    std::error_code ec;
    const auto gameDir = std::filesystem::current_path(ec);
    if (ec || gameDir.empty())
    {
        WriteBootTrace("[SKSE] Failed to query current_path() for SKSE search", false);
        return;
    }

    std::list<std::filesystem::path> dllMatches;
    for (const auto& dirEntry : std::filesystem::directory_iterator(gameDir, ec))
    {
        const auto& path = dirEntry.path();
        if (_wcsicmp(path.extension().c_str(), L".dll") != 0)
            continue;

        auto fileName = path.filename().wstring();
        if (fileName.length() <= kScriptExtenderNameLength)
            continue;

        if (_wcsnicmp(fileName.c_str(), kScriptExtenderName, kScriptExtenderNameLength) == 0)
        {
            dllMatches.push_back(path);
        }
    }

    // Match entirely in wide UTF-16 so non-ASCII / Cyrillic filenames in gameDir NEVER throw 0xE06D7363 on .string()!
    std::filesystem::path* needle = nullptr;
    for (auto& match : dllMatches)
    {
        const auto fnameW = match.filename().wstring();
        if (fnameW.length() <= kScriptExtenderNameLength + 1)
            continue;
        const wchar_t* ptr = fnameW.c_str() + kScriptExtenderNameLength + 1;
        if (_wcsnicmp(ptr, exeVersionW.c_str(), exeVersionW.length()) == 0)
        {
            needle = &match;
            break;
        }
    }

    if (!needle)
    {
        WriteBootTrace("[SKSE] No matching skse64_1_5_97.dll found in game root", false);
        return;
    }

    FileVersion fileVersion{};
    if (GetFileVersion(*needle, fileVersion) != 0)
    {
        WriteBootTrace("[SKSE] Unable to read VS_FIXEDFILEINFO from skse64_1_5_97.dll", false);
        spdlog::error("Unable to verify Script Extender version");
        return;
    }

    auto skseVersion = fmt::format("v{}.{}.{}.{}", fileVersion.versions[0], fileVersion.versions[1], fileVersion.versions[2], fileVersion.versions[3]);

    bool isExpectedSKSE = true;
    for (int i = 0; i < FileVersion::scVersionSize; i++)
    {
        if (fileVersion.versions[i] != kExpectedSKSEVersion[i])
            isExpectedSKSE = false;
    }

    if (!isExpectedSKSE)
    {
        spdlog::error("Unsupported SKSE version {}: RFAB Online requires SKSE64 2.0.20 (file version 0.2.0.20) for Skyrim SE 1.5.97. SKSE was not started.", skseVersion);
        return;
    }

    WriteBootTrace("[SKSE 1/3] Loading skse64_1_5_97.dll via LoadLibraryW...", false);
    if (g_SKSEModuleHandle = LoadLibraryW(needle->c_str()))
    {
        if (auto* pStartSKSE = reinterpret_cast<void (*)()>(GetProcAddress(g_SKSEModuleHandle, kScriptExtenderEntrypoint)))
        {
            spdlog::info(
                "Starting SKSE {}... be aware that messages that start without a colored [timestamp] prefix are "
                "logs from the "
                "Script Extender and its loaded mods.",
                skseVersion);
            WriteBootTrace("[SKSE 2/3] Invoking StartSKSE()...", false);
            pStartSKSE();
            WriteBootTrace("[SKSE 3/3] StartSKSE() finished cleanly — all SKSE plugins initialized!", false);
            spdlog::info("SKSE is active");
        }
        else
        {
            WriteBootTrace("[SKSE] StartSKSE export not found in skse64_1_5_97.dll", false);
            spdlog::warn("SKSE dll doesn't expose StartSKSE(), it may be outdated.");
        }
    }
    else
    {
        WriteBootTrace("[SKSE] LoadLibraryW(skse64_1_5_97.dll) returned NULL!", false);
        spdlog::error("Failed to load SKSE DLL! Check your privileges or re-download the Script Extender files.");
    }
}
