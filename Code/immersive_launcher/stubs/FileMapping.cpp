
#include <strsafe.h>

#include <MinHook.h>
#include <winternl.h>

#include <FunctionHook.hpp>
#include <TiltedCore/Initializer.hpp>

#include "DllBlocklist.h"
#include "TargetConfig.h"
#include "Utils/NtInternal.h"
#include "utils/Error.h"
#include "launcher.h"

// defined in MemoryLayout.cpp
extern bool IsThisExeAddress(const uint8_t* apAddress);
extern bool IsGameMemoryAddress(const uint8_t* apAddress);

extern "C" __declspec(dllimport) NTSTATUS WINAPI LdrGetDllFullName(HMODULE, PUNICODE_STRING);

extern "C" __declspec(dllexport) const wchar_t* GetTiltedClientDirW() noexcept;

namespace
{
std::wstring s_OverridePath;

DWORD(WINAPI* RealGetModuleFileNameW)(HMODULE, LPWSTR, DWORD) = nullptr;
DWORD(WINAPI* RealGetModuleFileNameA)(HMODULE, LPSTR, DWORD) = nullptr;
HMODULE(WINAPI* RealGetModuleHandleW)(LPCWSTR) = nullptr;
HMODULE(WINAPI* RealGetModuleHandleA)(LPCSTR) = nullptr;
NTSTATUS(WINAPI* RealLdrLoadDll)(const wchar_t*, uint32_t*, UNICODE_STRING*, HANDLE*) = nullptr;
NTSTATUS(WINAPI* RealLdrGetDllHandle)(PWSTR, PULONG, PUNICODE_STRING, PVOID*) = nullptr;
NTSTATUS(WINAPI* RealLdrGetDllFullName)(HMODULE, PUNICODE_STRING) = nullptr;
NTSTATUS(WINAPI* RealLdrGetDllHandleEx)
(ULONG Flags, PWSTR DllPath, PULONG DllCharacteristics, UNICODE_STRING* DllName, PVOID* DllHandle) = nullptr;

DWORD(WINAPI* RealGetFileVersionInfoSizeW)(LPCWSTR, LPDWORD) = nullptr;
BOOL(WINAPI* RealGetFileVersionInfoW)(LPCWSTR, DWORD, DWORD, LPVOID) = nullptr;
DWORD(WINAPI* RealGetFileVersionInfoSizeExW)(DWORD, LPCWSTR, LPDWORD) = nullptr;
BOOL(WINAPI* RealGetFileVersionInfoExW)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID) = nullptr;

bool IsSkyrimExeTargetQuery(LPCWSTR lptstrFilename, std::wstring& aOutRedirectPath) noexcept
{
    if (!lptstrFilename || !*lptstrFilename)
        return false;

    std::wstring_view sv(lptstrFilename);
    const size_t slash = sv.find_last_of(L"/\\");
    const std::wstring_view leaf = (slash == std::wstring_view::npos) ? sv : sv.substr(slash + 1);

    if (_wcsicmp(std::wstring(leaf).c_str(), L"SkyrimSE.exe") != 0 &&
        _wcsicmp(std::wstring(leaf).c_str(), L"SkyrimTogether.exe") != 0)
    {
        return false;
    }

    // If sibling SkyrimSE.cdx exists in game directory (standalone/CODEX repack where SkyrimSE.exe stub has 1.0.0.0),
    // prefer reading version block from SkyrimSE.cdx or our own 1.5.97.0 launcher resource!
    std::error_code ec;
    if (auto* LC = launcher::GetLaunchContext(); LC && !LC->gamePath.empty())
    {
        const auto cdx = LC->gamePath / L"SkyrimSE.cdx";
        if (std::filesystem::exists(cdx, ec))
        {
            aOutRedirectPath = cdx.wstring();
            return true;
        }
    }

    if (!s_OverridePath.empty())
    {
        const std::filesystem::path selfExe = std::filesystem::path(s_OverridePath) / L"SkyrimSE.exe";
        if (std::filesystem::exists(selfExe, ec))
        {
            aOutRedirectPath = selfExe.wstring();
            return true;
        }
    }

    aOutRedirectPath = lptstrFilename;
    return true;
}

