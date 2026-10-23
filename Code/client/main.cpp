
#include <TiltedOnlineApp.h>
#include <TiltedOnlinePCH.h>

#include <Commctrl.h>
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <base/dialogues/win/TaskDialog.h>

std::unique_ptr<TiltedOnlineApp> g_appInstance{nullptr};

extern HICON g_SharedWindowIcon;

namespace
{
std::filesystem::path s_pendingGamePath;
String s_pendingExeVersion;
std::once_flag s_tiltedInitOnce;
std::once_flag s_crashHandlerOnce;
thread_local bool s_inCrashLogger = false;
int s_loggedExceptions = 0;

void WriteBootTraceRaw(const std::filesystem::path& aPath, const char* apMsg, bool aReset) noexcept
{
    if (aPath.empty() || !apMsg)
        return;

    HANDLE hFile = CreateFileW(aPath.c_str(),
                               aReset ? GENERIC_WRITE : FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr,
                               aReset ? CREATE_ALWAYS : OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(hFile, apMsg, static_cast<DWORD>(std::strlen(apMsg)), &written, nullptr);
        WriteFile(hFile, "\r\n", 2, &written, nullptr);
        FlushFileBuffers(hFile);
        CloseHandle(hFile);
    }
}

void WriteBootTrace(const char* apMsg, bool aReset = false) noexcept
{
    if (!apMsg)
        return;

    WriteBootTraceRaw(TiltedPhoques::GetPath() / L"RFAB_Online_Startup.log", apMsg, aReset);
    if (!s_pendingGamePath.empty())
        WriteBootTraceRaw(s_pendingGamePath / L"RFAB_Online_Startup.log", apMsg, aReset);
}

LONG WINAPI RfabStartupVectoredHandler(EXCEPTION_POINTERS* apInfo) noexcept
{
    if (s_inCrashLogger || !apInfo || !apInfo->ExceptionRecord || !apInfo->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = apInfo->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (s_loggedExceptions >= 16)
        return EXCEPTION_CONTINUE_SEARCH;

    s_inCrashLogger = true;
    ++s_loggedExceptions;

    const uintptr_t rip = apInfo->ContextRecord->Rip;

    HMODULE hMod = nullptr;
    wchar_t modPath[MAX_PATH]{};
    uintptr_t modBase = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(rip), &hMod) && hMod)
    {
        modBase = reinterpret_cast<uintptr_t>(hMod);
        GetModuleFileNameW(hMod, modPath, MAX_PATH - 1);
    }

    char modUtf8[MAX_PATH * 3]{};
    WideCharToMultiByte(CP_UTF8, 0, modPath, -1, modUtf8, sizeof(modUtf8) - 1, nullptr, nullptr);

    char hexBytes[128]{};
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(rip), &mbi, sizeof(mbi)) &&
        mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(rip);
        int pos = 0;
        for (int i = 0; i < 16 && pos < 110; ++i)
            pos += std::snprintf(hexBytes + pos, sizeof(hexBytes) - pos, "%02X ", bytes[i]);
    }

    const uintptr_t faultAddr = (apInfo->ExceptionRecord->NumberParameters >= 2)
                                    ? apInfo->ExceptionRecord->ExceptionInformation[1]
                                    : 0;

    char crashBuf[1536];
    std::snprintf(crashBuf, sizeof(crashBuf),
                  "[EXCEPTION #%d] Code=0x%08lX RIP=0x%llX FaultAddr=0x%llX Module='%s' Base=0x%llX RVA=+0x%llX RSP=0x%llX RAX=0x%llX RCX=0x%llX RDX=0x%llX Bytes=[%s]",
                  s_loggedExceptions,
                  static_cast<unsigned long>(code),
                  static_cast<unsigned long long>(rip),
                  static_cast<unsigned long long>(faultAddr),
                  modUtf8,
                  static_cast<unsigned long long>(modBase),
                  static_cast<unsigned long long>(modBase ? (rip - modBase) : rip),
                  static_cast<unsigned long long>(apInfo->ContextRecord->Rsp),
                  static_cast<unsigned long long>(apInfo->ContextRecord->Rax),
                  static_cast<unsigned long long>(apInfo->ContextRecord->Rcx),
                  static_cast<unsigned long long>(apInfo->ContextRecord->Rdx),
                  hexBytes);
    WriteBootTrace(crashBuf, false);

    s_inCrashLogger = false;
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void InstallRfabBootDiagnostics(const std::filesystem::path& acGamePath, const String& aExeVersion)
{
    if (!acGamePath.empty())
        s_pendingGamePath = acGamePath;
    if (!aExeVersion.empty())
        s_pendingExeVersion = aExeVersion;

    bool firstInit = false;
    std::call_once(s_crashHandlerOnce, [&]() {
        firstInit = true;
        AddVectoredExceptionHandler(1, RfabStartupVectoredHandler);
    });

    char msg[512];
    std::snprintf(msg, sizeof(msg), "[BOOT] Checkpoint | ExeVersion='%s' | GamePathSet=%d",
                  s_pendingExeVersion.c_str(), s_pendingGamePath.empty() ? 0 : 1);
    WriteBootTrace(msg, firstInit);
}

static void ShowAddressLibraryError(const wchar_t* apGamePath)
{
    WriteBootTrace("[FATAL] ShowAddressLibraryError triggered!");
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

static void ShowUnsupportedVersionError(const std::string& acDetectedVersion)
{
    WriteBootTrace("[FATAL] ShowUnsupportedVersionError triggered!");
    const std::wstring detected(acDetectedVersion.begin(), acDetectedVersion.end());
    auto errorDetail = fmt::format(L"Detected game version: {}. RFAB Online supports Skyrim SE 1.5.97 (SKSE64 2.0.20) only. Anniversary Edition (1.6.x) is not supported.", detected);

    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Unsupported Skyrim Special Edition version", L"RFAB Online requires Skyrim SE 1.5.97", errorDetail.c_str());
    dia.Show();

    exit(5);
}

void EnsureTiltedInitialized()
{
    std::call_once(s_tiltedInitOnce, []() {
        WriteBootTrace("[BOOT 2/4] EnsureTiltedInitialized entered (CRT post-unpack)");
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

        WriteBootTrace("[BOOT 3/4] Address Library SE 1.5.97 loaded; installing hooks...");
        g_appInstance = std::make_unique<TiltedOnlineApp>();

        TiltedOnlineApp::InstallHooks2();
        TP_HOOK_COMMIT;
        WriteBootTrace("[BOOT 3.5/4] All .text hooks committed successfully!");
    });
}

void RunTiltedInit(const std::filesystem::path& acGamePath, const String& aExeVersion)
{
    InstallRfabBootDiagnostics(acGamePath, aExeVersion);
    TP_HOOK_COMMIT;
}

void RunTiltedApp()
{
    EnsureTiltedInitialized();
    WriteBootTrace("[BOOT 4/4] Entering BeginMain() + StartSKSE()...");
    g_appInstance->BeginMain();
    WriteBootTrace("[BOOT OK] BeginMain() + StartSKSE() completed; entering WinMain!");
}
