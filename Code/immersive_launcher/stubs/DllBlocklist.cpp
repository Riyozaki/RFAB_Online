#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#define SPDLOG_WCHAR_FILENAMES
#include <spdlog/formatter.h>

#include "utils/Error.h"        // For Die()
#include "DllBlocklist.h"
#include "../Launcher.h"        // For gameDir
#include "../TargetConfig.h"    // For PRODUCT_NAME
#include <common/RfabModpackCompat.h>

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

std::string NormalizeLineEndings(const std::string& aInput)
{
    std::string out;
    out.reserve(aInput.size());
    for (char c : aInput)
    {
        if (c != '\r')
            out.push_back(c);
    }
    return out;
}
} // namespace

// clang-format off

    // A list of modules blocked for a variety of reasons, but mainly
    // for causing crashes and incompatibility with ST
    const wchar_t* const kDllBlocklist[] = {
        L"EngineFixes.dll",          // Skyrim Engine Fixes, allowed when grey-list config check passes
        L"crashhandler64.dll",       // Steam crash handler, breaks heap
        L"fraps64.dll",              // Breaks tilted ui
        L"SpecialK64.dll",           // breaks rendering
        L"ReShade64_SpecialK64.dll", // same reason
        L"NvCamera64.dll",           // broken af nvidia stuff, blacklisted for now, needs fix later
        L"BetterStealing.dll",       // NetScriptFramework plugin conflicts on SE 36525 / 19282 add-item prologue bytes
       // L"atiuxp64.dll",
       // L"aticfx64.dll"
    };

// clang-format on

struct DllGreyEntry
{
    const wchar_t* m_dllName;           // The name of the DLL to check.
    const wchar_t* m_configLocation;    // The relative path of the config file (from the game dir)
    const char*    m_configValidator;   // Regex to check if config is sane. Helps distinguish multiple releases of same dll.
    const char*    m_sigRegex;          // If this pattern is found in the config, comments were already added.
    const wchar_t* m_prompt;            // The MessageBox prompt asking for permission to fix.
    const char*    m_sigToInsert;       // This is added at the top of the rewritten config. It can/should say more than just the pattern match signature.
    const char*    m_replacers;         // Regex replacers. Regex & replacement separated by newlines.
};                                      //     You can have more than one replacer, also separated by newlines

// Data drive this. Supports EngineFixes v5.x (Skyrim SE 1.5.97 / RFAB), v6.x, and v7.x!
const DllGreyEntry kDllGreyList[] = 
{
    {
        L"EngineFixes.dll", 
        L"Data\\SKSE\\Plugins\\EngineFixes.toml",
        "(^|\\n)\\s*VerboseLogging\\s*=", // Matches EngineFixes v5.x (SE 1.5.97 / RFAB) & v6.x even when preceded by comments/[EngineFixes] header!
        "# SKYRIM TOGETHER REBORN marker for EngineFixes required compatibility settings v2, DO NOT CHANGE THIS LINE",

        L"For EngineFixes to work with RFAB Online, some settings are required:\n"
        "\tMemoryManager = false\n"
        "\tScaleformAllocator = false\n"
        "\tMaxStdio = 8192\n\n"

        "OK:\tMakes the changes for you\n"
        "Cancel:\tEngineFixes will not load\n\n"

        "If later you get the (harmless) SrtCrashFix64 popup, manually make this EngineFixes configuration change to suppress it:\n"
        "\tAnimationLoadSignedCrash = false",

        "# SKYRIM TOGETHER REBORN marker for EngineFixes required compatibility settings v2, DO NOT CHANGE THIS LINE\n"
        "# For EngineFixes to work with RFAB Online, some settings are required:\n"
        "#    MemoryManager = false\n"
        "#    ScaleformAllocator = false\n"
        "#    MaxStdio = 8192\n\n",

        "(^\\s*MemoryManager\\s*=\\s*)(true|false)\n$1false\n"
        "(^\\s*ScaleformAllocator\\s*=\\s*)(true|false)\n$1false\n"
        "(^\\s*MaxStdio\\s*=\\s*)([0-9]+)\n$018192\n"
    },
    {
        L"EngineFixes.dll", 
        L"Data\\SKSE\\Plugins\\EngineFixes.toml",
        "(^|\\n)\\s*bVerboseLogging\\s*=", // Matches EngineFixes Release 7.x series.
        "",
        L"",
        "",
        nullptr
    }
};

enum GreyListDisposition
{
    kNotGreyList,       // Not on the greylist at all
    kGreyListAccept,    // It is on the greylist with a good config, or the changes were accepted
    kGreyListAbort      // It is on the greylist and the configuration is no good.
};

GreyListDisposition IsConfigOK(const std::filesystem::path& aPath, const DllGreyEntry& aEntry)
{
    // Auto-heal EngineFixes.toml (v5/v6/v7 & partial MO2 overrides such as RFAB's "Disable Quick Saves")
    // directly through USVFS / MO2 mods before EngineFixes.dll reads its configuration on startup.
    RfabCompat::EnsureEngineFixesPatchedUnderRoot(aPath);

    std::filesystem::path configFile = aPath / aEntry.m_configLocation;
    std::string content;
    if (!RfabCompat::Detail::ReadTextFile(configFile, content))
        return kGreyListAccept;

    if (RfabCompat::PatchEngineFixesTomlContent(content))
        RfabCompat::Detail::WriteTextFile(configFile, content);

    return kGreyListAccept;
}

enum GreyListDisposition IsDllGreyListBlocked(const std::wstring_view aDllName)
{
    GreyListDisposition retval = kNotGreyList;
    launcher::LaunchContext* LC = launcher::GetLaunchContext(); 

    if (!LC)
        return kNotGreyList;

    std::filesystem::path gamePath = LC->gamePath;
    for (auto& greyListEntry : kDllGreyList)
    {
        if (EqualsIgnoreCase(aDllName, greyListEntry.m_dllName))
        {
            retval = IsConfigOK(gamePath, greyListEntry);
            if (retval == kGreyListAccept)
                break;
        }
    }
    return retval;
}

bool IsDllBlocked(std::wstring_view dllName)
{
    GreyListDisposition retval = IsDllGreyListBlocked(dllName);

    if (retval != kNotGreyList)
        return retval == kGreyListAbort;

    for (const wchar_t* dllEntry : kDllBlocklist)
    {
        if (EqualsIgnoreCase(dllName, dllEntry))
        {
            return true;
        }
    }

    return false;
}

bool IsSoulsRE(std::wstring_view dllName)
{
    return EqualsIgnoreCase(dllName, L"SkyrimSoulsRE.dll");
}
} // namespace stubs
