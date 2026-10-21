
#include <TiltedOnlineApp.h>
#include <TiltedOnlinePCH.h>

#include <Commctrl.h>
#include <Windows.h>

#include <VersionDb.h>
#include <Games/GameProfile.h>
#include <base/dialogues/win/TaskDialog.h>

#include <algorithm>
#include <cstdlib>
#include <string>

std::unique_ptr<TiltedOnlineApp> g_appInstance{nullptr};

extern HICON g_SharedWindowIcon;

namespace
{
// The reverse engineered part of the 1.5.97 port is not complete yet, so the
// client refuses to start when any id it needs cannot be resolved. Setting this
// environment variable lets a developer start the game anyway (the unresolved
// pointers stay null and will crash on use).
constexpr wchar_t kAllowUnresolvedEnv[] = L"TP_ALLOW_UNRESOLVED_ADDRESS_IDS";

std::wstring Widen(const std::string& acText)
{
    if (acText.empty())
        return {};

    const int size = MultiByteToWideChar(CP_UTF8, 0, acText.c_str(), static_cast<int>(acText.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, acText.c_str(), static_cast<int>(acText.size()), result.data(), size);
    return result;
}

bool IsUnresolvedAllowed()
{
    wchar_t buffer[8]{};
    return GetEnvironmentVariableW(kAllowUnresolvedEnv, buffer, static_cast<DWORD>(std::size(buffer))) > 0;
}

std::string FormatIdList(const std::vector<std::uint32_t>& acIds, std::size_t aLimit)
{
    std::string result;
    const auto count = std::min(acIds.size(), aLimit);
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i != 0)
            result += ", ";
        result += std::to_string(acIds[i]);
    }

    if (acIds.size() > count)
        result += ", ...";

    return result;
}

void ShowAddressLibraryError(const std::filesystem::path& acGamePath, const std::string& acReason)
{
    const auto wideReason = Widen(acReason);
    const auto detail = fmt::format(L"Looking for versionlib-<version>.bin or version-<version>.bin in:\n{}\n\nReason:\n{}",
                                    acGamePath.wstring() + L"\\Data\\SKSE\\Plugins", wideReason);

    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Failed to load the Skyrim Address Library", L"", detail.c_str());

    dia.AppendButton(0xBEED, L"Visit troubleshooting page on wiki.tiltedphoques.com");
    dia.AppendButton(0xBEEF, L"Visit Address Library modpage on nexusmods.com");
    const int result = dia.Show();
    if (result == 0xBEEF)
    {
        ShellExecuteW(nullptr, L"open", LR"(https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files)", nullptr,
                      nullptr, SW_SHOWNORMAL);
    }
    else if (result == 0xBEED)
    {
        ShellExecuteW(nullptr, L"open", LR"(https://wiki.tiltedphoques.com/tilted-online/guides/troubleshooting/address-library-error)",
                      nullptr, nullptr, SW_SHOWNORMAL);
    }

    exit(4);
}

// The build profile decides the structure layouts compiled into the client, so a
// client built for one game version cannot run on another: the offsets would be
// silently wrong instead of failing loudly.
void ShowProfileMismatchError(int aMajor, int aMinor, int aRevision, int aBuild)
{
    const auto detail = fmt::format(L"This build targets SkyrimSE {}.\n"
                                    L"The running game is {}.{}.{}.{}.\n\n"
                                    L"Rebuild the client with  xmake f --skyrim_profile={}\n"
                                    L"or run the matching game version.",
                                    TP_TARGET_GAME_VERSION_W, aMajor, aMinor, aRevision, aBuild,
                                    TP_PROFILE_IS_1597 ? L"1600" : L"1597");

    spdlog::critical("Game profile mismatch: the build targets {}, the game is {}.{}.{}.{}", TP_TARGET_GAME_VERSION, aMajor,
                     aMinor, aRevision, aBuild);

    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Wrong game version for this build", L"", detail.c_str());
    dia.AppendButton(0xBEEF, L"Close");
    dia.Show();

    exit(6);
}

