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
    std::regex signatureRegex; 

    std::filesystem::path configFile = aPath / aEntry.m_configLocation;
    std::fstream file(configFile, std::ios::in);
    if (!file.good())
        return kGreyListAccept;

    std::stringstream rawStream;
    rawStream << file.rdbuf();
    if (rawStream.bad() || file.bad())
    {
        auto msg = fmt::format(__FUNCTION__ L": failed to read {}", aEntry.m_configLocation);
        Die(msg.c_str(), true);
        return kGreyListAbort;
    }

    const std::string normalizedOrig = NormalizeLineEndings(rawStream.str());
    std::stringstream configStream(normalizedOrig);
    std::string newConfig;

    try
    {
        {
            const std::regex validatorRegex(aEntry.m_configValidator, std::regex_constants::icase);
            std::smatch vmatch;
            if (!std::regex_search(normalizedOrig.begin(), normalizedOrig.end(), vmatch, validatorRegex))
                return kGreyListAbort;
        }

        signatureRegex = std::regex(aEntry.m_sigRegex, std::regex_constants::icase);

        if (!aEntry.m_replacers)
            return kGreyListAccept;

        std::string line;
        std::stringstream replacers(aEntry.m_replacers);

        while (std::getline(configStream, line))
        {
            std::string pattern;
            std::string withThat;
            while (std::getline(replacers, pattern) && std::getline(replacers, withThat))
            {
                std::regex replaceThis(pattern, std::regex_constants::icase);
                line = std::regex_replace(line, replaceThis, withThat);
            }

            newConfig += line + "\n";
            replacers.clear();
            replacers.seekg(0, std::ios::beg);
        }
    }
    catch ([[maybe_unused]] const std::regex_error& e)
    {
        auto msg = fmt::format(__FUNCTION__ L": invalid regular expression in DLL grey list handling");
        Die(msg.c_str(), true);
        return kGreyListAbort;
    }

    if (_stricmp(newConfig.c_str(), normalizedOrig.c_str()) == 0)
        return kGreyListAccept;

    if (MessageBoxW(NULL, aEntry.m_prompt, PRODUCT_NAME L" Requires Configuration Changes", MB_OKCANCEL | MB_ICONWARNING) == IDCANCEL)
        return kGreyListAbort;

    if (!std::regex_search(newConfig, signatureRegex))
        newConfig = aEntry.m_sigToInsert + newConfig;

    file = std::fstream(configFile, std::ios::in | std::ios::out | std::ios::trunc);

    if (!file.bad())
        file << newConfig;

    if (file.bad())
    {
        auto msg = fmt::format(__FUNCTION__ L": failed to rewrite {}", configFile.c_str());
        Die(msg.c_str(), true);
        return kGreyListAbort;
    }

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
