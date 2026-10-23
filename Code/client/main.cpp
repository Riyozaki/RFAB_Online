
#include <TiltedOnlineApp.h>
#include <TiltedOnlinePCH.h>

#include <Commctrl.h>
#include <Windows.h>
#include <mutex>

#include <base/dialogues/win/TaskDialog.h>

std::unique_ptr<TiltedOnlineApp> g_appInstance{nullptr};

extern HICON g_SharedWindowIcon;

namespace
{
std::filesystem::path s_pendingGamePath;
String s_pendingExeVersion;
std::once_flag s_tiltedInitOnce;
} // namespace

static void ShowAddressLibraryError(const wchar_t* apGamePath)
{
    auto errorDetail = fmt::format(L"Looking for it here: {}\\Data\\SKSE\\Plugins", apGamePath);

    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Failed to load Skyrim Address Library",
                         L"RFAB Online requires Skyrim SE 1.5.97 with the Address Library for SE (version-1-5-97-0.bin)", errorDetail.c_str());

    dia.AppendButton(0xBEED, L"Visit troubleshooting page on wiki.tiltedphoques.com");
    dia.AppendButton(0xBEEF, L"Visit Address Library modpage on nexusmods.com");
    const int result = dia.Show();
    if (result == 0xBEEF)
    {
        ShellExecuteW(nullptr, L"open", LR"(https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files)", nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (result == 0xBEED)
    {
        ShellExecuteW(nullptr, L"open", LR"(https://wiki.tiltedphoques.com/tilted-online/guides/troubleshooting/address-library-error)", nullptr, nullptr, SW_SHOWNORMAL);
    }

    exit(4);
}

// RFAB Online targets exactly one game build: Skyrim Special Edition 1.5.97
// (any distribution -- Steam, downgraded, GOG, or standalone/CODEX .cdx -- as long
// as the exe is 1.5.97). All reversed struct layouts and mid-function patches in
// this fork are audited for that build only, so refuse to install hooks on
// anything else instead of crashing inside the game.
static void ShowUnsupportedVersionError(const std::string& acDetectedVersion)
{
    const std::wstring detected(acDetectedVersion.begin(), acDetectedVersion.end());
    auto errorDetail = fmt::format(L"Detected game version: {}. RFAB Online supports Skyrim SE 1.5.97 (SKSE64 2.0.20) only. Anniversary Edition (1.6.x) is not supported.", detected);

    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Unsupported Skyrim Special Edition version", L"RFAB Online requires Skyrim SE 1.5.97", errorDetail.c_str());
    dia.Show();

    exit(5);
}

void EnsureTiltedInitialized()
{
    std::call_once(s_tiltedInitOnce, []() {
        if (!VersionDb::Get().Load(s_pendingGamePath, s_pendingExeVersion))
        {
            ShowAddressLibraryError(s_pendingGamePath.c_str());
        }

        {
            int major = 0, minor = 0, revision = 0, build = 0;
            VersionDb::Get().GetLoadedVersion(major, minor, revision, build);

            if (major != 1 || minor != 5 || revision != 97)
            {
                ShowUnsupportedVersionError(VersionDb::Get().GetLoadedVersionString());
            }
        }

        g_appInstance = std::make_unique<TiltedOnlineApp>();

        TiltedOnlineApp::InstallHooks2();
        TP_HOOK_COMMIT;
    });
}

void RunTiltedInit(const std::filesystem::path& acGamePath, const String& aExeVersion)
{
    // Commit ONLY pre-entry IAT hooks (_initterm_e + GetStartupInfoW/RaiseException) before LC->gameMain()!
    // Installing .text detours inside EnsureTiltedInitialized() at the start of CRT startup guarantees
    // standalone/CODEX (.cdx) entry-point stubs finish unpacking .text before any code hook is written.
    s_pendingGamePath = acGamePath;
    s_pendingExeVersion = aExeVersion;
    TP_HOOK_COMMIT;
}

void RunTiltedApp()
{
    EnsureTiltedInitialized();
    g_appInstance->BeginMain();
}
