
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
extern "C" const wchar_t* GetTiltedClientDirW() noexcept;

namespace
{
std::filesystem::path s_pendingGamePath;
String s_pendingExeVersion;
std::once_flag s_tiltedInitOnce;
std::once_flag s_crashHandlerOnce;
thread_local bool s_inCrashLogger = false;

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

void FormatAddrWithModulePod(uintptr_t aAddr, char* apOut, size_t aOutSz) noexcept
{
    if (!aOutSz)
        return;
    apOut[0] = '\0';

    HMODULE hMod = nullptr;
    char modBuf[MAX_PATH]{};
    if (aAddr >= 0x10000 &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(aAddr), &hMod) &&
        hMod != nullptr)
    {
        GetModuleFileNameA(hMod, modBuf, MAX_PATH - 1);
        const char* slash = std::strrchr(modBuf, '\\');
        const char* shortName = slash ? (slash + 1) : modBuf;
        const uintptr_t rva = aAddr - reinterpret_cast<uintptr_t>(hMod);
        std::snprintf(apOut, aOutSz, "%s+0x%llX (0x%llX)",
                      shortName,
                      static_cast<unsigned long long>(rva),
                      static_cast<unsigned long long>(aAddr));
        return;
    }

    std::snprintf(apOut, aOutSz, "0x%llX", static_cast<unsigned long long>(aAddr));
}

// Windows x64 MSVC __CxxThrowException ABI layout (frozen across all x64 MSVC versions since VS2005)
struct MsvcX64TypeDescriptor
{
    const void* pVFTable;
    void* spare;
    char name[1];
};
struct MsvcX64PMD
{
    int mdisp;
    int pdisp;
    int vdisp;
};
struct MsvcX64CatchableType
{
    unsigned int properties;
    int pTypeRva;
    MsvcX64PMD thisDisplacement;
    int sizeOrOffset;
    int copyFunctionRva;
};
struct MsvcX64CatchableTypeArray
{
    int nCatchableTypes;
    int arrayOfCatchableTypesRva[1];
};
struct MsvcX64ThrowInfo
{
    unsigned int attributes;
    int pmfnUnwindRva;
    int pForwardCompatRva;
    int pCatchableTypeArrayRva;
};

