#include "ConfigOverlayRedirect.h"

#include <common/Compat/ConfigOverlay.h>

#include <TiltedCore/Filesystem.hpp>

#include <MinHook.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace stubs
{
namespace
{
struct RedirectEntry
{
    std::string_view DataRelativePath;
    Compat::RuleKind Kind{Compat::RuleKind::kOverlay};
    std::wstring OverlayPath; // kOverlay only
};

// Written once by InitCoopConfigOverlay before the hooks are enabled, immutable afterwards, so the
// hooks (any thread, including the loader and the CLR) read it without locks.
std::array<RedirectEntry, Compat::kRuleCount> s_entries{};
size_t s_entryCount = 0;
std::atomic<bool> s_redirectActive{false};
std::atomic<bool> s_engineFixesAllowed{false};
std::string s_report;

using TCreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using TCreateFileA = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using TCreateFile2 = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
TCreateFileW RealCreateFileW = nullptr;
TCreateFileA RealCreateFileA = nullptr;
TCreateFile2 RealCreateFile2 = nullptr;

bool OpensForWriting(DWORD aAccess, DWORD aDisposition) noexcept
{
    constexpr DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC |
                                   WRITE_OWNER | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;
    return (aAccess & kWriteAccess) != 0 || aDisposition == CREATE_ALWAYS || aDisposition == CREATE_NEW ||
           aDisposition == TRUNCATE_EXISTING;
}

enum class Decision
{
    kPassThrough,
    kRedirect,
    kNotFound,
};

// Allocation-free; runs for every file the process opens while the redirect is active.
Decision Decide(LPCWSTR apPath, DWORD aAccess, DWORD aDisposition, const wchar_t*& aRedirectTo) noexcept
{
    if (!apPath || !s_redirectActive.load(std::memory_order_acquire))
        return Decision::kPassThrough;

    const std::wstring_view path(apPath);
    for (size_t i = 0; i < s_entryCount; ++i)
    {
        const RedirectEntry& entry = s_entries[i];
        if (!Compat::PathNamesDataFile(path, entry.DataRelativePath))
            continue;
        if (entry.Kind == Compat::RuleKind::kHide)
            return Decision::kNotFound;
        // A mod writing its own settings writes its real file; only reads see the co-op copy.
        if (OpensForWriting(aAccess, aDisposition))
            return Decision::kPassThrough;
        aRedirectTo = entry.OverlayPath.c_str();
        return Decision::kRedirect;
    }
    return Decision::kPassThrough;
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR apPath, DWORD aAccess, DWORD aShare, LPSECURITY_ATTRIBUTES apSecurity,
                               DWORD aDisposition, DWORD aFlags, HANDLE aTemplate)
{
    const wchar_t* pRedirect = nullptr;
    switch (Decide(apPath, aAccess, aDisposition, pRedirect))
    {
    case Decision::kNotFound: SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE;
    case Decision::kRedirect: return RealCreateFileW(pRedirect, aAccess, aShare, apSecurity, aDisposition, aFlags, aTemplate);
    default: return RealCreateFileW(apPath, aAccess, aShare, apSecurity, aDisposition, aFlags, aTemplate);
    }
}

HANDLE WINAPI Hook_CreateFile2(LPCWSTR apPath, DWORD aAccess, DWORD aShare, DWORD aDisposition,
                               LPCREATEFILE2_EXTENDED_PARAMETERS apParams)
{
    const wchar_t* pRedirect = nullptr;
    switch (Decide(apPath, aAccess, aDisposition, pRedirect))
    {
    case Decision::kNotFound: SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE;
    case Decision::kRedirect: return RealCreateFile2(pRedirect, aAccess, aShare, aDisposition, apParams);
    default: return RealCreateFile2(apPath, aAccess, aShare, aDisposition, apParams);
    }
}

HANDLE WINAPI Hook_CreateFileA(LPCSTR apPath, DWORD aAccess, DWORD aShare, LPSECURITY_ATTRIBUTES apSecurity,
                               DWORD aDisposition, DWORD aFlags, HANDLE aTemplate)
{
    if (apPath && s_redirectActive.load(std::memory_order_acquire))
    {
        // Cheap ASCII pre-check before converting: only names ending like a rule's file can match.
        const std::string_view narrow(apPath);
        bool candidate = false;
        for (size_t i = 0; i < s_entryCount && !candidate; ++i)
            candidate = Compat::PathNamesDataFile(narrow, s_entries[i].DataRelativePath);

        if (candidate)
        {
            wchar_t wide[MAX_PATH * 2];
            const UINT codePage = AreFileApisANSI() ? CP_ACP : CP_OEMCP;
            const int length = MultiByteToWideChar(codePage, 0, apPath, -1, wide, static_cast<int>(std::size(wide)));
            if (length > 0)
                return Hook_CreateFileW(wide, aAccess, aShare, apSecurity, aDisposition, aFlags, aTemplate);
        }
    }
    return RealCreateFileA(apPath, aAccess, aShare, apSecurity, aDisposition, aFlags, aTemplate);
}

std::filesystem::path FallbackOverlayDir() noexcept
{
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return std::filesystem::path(buffer) / L"RFAB Online" / L"coop_config";
}

bool AnyFailed(const std::vector<Compat::OverlayOutcome>& acOutcomes) noexcept
{
    for (const auto& outcome : acOutcomes)
        if (outcome.Status == Compat::OverlayStatus::kFailed)
            return true;
    return false;
}

bool InstallHooks(std::string& aError) noexcept
{
    // usvfs (MO2) virtualizes at ntdll!NtCreateFile/NtOpenFile, below these: our redirect target is
    // a real path outside Data, which usvfs passes through, and Data paths we do not match still
    // reach usvfs unchanged. CreateFile2 is optional (missing on old systems).
    struct HookSpec
    {
        const char* Name;
        void* Detour;
        void** ppOriginal;
        bool Required;
    };
    const HookSpec specs[] = {
        {"CreateFileW", reinterpret_cast<void*>(&Hook_CreateFileW), reinterpret_cast<void**>(&RealCreateFileW), true},
        {"CreateFileA", reinterpret_cast<void*>(&Hook_CreateFileA), reinterpret_cast<void**>(&RealCreateFileA), true},
        {"CreateFile2", reinterpret_cast<void*>(&Hook_CreateFile2), reinterpret_cast<void**>(&RealCreateFile2), false},
    };

    const HMODULE hKernelBase = GetModuleHandleW(L"KernelBase.dll");
    if (!hKernelBase)
    {
        aError = "KernelBase.dll not loaded";
        return false;
    }

    std::vector<void*> created;
    for (const auto& spec : specs)
    {
        void* pTarget = reinterpret_cast<void*>(GetProcAddress(hKernelBase, spec.Name));
        const MH_STATUS status = pTarget ? MH_CreateHook(pTarget, spec.Detour, spec.ppOriginal) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (status == MH_OK)
        {
            created.push_back(pTarget);
            continue;
        }
        if (!spec.Required)
            continue;
        aError = std::string("MH_CreateHook(") + spec.Name + ") failed: " + std::to_string(static_cast<int>(status));
        for (void* pDone : created)
            MH_RemoveHook(pDone);
        return false;
    }

    for (void* pTarget : created)
    {
        const MH_STATUS status = MH_EnableHook(pTarget);
        if (status != MH_OK)
        {
            aError = std::string("MH_EnableHook failed: ") + std::to_string(static_cast<int>(status));
            for (void* pDone : created)
            {
                MH_DisableHook(pDone);
                MH_RemoveHook(pDone);
            }
            return false;
        }
    }
    return true;
}

void WriteReport(const std::filesystem::path& acDir) noexcept
{
    std::error_code ec;
    std::filesystem::create_directories(acDir, ec);
    RfabCompat::Detail::WriteTextFile(acDir / L"coop_config_report.txt", s_report);
}
} // namespace

void InitCoopConfigOverlay(const std::filesystem::path& acGamePath) noexcept
{
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;

    try
    {
        const std::filesystem::path dataRoot = acGamePath / L"Data";
        std::filesystem::path overlayDir = TiltedPhoques::GetPath() / L"rfab_coop_config";
        std::vector<Compat::OverlayOutcome> outcomes = Compat::PrepareOverlays(dataRoot, overlayDir);
        if (AnyFailed(outcomes))
        {
            // e.g. the client folder is read-only (game under Program Files without MO2).
            const std::filesystem::path fallback = FallbackOverlayDir();
            if (!fallback.empty())
            {
                std::vector<Compat::OverlayOutcome> retry = Compat::PrepareOverlays(dataRoot, fallback);
                if (!AnyFailed(retry))
                {
                    outcomes = std::move(retry);
                    overlayDir = fallback;
                }
            }
        }

        s_entryCount = 0;
        for (const auto& outcome : outcomes)
        {
            if (!outcome.Redirects() || !outcome.pRule)
                continue;
            RedirectEntry& entry = s_entries[s_entryCount++];
            entry.DataRelativePath = outcome.pRule->DataRelativePath;
            entry.Kind = outcome.pRule->Kind;
            if (entry.Kind == Compat::RuleKind::kOverlay)
                entry.OverlayPath = outcome.Overlay.wstring();
        }

        bool installed = false;
        std::string hookError;
        if (s_entryCount == 0)
        {
            installed = true; // nothing to redirect: the mods already read what co-op needs
        }
        else if (InstallHooks(hookError))
        {
            s_redirectActive.store(true, std::memory_order_release);
            installed = true;
        }

        s_engineFixesAllowed.store(Compat::IsEngineFixesSafe(outcomes, installed), std::memory_order_release);
        s_report = Compat::FormatReport(outcomes, installed);
        if (!hookError.empty())
            s_report += "\nRedirect error: " + hookError + "\n";
        if (!s_engineFixesAllowed.load(std::memory_order_relaxed))
            s_report += "\nEngineFixes.dll will NOT load in co-op: its settings could not be made safe for Skyrim Together.\n";
        WriteReport(overlayDir);
    }
    catch (...)
    {
        s_redirectActive.store(false, std::memory_order_release);
        s_engineFixesAllowed.store(false, std::memory_order_release);
        s_report = "RFAB Online co-op config overlay: preparation threw; no redirect, EngineFixes.dll kept out.\n";
    }
}

const std::string& GetCoopConfigReport() noexcept
{
    return s_report;
}

bool IsEngineFixesAllowed() noexcept
{
    return s_engineFixesAllowed.load(std::memory_order_acquire);
}
} // namespace stubs
