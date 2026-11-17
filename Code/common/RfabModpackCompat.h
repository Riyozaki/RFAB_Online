#pragma once

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Compat/SafeFs.h"

namespace RfabCompat
{
inline constexpr std::string_view kRfabOnlineVersion = "5.0.0-dev.1";

namespace Detail
{
inline std::string Trim(std::string_view aView)
{
    size_t start = 0;
    while (start < aView.size() && std::isspace(static_cast<unsigned char>(aView[start])))
        ++start;

    size_t end = aView.size();
    while (end > start && std::isspace(static_cast<unsigned char>(aView[end - 1])))
        --end;

    return std::string(aView.substr(start, end - start));
}

inline std::string ToLowerAscii(std::string_view aView)
{
    std::string out(aView);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline bool StartsWithIgnoreCase(std::string_view aText, std::string_view aPrefix) noexcept
{
    if (aPrefix.empty() || aText.size() < aPrefix.size())
        return false;

    for (size_t i = 0; i < aPrefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(aText[i])) != std::tolower(static_cast<unsigned char>(aPrefix[i])))
            return false;
    }
    return true;
}

inline bool EndsWithIgnoreCase(std::string_view aText, std::string_view aSuffix) noexcept
{
    if (aSuffix.empty() || aText.size() < aSuffix.size())
        return false;

    const size_t offset = aText.size() - aSuffix.size();
    for (size_t i = 0; i < aSuffix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(aText[offset + i])) != std::tolower(static_cast<unsigned char>(aSuffix[i])))
            return false;
    }
    return true;
}


inline void AppendCodePointUtf8(std::string& aOut, uint32_t aCp)
{
    if (aCp <= 0x7Fu)
    {
        aOut.push_back(static_cast<char>(aCp));
    }
    else if (aCp <= 0x7FFu)
    {
        aOut.push_back(static_cast<char>(0xC0u | (aCp >> 6)));
        aOut.push_back(static_cast<char>(0x80u | (aCp & 0x3Fu)));
    }
    else if (aCp <= 0xFFFFu)
    {
        aOut.push_back(static_cast<char>(0xE0u | (aCp >> 12)));
        aOut.push_back(static_cast<char>(0x80u | ((aCp >> 6) & 0x3Fu)));
        aOut.push_back(static_cast<char>(0x80u | (aCp & 0x3Fu)));
    }
    else if (aCp <= 0x10FFFFu)
    {
        aOut.push_back(static_cast<char>(0xF0u | (aCp >> 18)));
        aOut.push_back(static_cast<char>(0x80u | ((aCp >> 12) & 0x3Fu)));
        aOut.push_back(static_cast<char>(0x80u | ((aCp >> 6) & 0x3Fu)));
        aOut.push_back(static_cast<char>(0x80u | (aCp & 0x3Fu)));
    }
}

inline bool ReadTextFile(const std::filesystem::path& acPath, std::string& aOut) noexcept
{
    std::error_code ec;
    if (!std::filesystem::exists(acPath, ec) || !std::filesystem::is_regular_file(acPath, ec))
        return false;

    std::ifstream ifs(acPath, std::ios::in | std::ios::binary);
    if (!ifs.is_open())
        return false;

    std::ostringstream ss;
    ss << ifs.rdbuf();
    aOut = ss.str();
    return true;
}

inline bool WriteTextFile(const std::filesystem::path& acPath, std::string_view aContent) noexcept
{
    std::error_code ec;
    std::filesystem::create_directories(acPath.parent_path(), ec);

    std::ofstream ofs(acPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs.is_open())
        return false;

    ofs.write(aContent.data(), static_cast<std::streamsize>(aContent.size()));
    return ofs.good();
}
} // namespace Detail