void PatchFixedVersionTo1597(LPVOID lpData) noexcept
{
    if (!lpData)
        return;

    VS_FIXEDFILEINFO* pFixed = nullptr;
    UINT uLen = 0;
    if (VerQueryValueW(lpData, L"\\", reinterpret_cast<LPVOID*>(&pFixed), &uLen) &&
        pFixed && uLen >= sizeof(VS_FIXEDFILEINFO) && pFixed->dwSignature == 0xFEEF04BD)
    {
        pFixed->dwFileVersionMS = 0x00010005;    // 1.5
        pFixed->dwFileVersionLS = 0x00610000;    // 97.0
        pFixed->dwProductVersionMS = 0x00010005; // 1.5
        pFixed->dwProductVersionLS = 0x00610000; // 97.0
    }
}

DWORD WINAPI TP_GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle)
{
    std::wstring redirected;
    if (IsSkyrimExeTargetQuery(lptstrFilename, redirected))
        return RealGetFileVersionInfoSizeW(redirected.c_str(), lpdwHandle);

    return RealGetFileVersionInfoSizeW(lptstrFilename, lpdwHandle);
}

BOOL WINAPI TP_GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    std::wstring redirected;
    if (IsSkyrimExeTargetQuery(lptstrFilename, redirected))
    {
        const BOOL ok = RealGetFileVersionInfoW(redirected.c_str(), dwHandle, dwLen, lpData);
        if (ok)
            PatchFixedVersionTo1597(lpData);
        return ok;
    }

    return RealGetFileVersionInfoW(lptstrFilename, dwHandle, dwLen, lpData);
}

DWORD WINAPI TP_GetFileVersionInfoSizeExW(DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle)
{
    std::wstring redirected;
    if (IsSkyrimExeTargetQuery(lpwstrFilename, redirected))
        return RealGetFileVersionInfoSizeExW(dwFlags, redirected.c_str(), lpdwHandle);

    return RealGetFileVersionInfoSizeExW(dwFlags, lpwstrFilename, lpdwHandle);
}

BOOL WINAPI TP_GetFileVersionInfoExW(DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    std::wstring redirected;
    if (IsSkyrimExeTargetQuery(lpwstrFilename, redirected))
    {
        const BOOL ok = RealGetFileVersionInfoExW(dwFlags, redirected.c_str(), dwHandle, dwLen, lpData);
        if (ok)
            PatchFixedVersionTo1597(lpData);
        return ok;
    }

    return RealGetFileVersionInfoExW(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

inline bool IsUsingMO2()
{
    return GetModuleHandleW(L"usvfs_x64.dll");
}

bool IsMyModule(HMODULE aHmod)
{
    return aHmod == nullptr || aHmod == NtInternal::ThePeb()->pImageBase;
}

HMODULE HModFromAddress(void* apAddress)
{
    HMODULE pModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(apAddress), &pModule);

    return pModule;
}

DWORD MYGetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize)
{
    if (!nSize)
        return 0;

    if (nSize > 0x7FFF)
        nSize = 0x7FFF;

    UNICODE_STRING str{};
    str.Buffer = lpFilename;
    str.MaximumLength = static_cast<USHORT>(sizeof(wchar_t) * nSize - sizeof(wchar_t));

    LdrGetDllFullName(hModule, &str);

    DWORD nChars = str.Length / sizeof(wchar_t);
    str.Buffer[nChars] = 0;
    return nChars;
}

bool IsLocalModulePath(HMODULE aHmod)
{
    std::wstring buf;

    size_t i = 1;
    DWORD res = 0;
    do
    {
        buf.resize(i * MAX_PATH);
        res = MYGetModuleFileNameW(aHmod, buf.data(), static_cast<DWORD>(buf.length()));
        i++;
    } while (res == ERROR_INSUFFICIENT_BUFFER);

    // does the file exist in the ST dir?
    return buf.find(s_OverridePath) != std::wstring::npos;
}