void ShowIncompletePortError(const VersionDb::IdAuditResult& acAudit)
{
    const auto missing = acAudit.notReversed.size() + acAudit.notInDatabase.size() + acAudit.unknown.size();

    std::string reason = fmt::format(
        "{} of {} address ids still need a 1.5.97 counterpart:\n"
        "  not reversed yet: {}\n"
        "  reversed but missing from the installed address library: {}\n"
        "  unknown: {}\n\n"
        "The full list is written to the log and to docs/phase1/address_id_inventory.csv.\n"
        "Set TP_ALLOW_UNRESOLVED_ADDRESS_IDS=1 to start anyway (unresolved pointers will crash on use).",
        missing, acAudit.totalIds, acAudit.notReversed.size(), acAudit.notInDatabase.size(), acAudit.unknown.size());

    if (!acAudit.notReversed.empty())
        reason += "\n\nNot reversed: " + FormatIdList(acAudit.notReversed, 24);

    spdlog::error("Incomplete 1.5.97 port: {}", reason);

    if (IsUnresolvedAllowed())
    {
        spdlog::warn("TP_ALLOW_UNRESOLVED_ADDRESS_IDS is set; continuing with unresolved address ids");
        return;
    }

    // TaskDialog keeps the pointer, so the converted string must outlive it.
    const auto wideReason = Widen(reason);
    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"The 1.5.97 port is incomplete", L"", wideReason.c_str());

    dia.AppendButton(0xBEEF, L"Close");
    dia.Show();

    exit(5);
}
} // namespace

void RunTiltedInit(const std::filesystem::path& acGamePath, const String& aExeVersion)
{
    auto& versionDb = VersionDb::Get();
    if (!versionDb.Load(acGamePath, aExeVersion))
    {
        ShowAddressLibraryError(acGamePath, versionDb.GetLastError());
    }

    int major = 0, minor = 0, revision = 0, build = 0;
    if (versionDb.GetExecutableVersion(major, minor, revision, build))
    {
        int dbMajor = 0, dbMinor = 0, dbRevision = 0, dbBuild = 0;
        versionDb.GetLoadedVersion(dbMajor, dbMinor, dbRevision, dbBuild);

        if (dbMajor != major || dbMinor != minor || dbRevision != revision)
        {
            ShowAddressLibraryError(acGamePath,
                                    fmt::format("the game is {}.{}.{}.{} but the loaded address library describes {}.{}.{}.{}",
                                                major, minor, revision, build, dbMajor, dbMinor, dbRevision, dbBuild));
        }

        // The profile is chosen at build time; there is no way to adapt the
        // compiled structure layouts at runtime, so refuse to run instead of
        // corrupting memory.
        if (major != TP_TARGET_GAME_VERSION_MAJOR || minor != TP_TARGET_GAME_VERSION_MINOR ||
            revision != TP_TARGET_GAME_VERSION_REVISION)
        {
            ShowProfileMismatchError(major, minor, revision, build);
        }

        spdlog::info("Game profile {} matches SkyrimSE {}.{}.{}.{}", TP_TARGET_PROFILE_NAME, major, minor, revision, build);
    }

    const auto audit = versionDb.AuditAddressIds();
    spdlog::info("Address id audit: {}/{} resolved ({} not reversed, {} missing from the database, {} unknown)", audit.resolvedIds,
                 audit.totalIds, audit.notReversed.size(), audit.notInDatabase.size(), audit.unknown.size());

    if (!audit.IsFullyResolved())
    {
        ShowIncompletePortError(audit);
    }

    // VersionDb::Get().DumpToTextFile(R"(S:\Work\Tilted\fallout\_addresslib.txt)");

    g_appInstance = std::make_unique<TiltedOnlineApp>();

    TiltedOnlineApp::InstallHooks2();
    TP_HOOK_COMMIT;
}

void RunTiltedApp()
{
    g_appInstance->BeginMain();
}
