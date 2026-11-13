#include <cwctype>

#include "ConfigOverlayRedirect.h"
#include "DllBlocklist.h"

namespace stubs
{
namespace
{
bool EqualsIgnoreCase(std::wstring_view aLhs, std::wstring_view aRhs) noexcept
{
    if (aLhs.size() != aRhs.size())
        return false;

    for (size_t i = 0; i < aLhs.size(); ++i)
    {
        if (std::towlower(aLhs[i]) != std::towlower(aRhs[i]))
            return false;
    }

    return true;
}
} // namespace

// clang-format off

    // A list of modules blocked for a variety of reasons, but mainly
    // for causing crashes and incompatibility with ST
    const wchar_t* const kDllBlocklist[] = {
        L"crashhandler64.dll",       // Steam crash handler, breaks heap
        L"fraps64.dll",              // Breaks tilted ui
        L"SpecialK64.dll",           // breaks rendering
        L"ReShade64_SpecialK64.dll", // same reason
        L"NvCamera64.dll",           // broken af nvidia stuff, blacklisted for now, needs fix later
        // NetScriptFramework plugin patching the SE 36525 / 19282 add-item prologues. NSF loads it as a
        // managed assembly (never through LdrLoadDll), so the effective switch is the co-op config
        // overlay's "betterstealing" hide rule; this entry only covers a native/mixed-mode build.
        L"BetterStealing.dll",
       // L"atiuxp64.dll",
       // L"aticfx64.dll"
    };

// clang-format on

bool IsDllBlocked(std::wstring_view dllName)
{
    // EngineFixes is welcome as long as what it reads is safe for Skyrim Together's allocator: the
    // co-op config overlay decided that before the game loaded (no I/O here - this runs inside the
    // loader lock).
    if (EqualsIgnoreCase(dllName, L"EngineFixes.dll"))
        return !IsEngineFixesAllowed();

    for (const wchar_t* dllEntry : kDllBlocklist)
    {
        if (EqualsIgnoreCase(dllName, dllEntry))
            return true;
    }

    return false;
}

bool IsSoulsRE(std::wstring_view dllName)
{
    return EqualsIgnoreCase(dllName, L"SkyrimSoulsRE.dll");
}
} // namespace stubs