bool IsSkyrimMainModuleQueryW(std::wstring_view aModuleName) noexcept
{
    if (aModuleName.empty())
        return false;

    const size_t slash = aModuleName.find_last_of(L"/\\");
    const std::wstring_view leaf = (slash == std::wstring_view::npos) ? aModuleName : aModuleName.substr(slash + 1);

    constexpr std::wstring_view kExe = TARGET_NAME L".exe";
    constexpr std::wstring_view kStem = TARGET_NAME;
    constexpr std::wstring_view kAltExe = L"SkyrimTogether.exe";
    constexpr std::wstring_view kAltStem = L"SkyrimTogether";

    auto equalsIgnoreCaseW = [](std::wstring_view a, std::wstring_view b) noexcept -> bool
    {
        return a.size() == b.size() && _wcsnicmp(a.data(), b.data(), a.size()) == 0;
    };

    return equalsIgnoreCaseW(leaf, kExe) ||
           equalsIgnoreCaseW(leaf, kStem) ||
           equalsIgnoreCaseW(leaf, kAltExe) ||
           equalsIgnoreCaseW(leaf, kAltStem);
}

bool IsSkyrimMainModuleQueryA(std::string_view aModuleName) noexcept
{
    if (aModuleName.empty())
        return false;

    const size_t slash = aModuleName.find_last_of("/\\");
    const std::string_view leaf = (slash == std::string_view::npos) ? aModuleName : aModuleName.substr(slash + 1);

    constexpr std::string_view kExe = TARGET_NAME_A ".exe";
    constexpr std::string_view kStem = TARGET_NAME_A;
    constexpr std::string_view kAltExe = "SkyrimTogether.exe";
    constexpr std::string_view kAltStem = "SkyrimTogether";

    auto equalsIgnoreCaseA = [](std::string_view a, std::string_view b) noexcept -> bool
    {
        return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
    };

    return equalsIgnoreCaseA(leaf, kExe) ||
           equalsIgnoreCaseA(leaf, kStem) ||
           equalsIgnoreCaseA(leaf, kAltExe) ||
           equalsIgnoreCaseA(leaf, kAltStem);
}

// some mods do GetModuleHandle("SkyrimSE.exe") for some reason instead of GetModuleHandle(nullptr)
HMODULE WINAPI TP_GetModuleHandleW(LPCWSTR lpModuleName)
{
    if (lpModuleName && IsSkyrimMainModuleQueryW(lpModuleName))
        lpModuleName = nullptr;
    return RealGetModuleHandleW(lpModuleName);
}

// some mods do GetModuleHandle("SkyrimSE.exe") for some reason instead of GetModuleHandle(nullptr)
HMODULE WINAPI TP_GetModuleHandleA(LPCSTR lpModuleName)
{
    if (lpModuleName && IsSkyrimMainModuleQueryA(lpModuleName))
        lpModuleName = nullptr;
    return RealGetModuleHandleA(lpModuleName);
}

// some mods do GetModuleHandle("SkyrimSE.exe") for some reason instead of GetModuleHandle(nullptr)
NTSTATUS WINAPI TP_LdrGetDllHandle(PWSTR DllPath, PULONG DllCharacteristics, PUNICODE_STRING DllName, PVOID* DllHandle)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    if (DllName && DllName->Buffer && DllHandle &&
        IsSkyrimMainModuleQueryW(std::wstring_view(DllName->Buffer, DllName->Length / sizeof(wchar_t))))
    {
        *DllHandle = NtInternal::ThePeb()->pImageBase;
        return 0; // success
    }

    return RealLdrGetDllHandle(DllPath, DllCharacteristics, DllName, DllHandle);
}

NTSTATUS WINAPI TP_LdrGetDllHandleEx(ULONG Flags, PWSTR DllPath, PULONG DllCharacteristics, UNICODE_STRING* DllName, PVOID* DllHandle)
{
    if (DllName && DllName->Buffer && DllHandle &&
        IsSkyrimMainModuleQueryW(std::wstring_view(DllName->Buffer, DllName->Length / sizeof(wchar_t))))
    {
        *DllHandle = NtInternal::ThePeb()->pImageBase;
        return 0; // success
    }

    return RealLdrGetDllHandleEx(Flags, DllPath, DllCharacteristics, DllName, DllHandle);
}

