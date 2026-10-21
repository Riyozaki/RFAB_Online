
#include <TiltedOnlineApp.h>
#include <TiltedOnlinePCH.h>

#include <Commctrl.h>
#include <Windows.h>

#include <base/dialogues/win/TaskDialog.h>

std::unique_ptr<TiltedOnlineApp> g_appInstance{nullptr};

extern HICON g_SharedWindowIcon;

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
// (any distribution -- Steam, downgraded, or DRM-free -- as long as the exe is
// 1.5.97). All reversed struct layouts and mid-function patches in this fork are
// audited for that build only, so refuse to install hooks on anything else
// instead of crashing inside the game.
static void ShowUnsupportedVersionError(const std::string& acDetectedVersion)
{
    const std::wstring detected(acDetectedVersion.begin(), acDetectedVersion.end());
    auto errorDetail = fmt::format(L"Detected game version: {}. RFAB Online supports Skyrim SE 1.5.97 (SKSE64 2.0.20) only. Anniversary Edition (1.6.x) is not supported.", detected);

    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Unsupported Skyrim Special Edition version", L"RFAB Online requires Skyrim SE 1.5.97", errorDetail.c_str());
    dia.Show();

    exit(5);
}

void RunTiltedInit(const std::filesystem::path& acGamePath, const String& aExeVersion)
{
    if (!VersionDb::Get().Load(acGamePath, aExeVersion))
    {
        ShowAddressLibraryError(acGamePath.c_str());
    }

    {
        int major = 0, minor = 0, revision = 0, build = 0;
        VersionDb::Get().GetLoadedVersion(major, minor, revision, build);

        if (major != 1 || minor != 5 || revision != 97)
        {
            ShowUnsupportedVersionError(VersionDb::Get().GetLoadedVersionString());
        }
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