// Pure POD helper (zero C++ destructors -> safe with MSVC __try / __except without C2712!)
void DecodeCppExceptionAndStackPod(PEXCEPTION_POINTERS apInfo, char* apCppBuf, size_t aCppSz, char* apStackBuf, size_t aStackSz) noexcept
{
    apCppBuf[0] = '\0';
    apStackBuf[0] = '\0';
    if (!apInfo || !apInfo->ExceptionRecord)
        return;

    __try
    {
        const EXCEPTION_RECORD* rec = apInfo->ExceptionRecord;
        if (rec->ExceptionCode == 0xE06D7363 && rec->NumberParameters >= 4)
        {
            const uintptr_t magic = rec->ExceptionInformation[0];
            const uintptr_t pObj = rec->ExceptionInformation[1];
            const uintptr_t pThrowInfoRaw = rec->ExceptionInformation[2];
            const uintptr_t imgBase = rec->ExceptionInformation[3];

            char originMod[192]{};
            FormatAddrWithModulePod(imgBase, originMod, sizeof(originMod));

            const char* rttiName = "<unknown>";
            const char* whatText = "";

            if (pThrowInfoRaw && imgBase)
            {
                const auto* pTI = reinterpret_cast<const MsvcX64ThrowInfo*>(pThrowInfoRaw);
                if (pTI->pCatchableTypeArrayRva)
                {
                    const auto* pCTA = reinterpret_cast<const MsvcX64CatchableTypeArray*>(imgBase + pTI->pCatchableTypeArrayRva);
                    if (pCTA->nCatchableTypes > 0)
                    {
                        const auto* pCT0 = reinterpret_cast<const MsvcX64CatchableType*>(imgBase + pCTA->arrayOfCatchableTypesRva[0]);
                        if (pCT0 && pCT0->pTypeRva)
                        {
                            const auto* pTD0 = reinterpret_cast<const MsvcX64TypeDescriptor*>(imgBase + pCT0->pTypeRva);
                            if (pTD0 && pTD0->name[0])
                                rttiName = pTD0->name;
                        }
                    }

                    for (int i = 0; i < pCTA->nCatchableTypes && i < 16; ++i)
                    {
                        const auto* pCT = reinterpret_cast<const MsvcX64CatchableType*>(imgBase + pCTA->arrayOfCatchableTypesRva[i]);
                        if (!pCT || !pCT->pTypeRva)
                            continue;
                        const auto* pTD = reinterpret_cast<const MsvcX64TypeDescriptor*>(imgBase + pCT->pTypeRva);
                        if (pTD && pTD->name[0] && std::strstr(pTD->name, "exception@std@@"))
                        {
                            const auto* pEx = reinterpret_cast<const std::exception*>(pObj + pCT->thisDisplacement.mdisp);
                            if (pEx)
                            {
                                const char* w = pEx->what();
                                if (w)
                                    whatText = w;
                            }
                            break;
                        }
                    }
                }
            }

            std::snprintf(apCppBuf, aCppSz,
                          " | CppThrowOrigin=[%s] Magic=0x%llX Type=[%s] what()=[\"%s\"]",
                          originMod,
                          static_cast<unsigned long long>(magic),
                          rttiName,
                          whatText);
        }

        if (apInfo->ContextRecord)
        {
            size_t pos = 0;
            int unwound = 0;

            CONTEXT unwindCtx = *apInfo->ContextRecord;
            for (int depth = 0; depth < 16 && pos + 112 < aStackSz; ++depth)
            {
                const DWORD64 pc = unwindCtx.Rip;
                if (pc < 0x10000)
                    break;

                char frameDesc[160]{};
                FormatAddrWithModulePod(static_cast<uintptr_t>(pc), frameDesc, sizeof(frameDesc));
                const int n = std::snprintf(apStackBuf + pos, aStackSz - pos, "\r\n    [unwind #%d] %s", unwound, frameDesc);
                if (n > 0)
                    pos += static_cast<size_t>(n);
                ++unwound;

                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION pRuntimeFn = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
                if (!pRuntimeFn)
                {
                    if (depth == 0 && unwindCtx.Rsp >= 0x10000)
                    {
                        unwindCtx.Rip = *reinterpret_cast<const DWORD64*>(unwindCtx.Rsp);
                        unwindCtx.Rsp += 8;
                        continue;
                    }
                    break;
                }

                PVOID handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(
                    UNW_FLAG_NHANDLER,
                    imageBase,
                    pc,
                    pRuntimeFn,
                    &unwindCtx,
                    &handlerData,
                    &establisherFrame,
                    nullptr);

                if (unwindCtx.Rip == 0 || unwindCtx.Rsp == 0)
                    break;
            }

            if (unwound < 3)
            {
                const auto* sp = reinterpret_cast<const uintptr_t*>(apInfo->ContextRecord->Rsp);
                int found = 0;
                for (int slot = 0; slot < 96 && found < 8 && pos + 96 < aStackSz; ++slot)
                {
                    const uintptr_t cand = sp[slot];
                    HMODULE hCand = nullptr;
                    if (cand >= 0x10000 &&
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCSTR>(cand), &hCand) &&
                        hCand != nullptr)
                    {
                        char frameDesc[160]{};
                        FormatAddrWithModulePod(cand, frameDesc, sizeof(frameDesc));
                        const int n = std::snprintf(apStackBuf + pos, aStackSz - pos, "\r\n    [stack #%d] %s", found, frameDesc);
                        if (n > 0)
                            pos += static_cast<size_t>(n);
                        ++found;
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
} // namespace

void WriteBootTrace(const char* apMsg, bool aReset = false)
{
    if (!apMsg)
        return;

    const wchar_t* pClientDir = GetTiltedClientDirW();
    const std::filesystem::path clientPath = (pClientDir && *pClientDir)
                                                 ? std::filesystem::path(pClientDir)
                                                 : TiltedPhoques::GetPath();

    WriteBootTraceRaw(clientPath / L"RFAB_Online_Startup.log", apMsg, aReset);
    if (!s_pendingGamePath.empty() && s_pendingGamePath != clientPath)
        WriteBootTraceRaw(s_pendingGamePath / L"RFAB_Online_Startup.log", apMsg, aReset);
}

void LogUnhandledCrashToBootFile(PEXCEPTION_POINTERS apInfo) noexcept
{
    if (s_inCrashLogger || !apInfo || !apInfo->ExceptionRecord || !apInfo->ContextRecord)
        return;

    // Ignore benign Win32 informational codes (OutputDebugStringA / thread naming)
    if (apInfo->ExceptionRecord->ExceptionCode < 0x80000000)
        return;

    s_inCrashLogger = true;

    const DWORD code = apInfo->ExceptionRecord->ExceptionCode;
    const uintptr_t rip = apInfo->ContextRecord->Rip;

    HMODULE hMod = nullptr;
    wchar_t modPath[MAX_PATH]{};
    uintptr_t modBase = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(rip), &hMod) &&
        hMod)
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

    char cppInfo[1024]{};
    char stackInfo[2048]{};
    DecodeCppExceptionAndStackPod(apInfo, cppInfo, sizeof(cppInfo), stackInfo, sizeof(stackInfo));

    SYSTEMTIME stLocal{};
    GetLocalTime(&stLocal);
    const DWORD tid = GetCurrentThreadId();
    const CONTEXT* ctx = apInfo->ContextRecord;

    char crashBuf[4608];
    std::snprintf(crashBuf, sizeof(crashBuf),
                  "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [UNHANDLED_CRASH] TID=%lu Code=0x%08lX RIP=0x%llX FaultAddr=0x%llX Module='%s' Base=0x%llX RVA=+0x%llX "
                  "RSP=0x%llX RBP=0x%llX RAX=0x%llX RBX=0x%llX RCX=0x%llX RDX=0x%llX RSI=0x%llX RDI=0x%llX "
                  "R8=0x%llX R9=0x%llX R10=0x%llX R11=0x%llX R12=0x%llX R13=0x%llX R14=0x%llX R15=0x%llX Bytes=[%s]%s%s",
                  static_cast<unsigned>(stLocal.wYear),
                  static_cast<unsigned>(stLocal.wMonth),
                  static_cast<unsigned>(stLocal.wDay),
                  static_cast<unsigned>(stLocal.wHour),
                  static_cast<unsigned>(stLocal.wMinute),
                  static_cast<unsigned>(stLocal.wSecond),
                  static_cast<unsigned>(stLocal.wMilliseconds),
                  static_cast<unsigned long>(tid),
                  static_cast<unsigned long>(code),
                  static_cast<unsigned long long>(rip),
                  static_cast<unsigned long long>(faultAddr),
                  modUtf8,
                  static_cast<unsigned long long>(modBase),
                  static_cast<unsigned long long>(modBase ? (rip - modBase) : rip),
                  static_cast<unsigned long long>(ctx->Rsp),
                  static_cast<unsigned long long>(ctx->Rbp),
                  static_cast<unsigned long long>(ctx->Rax),
                  static_cast<unsigned long long>(ctx->Rbx),
                  static_cast<unsigned long long>(ctx->Rcx),
                  static_cast<unsigned long long>(ctx->Rdx),
                  static_cast<unsigned long long>(ctx->Rsi),
                  static_cast<unsigned long long>(ctx->Rdi),
                  static_cast<unsigned long long>(ctx->R8),
                  static_cast<unsigned long long>(ctx->R9),
                  static_cast<unsigned long long>(ctx->R10),
                  static_cast<unsigned long long>(ctx->R11),
                  static_cast<unsigned long long>(ctx->R12),
                  static_cast<unsigned long long>(ctx->R13),
                  static_cast<unsigned long long>(ctx->R14),
                  static_cast<unsigned long long>(ctx->R15),
                  hexBytes,
                  cppInfo,
                  stackInfo);
    WriteBootTrace(crashBuf, false);

    // Also persist to append-only RFAB_Online_Crash.log so restarting Skyrim never overwrites prior crash records!
    const wchar_t* pClientDir = GetTiltedClientDirW();
    const std::filesystem::path clientPath = (pClientDir && *pClientDir)
                                                 ? std::filesystem::path(pClientDir)
                                                 : TiltedPhoques::GetPath();
    WriteBootTraceRaw(clientPath / L"RFAB_Online_Crash.log", crashBuf, false);
    if (!s_pendingGamePath.empty() && s_pendingGamePath != clientPath)
        WriteBootTraceRaw(s_pendingGamePath / L"RFAB_Online_Crash.log", crashBuf, false);

    s_inCrashLogger = false;
}

static LONG WINAPI EarlyUnhandledFilter(EXCEPTION_POINTERS* apInfo) noexcept
{
    LogUnhandledCrashToBootFile(apInfo);
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallRfabBootDiagnostics(const std::filesystem::path& acGamePath, const String& aExeVersion)
{
    if (!acGamePath.empty())
        s_pendingGamePath = acGamePath;
    if (!aExeVersion.empty())
        s_pendingExeVersion = aExeVersion;

    bool firstInit = false;
    std::call_once(s_crashHandlerOnce, [&]() {
        firstInit = true;
        SetUnhandledExceptionFilter(&EarlyUnhandledFilter);
        AddVectoredContinueHandler(1, &EarlyUnhandledFilter);
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

    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Unsupported Skyrim version",
                         L"RFAB Online only supports Skyrim Special Edition 1.5.97 with SKSE64 2.0.20", errorDetail.c_str());
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

        WriteBootTrace("[BOOT 3/4] Address Library SE 1.5.97 loaded; constructing TiltedOnlineApp...");
        g_appInstance = std::make_unique<TiltedOnlineApp>();
        WriteBootTrace("[BOOT 3.1/4] TiltedOnlineApp constructed; calling InstallHooks2()...");

        TiltedOnlineApp::InstallHooks2();
        WriteBootTrace("[BOOT 3.3/4] Committing delayed .text hooks...");
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