NTSTATUS WINAPI TP_LdrGetDllFullName(HMODULE Module, PUNICODE_STRING DllName)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    if (launcher::GetLaunchContext())
    {
        if (Module == nullptr || Module == NtInternal::ThePeb()->pImageBase)
        {
            auto& aExePath = launcher::GetLaunchContext()->exePath;
            RtlInitUnicodeString(DllName, aExePath.c_str());
            return 0; // sucess
        }
    }

    return RealLdrGetDllFullName(Module, DllName);
}

DWORD WINAPI TP_GetModuleFileNameW(HMODULE aModule, LPWSTR alpFilename, DWORD aSize)
{
    // trampoline space for USVFS
    TP_EMPTY_HOOK_PLACEHOLDER;

    if (IsMyModule(aModule) && launcher::GetLaunchContext() && launcher::GetLaunchContext()->GetLoaded())
    {
        auto& aExePath = launcher::GetLaunchContext()->exePath;
        StringCchCopyW(alpFilename, aSize, aExePath.c_str());

        return static_cast<DWORD>(std::wcslen(alpFilename));
    }

    return RealGetModuleFileNameW(aModule, alpFilename, aSize);
}

struct ScopedOSHeapItem
{
    ScopedOSHeapItem(size_t aSize) { m_pBlock = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, aSize); }

    ~ScopedOSHeapItem()
    {
        if (m_pBlock)
            HeapFree(GetProcessHeap(), 0, m_pBlock);
    }

    void* m_pBlock;
};

// NOTE(Vince): Introduce one layer of indirection by calling GetModuleFileNameW function directly, to trigger usvfs
// indirection.
DWORD WINAPI TP_GetModuleFileNameA(HMODULE aModule, char* alpFileName, DWORD aBufferSize)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    ScopedOSHeapItem wideBuffer((aBufferSize * sizeof(wchar_t)) + 1);

    wchar_t* pBuffer = static_cast<wchar_t*>(wideBuffer.m_pBlock);

    DWORD result = 0;
    HMODULE tMod = RealGetModuleHandleW(L"XAudio2_7.dll");
    if (tMod == nullptr || aModule != tMod)
        result = TP_GetModuleFileNameW(aModule, pBuffer, aBufferSize * sizeof(wchar_t)); // To make sure spoofing happens if needed

    if (result == 0)
    {
        return result;
    }

    UNICODE_STRING source{};
    RtlInitUnicodeString(&source, pBuffer);

    // convert using the proper OS function
    ANSI_STRING dest{.Length = static_cast<USHORT>(result), .MaximumLength = static_cast<USHORT>(aBufferSize), .Buffer = alpFileName};
    if (RtlUnicodeStringToAnsiString(&dest, &source, FALSE) != 0)
        return 0;

    return static_cast<DWORD>(dest.Length);
}

// we use this function to enforce the DLL load policy
NTSTATUS WINAPI TP_LdrLoadDll(const wchar_t* apPath, uint32_t* apFlags, UNICODE_STRING* apFileName, HANDLE* apHandle)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    std::wstring_view fileName(apFileName->Buffer, apFileName->Length / sizeof(wchar_t));
    size_t pos = fileName.find_last_of(L'\\');
    if (pos != std::wstring_view::npos && (pos + 1) != fileName.length())
    {
        const wchar_t *name = &fileName[pos + 1];
        if (stubs::IsSoulsRE(name))
            stubs::g_IsSoulsREActive = true;
        
        if (stubs::IsDllBlocked(name))
        {
            // invalid image hash
            // this signals windows to *NOT TRY* loading it again at a later time.
            return 0xC0000428;
        }
    }

    return RealLdrLoadDll(apPath, apFlags, apFileName, apHandle);
}
} // namespace

extern "C" __declspec(dllexport) const wchar_t* GetTiltedClientDirW() noexcept
{
    return s_OverridePath.c_str();
}

#define VALIDATE(x) \
    if (x != MH_OK) \
        Die(L"CoreStubsInit(): Fatal Minhook error.", true);