// Idempotently patches EngineFixes.toml content (including overrides like RFAB's
// "Disable Quick Saves/skse/plugins/EngineFixes.toml") so Tilted's allocator & MO2 VFS work cleanly
// across both EngineFixes v5/v6 (Skyrim SE 1.5.97) and v7 keys while preserving user settings.
inline bool PatchEngineFixesTomlContent(std::string& aToml)
{
    bool changed = false;
    const bool hasCrLf = aToml.find("\r\n") != std::string::npos;
    const std::string newline = hasCrLf ? "\r\n" : "\n";

    // Auto-heal invalid single-letter TOML lines ("S = a", "t = r", "f = a") left by an earlier
    // PowerShell single-element array-unrolling bug, which otherwise abort EngineFixes.toml parsing
    // and drop MaxStdio back to 512 (causing EMFILE -> sSaveGameCorrupt in 600-BSA RFAB installs).
    {
        std::istringstream cleanIss(aToml);
        std::ostringstream cleanOss;
        std::string cleanLine;
        bool cleanFirst = true;
        bool strippedAny = false;
        while (std::getline(cleanIss, cleanLine))
        {
            if (!cleanLine.empty() && cleanLine.back() == '\r')
                cleanLine.pop_back();
            const std::string t = Detail::Trim(cleanLine);
            if (t == "S = a" || t == "t = r" || t == "f = a")
            {
                strippedAny = true;
                continue;
            }
            if (!cleanFirst)
                cleanOss << newline;
            cleanFirst = false;
            cleanOss << cleanLine;
        }
        if (strippedAny)
        {
            aToml = cleanOss.str() + ((!aToml.empty() && aToml.back() == '\n') ? newline : "");
            changed = true;
        }
    }

    const bool hasBPrefixV7 = aToml.find("bVerboseLogging") != std::string::npos || aToml.find("bMemoryManager") != std::string::npos;
    const bool hasMemMgrSectionV7 = aToml.find("[MemoryManager]") != std::string::npos &&
                                    (aToml.find("OverrideVanilla") != std::string::npos || aToml.find("OverrideScaleform") != std::string::npos);

    auto patchKey = [&](std::string_view keyName, std::string_view desiredValue) -> bool
    {
        std::istringstream iss(aToml);
        std::ostringstream oss;
        std::string line;
        bool first = true;
        bool found = false;

        while (std::getline(iss, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (!first)
                oss << newline;
            first = false;

            const std::string trimmed = Detail::Trim(line);
            if (!trimmed.empty() && trimmed.front() != '#' && trimmed.front() != '[')
            {
                if (const size_t eq = trimmed.find('='); eq != std::string::npos)
                {
                    const std::string currentKey = Detail::Trim(std::string_view(trimmed).substr(0, eq));
                    if (Detail::ToLowerAscii(currentKey) == Detail::ToLowerAscii(keyName))
                    {
                        found = true;
                        std::string currentVal = Detail::Trim(std::string_view(trimmed).substr(eq + 1));
                        if (const size_t hash = currentVal.find('#'); hash != std::string::npos)
                            currentVal = Detail::Trim(std::string_view(currentVal).substr(0, hash));

                        if (Detail::ToLowerAscii(currentVal) != Detail::ToLowerAscii(desiredValue))
                        {
                            const size_t rawEq = line.find('=');
                            std::string commentSuffix;
                            if (const size_t rawHash = line.find('#', rawEq + 1); rawHash != std::string::npos)
                                commentSuffix = " " + Detail::Trim(std::string_view(line).substr(rawHash));

                            line = line.substr(0, rawEq + 1) + " " + std::string(desiredValue) + commentSuffix;
                            changed = true;
                        }
                    }
                }
            }

            oss << line;
        }

        if (found)
            aToml = oss.str() + ((!aToml.empty() && aToml.back() == '\n') ? newline : "");
        return found;
    };

    auto insertUnderSection = [&](std::string_view sectionName, const std::vector<std::pair<std::string_view, std::string_view>>& missingEntries)
    {
        if (missingEntries.empty())
            return;

        std::string addBlock;
        for (const auto& [k, v] : missingEntries)
            addBlock += std::string(k) + " = " + std::string(v) + newline;

        const std::string header = "[" + std::string(sectionName) + "]";
        const size_t pos = aToml.find(header);
        if (pos != std::string::npos)
        {
            size_t lineEnd = aToml.find('\n', pos);
            if (lineEnd == std::string::npos)
            {
                aToml += newline + addBlock;
            }
            else
            {
                aToml.insert(lineEnd + 1, addBlock);
            }
        }
        else
        {
            if (!aToml.empty() && aToml.back() != '\n')
                aToml += newline;
            aToml += header + newline + addBlock;
        }
        changed = true;
    };

    if (hasBPrefixV7)
    {
        const bool hasBMem = patchKey("bMemoryManager", "false");
        const bool hasBScale = patchKey("bScaleformAllocator", "false");
        patchKey("bOverrideScrapHeap", "false");
        patchKey("iSaveGameMaxSize", "64");
        const bool hasBQuick = patchKey("bRegularQuicksaves", "true");
        const bool hasBStdio = patchKey("iMaxStdio", "8192");
        const bool hasBFormCache = patchKey("bFormCaching", "false");
        const bool hasBTreeLodCache = patchKey("bTreeLodReferenceCaching", "false");

        std::vector<std::pair<std::string_view, std::string_view>> missingV7Patches;
        if (!hasBMem)
            missingV7Patches.emplace_back("bMemoryManager", "false");
        if (!hasBScale)
            missingV7Patches.emplace_back("bScaleformAllocator", "false");
        if (!hasBQuick)
            missingV7Patches.emplace_back("bRegularQuicksaves", "true");
        if (!hasBStdio)
            missingV7Patches.emplace_back("iMaxStdio", "8192");
        if (!hasBFormCache)
            missingV7Patches.emplace_back("bFormCaching", "false");
        if (!hasBTreeLodCache)
            missingV7Patches.emplace_back("bTreeLodReferenceCaching", "false");
        insertUnderSection("Patches", missingV7Patches);
    }
    if (hasMemMgrSectionV7)
    {
        const bool hasOvVanilla = patchKey("OverrideVanilla", "false");
        const bool hasOvScaleform = patchKey("OverrideScaleform", "false");
        patchKey("OverrideScrapHeap", "false");
        std::vector<std::pair<std::string_view, std::string_view>> missingMm;
        if (!hasOvVanilla)
            missingMm.emplace_back("OverrideVanilla", "false");
        if (!hasOvScaleform)
            missingMm.emplace_back("OverrideScaleform", "false");
        insertUnderSection("MemoryManager", missingMm);
    }
    if (!hasBPrefixV7)
    {
        // Heal stale [SaveGameMaxSize] section header if created by earlier patcher version -> standard [Experimental]
        if (const size_t wrongSec = aToml.find("[SaveGameMaxSize]"); wrongSec != std::string::npos)
        {
            aToml.replace(wrongSec, std::strlen("[SaveGameMaxSize]"), "[Experimental]");
            changed = true;
        }

        // CRITICAL:
        // 1. In SSE Engine Fixes v5.7.0 / v6.0.2 (Skyrim SE 1.5.97 & drake1ch/RFAB), `PatchFormCaching()`
        //    replaces thread-safe `GlobalFormTableLock` with an unlocked `tbb::concurrent_hash_map` whose
        //    `.emplace(baseId, Value)` fails to overwrite recycled 0xFFxxxxxx FormIDs and lacks ClearData hooks,
        //    causing `TreeLODReferenceCaching::hk_UpdateBlockVisibility` (`EngineFixes.dll+0x411EB` -> `__RTDynamicCast`)
        //    on background render threads to throw `0xE06D7363` (`std::__non_rtti_object: Access violation - no RTTI data!`)
        //    during co-op dynamic actor spawns/despawns/saves. Disabling `FormCaching = false` & `TreeLODReferenceCaching = false`
        //    restores Bethesda's native `BSReadWriteLock`-protected form lookup with zero performance loss on ~30-plugin RFAB.
        // 2. `PatchSaveGameMaxSize()` lacks v7's `if (!bOverrideScrapHeap) return;` guard: when
        //    `SaveGameMaxSize = true` and `MemoryManager = false`, it patches `Win32FileType` to request
        //    128 MB (`0x08000000`) from vanilla's 64 MB (`0x04000000`) `ScrapHeap`, causing
        //    `ScrapHeap::Allocate` to return `nullptr` and corrupting saves/loads (`sSaveGameCorrupt`).
        //    Always keep `SaveGameMaxSize = false` whenever `MemoryManager = false`.
        const bool hasMemMgr = patchKey("MemoryManager", "false");
        const bool hasScaleform = patchKey("ScaleformAllocator", "false");
        const bool hasFormCache = patchKey("FormCaching", "false");
        const bool hasTreeLodCache = patchKey("TreeLODReferenceCaching", "false");
        const bool hasQuicksaves = patchKey("RegularQuicksaves", "true");
        const bool hasMaxStdio = patchKey("MaxStdio", "8192");
        const bool hasSaveMax = patchKey("SaveGameMaxSize", "false");

        // Ensure partial MO2 overrides (e.g. "Disable Quick Saves/skse/plugins/EngineFixes.toml")
        // contain all required [Patches] keys inside [Patches] itself for SSE Engine Fixes v5/v6 (SE 1.5.97)!
        std::vector<std::pair<std::string_view, std::string_view>> missingPatches;
        if (!hasMemMgr && !hasMemMgrSectionV7)
            missingPatches.emplace_back("MemoryManager", "false");
        if (!hasScaleform && !hasMemMgrSectionV7)
            missingPatches.emplace_back("ScaleformAllocator", "false");
        if (!hasFormCache)
            missingPatches.emplace_back("FormCaching", "false");
        if (!hasTreeLodCache)
            missingPatches.emplace_back("TreeLODReferenceCaching", "false");
        if (!hasQuicksaves)
            missingPatches.emplace_back("RegularQuicksaves", "true");
        if (!hasMaxStdio)
            missingPatches.emplace_back("MaxStdio", "8192");
        insertUnderSection("Patches", missingPatches);

        if (!hasSaveMax)
            insertUnderSection("Experimental", {{"SaveGameMaxSize", "false"}});
    }

    return changed;
}

// Neutralizes local menu time-dilation in SkyrimSoulsRE.ini ([SLOWMOTION]) so opening menus never desyncs real-time co-op.
inline bool PatchSkyrimSoulsReIniContent(std::string& aIni)
{
    bool changed = false;
    const bool hasCrLf = aIni.find("\r\n") != std::string::npos;
    const std::string newline = hasCrLf ? "\r\n" : "\n";

    std::istringstream iss(aIni);
    std::ostringstream oss;
    std::string line;
    bool first = true;

    while (std::getline(iss, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (!first)
            oss << newline;
        first = false;

        const std::string trimmed = Detail::Trim(line);
        if (!trimmed.empty() && trimmed.front() != ';' && trimmed.front() != '#' && trimmed.front() != '[')
        {
            if (const size_t eq = trimmed.find('='); eq != std::string::npos)
            {
                const std::string keyLower = Detail::ToLowerAscii(Detail::Trim(std::string_view(trimmed).substr(0, eq)));
                std::string val = Detail::Trim(std::string_view(trimmed).substr(eq + 1));
                if (const size_t semi = val.find_first_of(";#"); semi != std::string::npos)
                    val = Detail::Trim(std::string_view(val).substr(0, semi));

                if (Detail::StartsWithIgnoreCase(keyLower, "benableslowmotion") && Detail::ToLowerAscii(val) != "false")
                {
                    const size_t rawEq = line.find('=');
                    line = line.substr(0, rawEq + 1) + " false";
                    changed = true;
                }
                else if (keyLower == "bstatsmenu" && Detail::ToLowerAscii(val) != "false")
                {
                    // StatsMenu (Skills/Perks) MUST remain paused so CustomSkills skydomes and RFAB PerkPlacementMenuOpenCloseManager never race unpaused world ticks.
                    const size_t rawEq = line.find('=');
                    line = line.substr(0, rawEq + 1) + " false";
                    changed = true;
                }
                else if (keyLower == "fslowmotionmultiplier" && val != "1.000000" && val != "1.0" && val != "1")
                {
                    const size_t rawEq = line.find('=');
                    line = line.substr(0, rawEq + 1) + " 1.000000";
                    changed = true;
                }
            }
        }

        oss << line;
    }

    if (changed)
        aIni = oss.str() + ((!aIni.empty() && aIni.back() == '\n') ? newline : "");

    return changed;
}

inline std::string SuppressRfabAutosaveIniContent(std::string_view aOriginalIni);

// "[RFAB] Autosave.ini" as the co-op process must read it: every RFAB autosave trigger off (the
// coordinated co-op save is the only save). Served by the co-op config overlay; never written back.
inline std::string SuppressRfabAutosaveIniContent(std::string_view aOriginalIni)
{
    const bool hasCrLf = aOriginalIni.find("\r\n") != std::string_view::npos;
    const std::string newline = hasCrLf ? "\r\n" : "\n";

    std::istringstream iss{std::string(aOriginalIni)};
    std::ostringstream oss;
    std::string line;
    bool first = true;

    while (std::getline(iss, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (!first)
            oss << newline;
        first = false;

        const std::string trimmed = Detail::Trim(line);
        if (!trimmed.empty() && trimmed.front() != ';' && trimmed.front() != '#' && trimmed.front() != '[')
        {
            if (const size_t eq = trimmed.find('='); eq != std::string::npos)
            {
                const std::string key = Detail::ToLowerAscii(Detail::Trim(std::string_view(trimmed).substr(0, eq)));
                if (key == "enable autosave" || key == "save in combat" || key == "save after combat" ||
                    key == "save on location change" || key == "save on sleep/wait" || key == "enabled")
                {
                    const size_t rawEq = line.find('=');
                    line = (key == "enabled")
                        ? (line.substr(0, rawEq + 1) + " false")
                        : (line.substr(0, rawEq + 1) + " 0");
                }
            }
        }

        oss << line;
    }

    if (!aOriginalIni.empty() && aOriginalIni.back() == '\n')
        oss << newline;

    return oss.str();
}

namespace Detail
{
[[nodiscard]] inline int HexDigitVal(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

[[nodiscard]] inline std::filesystem::path PathFromUtf8Safe(std::string_view aUtf8)
{
#ifndef _WIN32
    std::string normalized(aUtf8);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const std::u8string u8(reinterpret_cast<const char8_t*>(normalized.data()), normalized.size());
#else
    const std::u8string u8(reinterpret_cast<const char8_t*>(aUtf8.data()), aUtf8.size());
#endif
    return std::filesystem::path(u8);
}

// Never throws and never fails on the name: path::u8string() throws on a name with an unpaired
// surrogate, path::string() on any character outside the ANSI code page (M14).
[[nodiscard]] inline std::string PathToUtf8Safe(const std::filesystem::path& acPath) noexcept
{
    return ::Compat::SafeFs::ToUtf8Lossy(acPath);
}
} // namespace Detail

/**
 * Decodes a ModOrganizer.ini value into clean UTF-8, supporting both Qt `@ByteArray(...)`
 * (\xHH raw UTF-8 byte escapes + \\) and Qt QSettings string escapes (\xHHHH UTF-16 code units + \\).
 */
[[nodiscard]] inline std::string DecodeMo2IniValueUtf8(std::string_view aRaw)
{
    std::string val = Detail::Trim(aRaw);
    if (val.empty())
        return {};

    std::string out;
    if (val.size() >= 12 && Detail::StartsWithIgnoreCase(val, "@bytearray(") && val.back() == ')')
    {
        const std::string_view inner(val.data() + 11, val.size() - 12);
        out.reserve(inner.size());
        for (size_t i = 0; i < inner.size(); ++i)
        {
            if (inner[i] == '\\' && (i + 1) < inner.size())
            {
                const char n = inner[i + 1];
                if (n == 'x' && (i + 3) < inner.size())
                {
                    const int hi = Detail::HexDigitVal(inner[i + 2]);
                    const int lo = Detail::HexDigitVal(inner[i + 3]);
                    if (hi >= 0 && lo >= 0)
                    {
                        out.push_back(static_cast<char>((hi << 4) | lo));
                        i += 3;
                        continue;
                    }
                }
                else if (n == '\\')
                {
                    out.push_back('\\');
                    ++i;
                    continue;
                }
                else if (n == '"')
                {
                    out.push_back('"');
                    ++i;
                    continue;
                }
                else if (n == 't')
                {
                    out.push_back('\t');
                    ++i;
                    continue;
                }
            }
            out.push_back(inner[i]);
        }
    }
    else
    {
        out.reserve(val.size());
        for (size_t i = 0; i < val.size(); ++i)
        {
            if (val[i] == '\\' && (i + 1) < val.size())
            {
                const char n = val[i + 1];
                if (n == 'x')
                {
                    uint32_t codeUnit = 0;
                    size_t digits = 0;
                    while (digits < 4 && (i + 2 + digits) < val.size())
                    {
                        const int hv = Detail::HexDigitVal(val[i + 2 + digits]);
                        if (hv < 0)
                            break;
                        codeUnit = (codeUnit << 4) | static_cast<uint32_t>(hv);
                        ++digits;
                    }
                    if (digits >= 2)
                    {
                        Detail::AppendCodePointUtf8(out, codeUnit);
                        i += 1 + digits;
                        continue;
                    }
                }
                else if (n == '\\')
                {
                    out.push_back('\\');
                    ++i;
                    continue;
                }
                else if (n == '"')
                {
                    out.push_back('"');
                    ++i;
                    continue;
                }
            }
            out.push_back(val[i]);
        }
    }

    out = Detail::Trim(out);
    if (out.size() >= 2 && out.front() == '"' && out.back() == '"')
        out = out.substr(1, out.size() - 2);

    return out;
}

/**
 * Walks up from `acStartDir` (e.g. `<MO2>/mods/RFAB_Online/SkyrimTogetherReborn`) to locate `<MO2>/ModOrganizer.ini`,
 * parses `gamePath=` (and `%BASE_DIR%`), and returns the configured Skyrim root if `<gamePath>/<acExeName>` + `Data` exist.
 * Prevents USVFS / CWD desync when a user reinstalls clean Skyrim in a new path while an old `SkyrimSE.exe` or registry
 * `TitlePath` still exists elsewhere on disk.
 */
[[nodiscard]] inline std::filesystem::path ResolveMo2ConfiguredGamePath(
    const std::filesystem::path& acStartDir,
    const std::filesystem::path& acSelfDir = {},
    std::string_view aExeNameUtf8 = "SkyrimSE.exe") noexcept
{
    std::error_code ec;
    if (acStartDir.empty())
        return {};

    std::filesystem::path cur = acStartDir;
    for (int depth = 0; depth < 6 && !cur.empty(); ++depth)
    {
        const std::filesystem::path iniPath = cur / "ModOrganizer.ini";
        ec.clear();
        if (std::filesystem::exists(iniPath, ec) && std::filesystem::is_regular_file(iniPath, ec))
        {
            std::string content;
            if (Detail::ReadTextFile(iniPath, content))
            {
                std::string rawBaseDir;
                std::string rawGamePath;
                std::istringstream iss(content);
                std::string line;
                while (std::getline(iss, line))
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    const std::string trimmed = Detail::Trim(line);
                    if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' || trimmed.front() == '[')
                        continue;
                    const size_t eq = trimmed.find('=');
                    if (eq == std::string::npos)
                        continue;
                    const std::string keyLower = Detail::ToLowerAscii(Detail::Trim(std::string_view(trimmed).substr(0, eq)));
                    const std::string_view rhs = std::string_view(trimmed).substr(eq + 1);
                    if (keyLower == "base_directory")
                        rawBaseDir = DecodeMo2IniValueUtf8(rhs);
                    else if (keyLower == "gamepath")
                        rawGamePath = DecodeMo2IniValueUtf8(rhs);
                }

                if (!rawGamePath.empty())
                {
                    std::filesystem::path baseDirPath = cur;
                    if (!rawBaseDir.empty())
                    {
                        std::filesystem::path parsedBase = Detail::PathFromUtf8Safe(rawBaseDir);
                        if (!parsedBase.is_absolute())
                            parsedBase = (cur / parsedBase).lexically_normal();
                        ec.clear();
                        if (std::filesystem::exists(parsedBase, ec))
                            baseDirPath = parsedBase;
                    }

                    // Case-insensitive %BASE_DIR% token replacement
                    const std::string baseDirUtf8 = Detail::PathToUtf8Safe(baseDirPath);
                    std::string expanded = rawGamePath;
                    constexpr std::string_view kToken = "%base_dir%";
                    for (;;)
                    {
                        const std::string lowerExp = Detail::ToLowerAscii(expanded);
                        const size_t pos = lowerExp.find(kToken);
                        if (pos == std::string::npos)
                            break;
                        expanded.replace(pos, kToken.size(), baseDirUtf8);
                    }

                    std::filesystem::path candidateRoot = Detail::PathFromUtf8Safe(expanded);
                    if (!candidateRoot.is_absolute())
                        candidateRoot = (cur / candidateRoot).lexically_normal();
                    else
                        candidateRoot = candidateRoot.lexically_normal();

                    ec.clear();
                    const bool sameAsSelf =
                        !acSelfDir.empty() &&
                        std::filesystem::exists(acSelfDir, ec) &&
                        std::filesystem::exists(candidateRoot, ec) &&
                        std::filesystem::equivalent(candidateRoot, acSelfDir, ec);

                    if (!sameAsSelf)
                    {
                        const std::filesystem::path exePath = candidateRoot / Detail::PathFromUtf8Safe(aExeNameUtf8);
                        const std::filesystem::path dataPath = candidateRoot / "Data";
                        ec.clear();
                        if (std::filesystem::exists(exePath, ec) && std::filesystem::exists(dataPath, ec))
                            return candidateRoot;
                    }
                }
            }
        }

        const std::filesystem::path parent = cur.parent_path();
        if (parent == cur)
            break;
        cur = parent;
    }

    return {};
}

/**
 * Version-agnostic detection of CustomSkills (v1 .NetScriptFramework configs OR v2/v3 SKSE JSON configs),
 * RFAB PerkPlacement configs, or Extended UI under `Data/`, so StatsMenu stays safely paused across all RFAB updates.
 */
[[nodiscard]] inline bool HasCustomSkillsOrRfabFilesUnderData(const std::filesystem::path& acDataDir) noexcept
{
    std::error_code ec;
    if (acDataDir.empty() || !std::filesystem::exists(acDataDir, ec))
        return false;

    // 1. CustomSkills v2/v3+ directory (`Data/SKSE/Plugins/CustomSkills`) or RFAB PerkPlacement INI
    const std::filesystem::path sksePlugins = acDataDir / "SKSE" / "Plugins";
    ec.clear();
    if (std::filesystem::exists(sksePlugins / "CustomSkills", ec) ||
        std::filesystem::exists(sksePlugins / "[RFAB] PerksPlacement.ini", ec))
    {
        return true;
    }

    bool found = false;
    ::Compat::SafeFs::ForEachEntry(sksePlugins, [&found](const std::filesystem::directory_entry& acEntry) {
        const std::string lowerName = Detail::ToLowerAscii(::Compat::SafeFs::FileNameUtf8Lossy(acEntry.path()));
        found = lowerName.starts_with("customskills") || lowerName.starts_with("rfab") || lowerName.starts_with("[rfab]");
        return !found;
    });
    if (found)
        return true;

    // 2. CustomSkills v1 (`Data/NetScriptFramework/Plugins/customskill.*.config.txt`)
    const std::filesystem::path nsfPlugins = acDataDir / "NetScriptFramework" / "Plugins";
    ::Compat::SafeFs::ForEachEntry(nsfPlugins, [&found](const std::filesystem::directory_entry& acEntry) {
        found = Detail::ToLowerAscii(::Compat::SafeFs::FileNameUtf8Lossy(acEntry.path())).starts_with("customskill.");
        return !found;
    });
    if (found)
        return true;

    // 3. Extended UI or RFAB master plugin in Data
    ec.clear();
    if (std::filesystem::exists(acDataDir / "Extended UI.esp", ec) ||
        std::filesystem::exists(acDataDir / "RFAB.esp", ec))
    {
        return true;
    }

    return false;
}

// ============================================================================
// Runtime signature scanner & trampoline builder for RFAB.dll StatsMenu crash:
// PerkPlacementMenuOpenCloseManager::ProcessEvent walks ActorValueList::actorValues
// (`[rsi + rbx*8 + 0x8]`) until `rax == 0` with no `< 164` (`0xA4`) upper bound and
// no `children._data != nullptr` (`[rdx + 0x10]`) check before BSTArray copy-ctor.
// Scanning the loop tail (`48 FF C3 48 8B 44 DE 08 48 85 C0 75 <rel8>`) works across:
//   - unpatched RFAB.dll builds (even if recompiled at new RVAs in future RFAB updates),
//   - already disk-patched RFAB.dll builds (`loopHead[0] == 0xE9`, left untouched).
// ============================================================================
struct RfabPerkLoopScanResult
{
    enum class Status : uint8_t
    {
        NotFound = 0,
        FoundUnpatched = 1,
        AlreadyPatched = 2,
    };

    Status State{Status::NotFound};
    size_t LoopHeadOffset{0};
    size_t ResumeOffset{0};
    size_t SkipOffset{0};
    size_t ExitOffset{0};
};

[[nodiscard]] inline RfabPerkLoopScanResult LocateRfabPerkPlacementLoop(
    const uint8_t* apBytes,
    size_t aSize) noexcept
{
    RfabPerkLoopScanResult res{};
    if (!apBytes || aSize < 64)
        return res;

    // Loop tail signature:
    //   48 FF C3             inc rbx
    //   48 8B 44 DE 08       mov rax, qword ptr [rsi + rbx*8 + 0x8]
    //   48 85 C0             test rax, rax
    //   75 <rel8>            jne loopHead
    constexpr uint8_t kTailSig[12] = {
        0x48, 0xFF, 0xC3,
        0x48, 0x8B, 0x44, 0xDE, 0x08,
        0x48, 0x85, 0xC0,
        0x75
    };

    for (size_t i = 0; i + 13 <= aSize; ++i)
    {
        if (std::memcmp(apBytes + i, kTailSig, sizeof(kTailSig)) != 0)
            continue;

        const int8_t backDisp = static_cast<int8_t>(apBytes[i + 12]);
        if (backDisp >= 0)
            continue;

        const ptrdiff_t headSigned = static_cast<ptrdiff_t>(i + 13) + static_cast<ptrdiff_t>(backDisp);
        if (headSigned < 2 || static_cast<size_t>(headSigned + 16) > aSize)
            continue;

        const size_t head = static_cast<size_t>(headSigned);
        // Preceded by `xor ebx, ebx` (`33 DB`)
        if (apBytes[head - 2] != 0x33 || apBytes[head - 1] != 0xDB)
            continue;

        // Resume at `head + 7` must be `48 85 D2 74 <skipDisp> 83 7A 20 00 74`
        if (apBytes[head + 7] != 0x48 || apBytes[head + 8] != 0x85 || apBytes[head + 9] != 0xD2 ||
            apBytes[head + 10] != 0x74 ||
            apBytes[head + 12] != 0x83 || apBytes[head + 13] != 0x7A || apBytes[head + 14] != 0x20 || apBytes[head + 15] != 0x00)
        {
            continue;
        }

        const int8_t skipDisp = static_cast<int8_t>(apBytes[head + 11]);
        const ptrdiff_t targetSkip = static_cast<ptrdiff_t>(head + 12) + static_cast<ptrdiff_t>(skipDisp);
        if (targetSkip != static_cast<ptrdiff_t>(i))
            continue;

        res.LoopHeadOffset = head;
        res.ResumeOffset = head + 7;
        res.SkipOffset = i;
        res.ExitOffset = i + 13;

        if (apBytes[head] == 0xE9)
        {
            res.State = RfabPerkLoopScanResult::Status::AlreadyPatched;
            return res;
        }

        constexpr uint8_t kUnpatchedHead[7] = {0x48, 0x8B, 0x90, 0x18, 0x01, 0x00, 0x00};
        if (std::memcmp(apBytes + head, kUnpatchedHead, sizeof(kUnpatchedHead)) == 0)
        {
            res.State = RfabPerkLoopScanResult::Status::FoundUnpatched;
            return res;
        }
    }

    return res;
}

inline constexpr size_t kRfabPerkTrampolineSize = 55;

[[nodiscard]] inline bool BuildRfabPerkPlacementTrampoline(
    uintptr_t aCaveAddr,
    uintptr_t aResumeAddr,
    uintptr_t aSkipAddr,
    uintptr_t aExitAddr,
    uint8_t (&aOutCave)[kRfabPerkTrampolineSize]) noexcept
{
    auto writeRel32 = [](uint8_t* pDest, uintptr_t nextIp, uintptr_t target) noexcept -> bool
    {
        const intptr_t diff = static_cast<intptr_t>(target) - static_cast<intptr_t>(nextIp);
        if (diff < INT32_MIN || diff > INT32_MAX)
            return false;
        const auto d32 = static_cast<int32_t>(diff);
        std::memcpy(pDest, &d32, sizeof(d32));
        return true;
    };

    // 00: 48 81 FB A4 00 00 00    cmp rbx, 0xA4 (164 entries in SE 1.5.97 ActorValueList)
    // 07: 0F 83 <rel32 exit>      jae exitAddr
    // 0D: 80 78 1A 5F             cmp byte ptr [rax+0x1A], 0x5F (FormType::ActorValueInfo)
    // 11: 0F 85 <rel32 skip>      jne skipAddr
    // 17: 48 8B 90 18 01 00 00    mov rdx, qword ptr [rax+0x118]
    // 1E: 48 85 D2                test rdx, rdx
    // 21: 0F 84 <rel32 skip>      je skipAddr
    // 27: 48 83 7A 10 00          cmp qword ptr [rdx+0x10], 0 (children._data != nullptr)
    // 2C: 0F 84 <rel32 skip>      je skipAddr
    // 32: E9 <rel32 resume>       jmp resumeAddr
    const uint8_t tmpl[kRfabPerkTrampolineSize] = {
        0x48, 0x81, 0xFB, 0xA4, 0x00, 0x00, 0x00,
        0x0F, 0x83, 0x00, 0x00, 0x00, 0x00,
        0x80, 0x78, 0x1A, 0x5F,
        0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x90, 0x18, 0x01, 0x00, 0x00,
        0x48, 0x85, 0xD2,
        0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x83, 0x7A, 0x10, 0x00,
        0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,
        0xE9, 0x00, 0x00, 0x00, 0x00
    };
    std::memcpy(aOutCave, tmpl, sizeof(tmpl));

    if (!writeRel32(aOutCave + 9,  aCaveAddr + 13, aExitAddr))   return false;
    if (!writeRel32(aOutCave + 19, aCaveAddr + 23, aSkipAddr))   return false;
    if (!writeRel32(aOutCave + 35, aCaveAddr + 39, aSkipAddr))   return false;
    if (!writeRel32(aOutCave + 46, aCaveAddr + 50, aSkipAddr))   return false;
    if (!writeRel32(aOutCave + 51, aCaveAddr + 55, aResumeAddr)) return false;

    return true;
}

// ============================================================================
// Stage 3.3: Cross-Version RFAB Compatibility Suite
//   1. Canonical SSE `plugins.txt` (`*Active.esp` vs disabled lines) & `loadorder.txt` parser
//   2. Recursive `Data/SKSE/Plugins/` config discovery when RFAB relocates INI/JSON files
//   3. Zero-dependency TES4 `.esm/.esp/.esl` binary `EDID <-> (PluginName, LocalFormId)` indexer
//      (resolves BGSListForm, BGSPerk, TESNPC, TESObjectARMO, etc. whose runtime EDIDs Skyrim SE strips!)
// ============================================================================

[[nodiscard]] inline bool IsImplicitSseBaseMaster(std::string_view aFilename) noexcept
{
    const std::string lower = Detail::ToLowerAscii(Detail::Trim(aFilename));
    return lower == "skyrim.esm" ||
           lower == "update.esm" ||
           lower == "dawnguard.esm" ||
           lower == "hearthfires.esm" ||
           lower == "dragonborn.esm";
}

[[nodiscard]] inline std::vector<std::string> ParseActivePluginList(
    std::string_view aContent,
    bool aInjectMissingBaseMastersInPluginsTxt = false)
{
    struct RawEntry
    {
        std::string Name{};
        bool HasStar{false};
    };

    std::vector<RawEntry> entries;
    bool anyStarLine = false;
    bool anyStarredNonBaseLine = false;
    bool firstLine = true;

    std::istringstream iss{std::string(aContent)};
    std::string line;
    while (std::getline(iss, line))
    {
        if (firstLine)
        {
            firstLine = false;
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
            {
                line.erase(0, 3);
            }
        }

        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        std::string trimmed = Detail::Trim(line);
        if (trimmed.empty() || trimmed.front() == '#')
            continue;

        bool hasStar = false;
        if (trimmed.front() == '*')
        {
            hasStar = true;
            anyStarLine = true;
            trimmed = Detail::Trim(std::string_view(trimmed).substr(1));
        }

        if (trimmed.empty())
            continue;

        if (hasStar && !IsImplicitSseBaseMaster(trimmed))
            anyStarredNonBaseLine = true;

        entries.push_back(RawEntry{std::move(trimmed), hasStar});
    }

    const bool filterUnstarred = aInjectMissingBaseMastersInPluginsTxt ? anyStarLine : anyStarredNonBaseLine;

    std::vector<std::string> active;
    std::unordered_set<std::string> seenLower;

    auto appendUnique = [&](const std::string& name)
    {
        const std::string lower = Detail::ToLowerAscii(name);
        if (!lower.empty() && seenLower.insert(lower).second)
            active.push_back(name);
    };

    if (filterUnstarred && aInjectMissingBaseMastersInPluginsTxt)
    {
        static constexpr std::string_view kBaseMasters[] = {
            "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"
        };
        for (const auto& baseMaster : kBaseMasters)
            appendUnique(std::string(baseMaster));
    }

    for (const auto& entry : entries)
    {
        if (filterUnstarred)
        {
            // SSE `plugins.txt` mode: lines without `*` are disabled plugins in the user's MO2/Vortex profile,
            // except implicit SSE base masters which vanilla Skyrim SE may omit `*` on.
            if (!entry.HasStar && !IsImplicitSseBaseMaster(entry.Name))
                continue;
        }
        appendUnique(entry.Name);
    }

    return active;
}
} // namespace RfabCompat