void CoreStubsInit()
{
    s_OverridePath = TiltedPhoques::GetPath().wstring();
    std::replace(s_OverridePath.begin(), s_OverridePath.end(), L'/', L'\\');
    SetEnvironmentVariableW(L"RFAB_TILTED_CLIENT_DIR", s_OverridePath.c_str());

    VALIDATE(MH_Initialize());

    if (!IsUsingMO2())
    {
        VALIDATE(MH_CreateHookApi(L"KernelBase.dll", "GetModuleFileNameW", &TP_GetModuleFileNameW, (void**)&RealGetModuleFileNameW));
        VALIDATE(MH_CreateHookApi(L"KernelBase.dll", "GetModuleFileNameA", &TP_GetModuleFileNameA, (void**)&RealGetModuleFileNameA));
    }
    else
    {
        VALIDATE(MH_CreateHookApi(L"usvfs_x64.dll", "?hook_GetModuleFileNameW@usvfs@@YAKPEAUHINSTANCE__@@PEA_WK@Z", &TP_GetModuleFileNameW, (void**)&RealGetModuleFileNameW));
        VALIDATE(MH_CreateHookApi(L"KernelBase.dll", "GetModuleFileNameA", &TP_GetModuleFileNameA, (void**)&RealGetModuleFileNameA));
    }

    VALIDATE(MH_CreateHookApi(L"ntdll.dll", "LdrGetDllHandleEx", &TP_LdrGetDllHandleEx, (void**)&RealLdrGetDllHandleEx));
    VALIDATE(MH_CreateHookApi(L"ntdll.dll", "LdrLoadDll", &TP_LdrLoadDll, (void**)&RealLdrLoadDll));

    VALIDATE(MH_CreateHookApi(L"kernel32.dll", "GetModuleHandleW", &TP_GetModuleHandleW, (void**)&RealGetModuleHandleW));
    VALIDATE(MH_CreateHookApi(L"kernel32.dll", "GetModuleHandleA", &TP_GetModuleHandleA, (void**)&RealGetModuleHandleA));

    // Spoof VS_FIXEDFILEINFO (1.5.97.0) for any SKSE / CommonLibSSE / CommonLibSSE-NG plugin querying SkyrimSE.exe
    // even when game root SkyrimSE.exe is a standalone/CODEX stub whose disk header carries FILEVERSION 1.0.0.0!
    MH_CreateHookApi(L"KernelBase.dll", "GetFileVersionInfoSizeW", &TP_GetFileVersionInfoSizeW, (void**)&RealGetFileVersionInfoSizeW);
    MH_CreateHookApi(L"KernelBase.dll", "GetFileVersionInfoW", &TP_GetFileVersionInfoW, (void**)&RealGetFileVersionInfoW);
    MH_CreateHookApi(L"KernelBase.dll", "GetFileVersionInfoSizeExW", &TP_GetFileVersionInfoSizeExW, (void**)&RealGetFileVersionInfoSizeExW);
    MH_CreateHookApi(L"KernelBase.dll", "GetFileVersionInfoExW", &TP_GetFileVersionInfoExW, (void**)&RealGetFileVersionInfoExW);

    VALIDATE(MH_EnableHook(nullptr));
}

#undef VALIDATE

void SpoofPebMainModuleName(const std::filesystem::path& acExePath)
{
    static std::wstring s_spoofFullPath;
    static std::wstring s_spoofBaseName;
    s_spoofFullPath = acExePath.wstring();
    s_spoofBaseName = acExePath.filename().wstring();

    auto* pPeb = reinterpret_cast<PEB*>(NtInternal::ThePeb());
    if (!pPeb || !pPeb->Ldr)
        return;

    LIST_ENTRY* pHead = &pPeb->Ldr->InMemoryOrderModuleList;
    for (LIST_ENTRY* pEntry = pHead->Flink; pEntry && pEntry != pHead; pEntry = pEntry->Flink)
    {
        auto* pMod = CONTAINING_RECORD(pEntry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (pMod->DllBase == NtInternal::ThePeb()->pImageBase)
        {
            RtlInitUnicodeString(&pMod->FullDllName, s_spoofFullPath.c_str());
            auto* pBaseDllName = reinterpret_cast<UNICODE_STRING*>(
                reinterpret_cast<uint8_t*>(&pMod->FullDllName) + sizeof(UNICODE_STRING));
            RtlInitUnicodeString(pBaseDllName, s_spoofBaseName.c_str());
            break;
        }
    }
}
