#pragma once

// Co-op view of third-party mod configuration (architecture step 5).
//
// RFAB Online needs a handful of other mods to behave differently while the co-op client runs:
// EngineFixes must leave the allocator to Skyrim Together, Skyrim Souls must not slow the local
// game down in menus, RFAB's own autosave must not save outside the coordinated save, po3 Tweaks
// must keep editor ids, and the BetterStealing .NET plugin must not load (it rewrites the same
// add-item prologues the client hooks).
//
// Earlier releases rewrote those files on disk: the launcher on every start (and again from inside
// the DLL loader lock, when EngineFixes.dll loaded) and the installer in Data/, MO2/overwrite and
// every MO2/mods/* folder, plus renaming BetterStealing.dll to .mohidden. Solo RFAB silently
// changed too and an RFAB update was silently re-patched. This header replaces all of that with
// one declarative rule table:
//
//   * a rule names a file relative to <game>/Data and either an OVERLAY (a pure transform of the
//     original text) or HIDE;
//   * at launch the launcher reads the file the game would read (MO2's virtual Data resolves the
//     winning mod), applies the transform and, only if something changes, writes the result into
//     its OWN folder;
//   * inside the co-op process, opens of that Data path are redirected to the overlay (or fail
//     with "file not found" for HIDE). Nothing outside the RFAB Online folder is ever written.
//
// Everything here is portable and side-effect free except PrepareOverlays (which only writes into
// the overlay directory it is given); the Windows redirect lives in the launcher
// (immersive_launcher/stubs/ConfigOverlayRedirect.cpp). Tools/rfab/Setup-RfabOnline.ps1 undoes the
// on-disk edits of earlier releases using the same ManagedKeys lists (cross-checked by
// Tools/rfab/check_release_invariants.py).

#include <common/RfabModpackCompat.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace Compat
{
enum class RuleKind : uint8_t
{
    kOverlay, // the co-op process reads a transformed copy
    kHide,    // the co-op process cannot open the file at all
};

struct Rule
{
    std::string_view Id;               // stable short name, used in logs and the report
    std::string_view DataRelativePath; // as under <game>/Data, forward slashes, original case
    RuleKind Kind;
    // kOverlay only: pure and idempotent (Transform(Transform(x)) == Transform(x)).
    std::string (*Transform)(std::string_view aOriginal);
    std::string_view Reason;
    // kOverlay only, lower-case key names (section-agnostic): every key RFAB Online has ever
    // written into this file, by this transform or by an earlier release. The transform may only
    // change these; the installer's undo treats a file that differs from its backup only in these
    // keys as "ours" and restores the backup.
    const std::string_view* pManagedKeys{nullptr};
    size_t ManagedKeyCount{0};

    [[nodiscard]] bool IsManagedKey(std::string_view aKey) const noexcept;
};

// ------------------------------------------------------------------------------------------------
// INI editing that keeps everything it does not touch (comments, order, line endings).
// ------------------------------------------------------------------------------------------------

namespace Detail
{
inline char AsciiLower(char aChar) noexcept
{
    return (aChar >= 'A' && aChar <= 'Z') ? static_cast<char>(aChar - 'A' + 'a') : aChar;
}

inline bool EqualsIgnoreCase(std::string_view aLeft, std::string_view aRight) noexcept
{
    if (aLeft.size() != aRight.size())
        return false;
    for (size_t i = 0; i < aLeft.size(); ++i)
        if (AsciiLower(aLeft[i]) != AsciiLower(aRight[i]))
            return false;
    return true;
}

inline std::string_view TrimView(std::string_view aView) noexcept
{
    // A UTF-8 BOM in front of the first line (stock SkyrimSoulsRE.ini and po3_Tweaks.ini have one:
    // "\xEF\xBB\xBF[Fixes]") belongs to the file, not to the line's syntax. Output keeps it, since
    // lines are re-emitted from the original text.
    if (aView.size() >= 3 && static_cast<unsigned char>(aView[0]) == 0xEF && static_cast<unsigned char>(aView[1]) == 0xBB &&
        static_cast<unsigned char>(aView[2]) == 0xBF)
        aView.remove_prefix(3);
    while (!aView.empty() && (aView.front() == ' ' || aView.front() == '\t'))
        aView.remove_prefix(1);
    while (!aView.empty() && (aView.back() == ' ' || aView.back() == '\t' || aView.back() == '\r'))
        aView.remove_suffix(1);
    return aView;
}

struct IniLine
{
    std::string_view Text;  // without the line break
    std::string_view Break; // "\r\n", "\n" or "" for the last line
};

inline std::vector<IniLine> SplitLines(std::string_view aContent)
{
    std::vector<IniLine> lines;
    size_t pos = 0;
    while (pos < aContent.size())
    {
        const size_t newline = aContent.find('\n', pos);
        if (newline == std::string_view::npos)
        {
            lines.push_back({aContent.substr(pos), {}});
            break;
        }
        const bool crlf = newline > pos && aContent[newline - 1] == '\r';
        const size_t textEnd = crlf ? newline - 1 : newline;
        lines.push_back({aContent.substr(pos, textEnd - pos), aContent.substr(textEnd, newline + 1 - textEnd)});
        pos = newline + 1;
    }
    return lines;
}

// "[Section]" -> "Section"; empty when the line is not a header.
inline std::string_view SectionOf(std::string_view aLine) noexcept
{
    const std::string_view trimmed = TrimView(aLine);
    if (trimmed.size() < 2 || trimmed.front() != '[')
        return {};
    const size_t close = trimmed.find(']');
    if (close == std::string_view::npos)
        return {};
    return TrimView(trimmed.substr(1, close - 1));
}

// "Key = Value ; comment" -> Key / Value (comment stripped); false for comments, headers, blanks.
inline bool ParseKeyValue(std::string_view aLine, std::string_view& aKey, std::string_view& aValue) noexcept
{
    const std::string_view trimmed = TrimView(aLine);
    if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' || trimmed.front() == '[')
        return false;
    const size_t equals = trimmed.find('=');
    if (equals == std::string_view::npos)
        return false;
    aKey = TrimView(trimmed.substr(0, equals));
    std::string_view value = trimmed.substr(equals + 1);
    // An inline comment needs whitespace before it, so values such as "a;b" survive.
    for (size_t i = 1; i < value.size(); ++i)
    {
        if ((value[i] == ';' || value[i] == '#') && (value[i - 1] == ' ' || value[i - 1] == '\t'))
        {
            value = value.substr(0, i);
            break;
        }
    }
    aValue = TrimView(value);
    return !aKey.empty();
}

inline std::string_view PreferredBreak(const std::vector<IniLine>& acLines) noexcept
{
    for (const auto& line : acLines)
        if (!line.Break.empty())
            return line.Break;
    return "\n";
}
} // namespace Detail

/**
 * Sets `aKey = aValue` inside `[aSection]` (names compared case-insensitively). Every existing
 * assignment of the key in that section is rewritten in place, keeping its indentation and inline
 * comment; a missing key is added after the section's last key; a missing section is appended.
 * Everything else is left byte-for-byte as it was. `aSection` must not be empty.
 */
inline std::string SetIniValue(std::string_view aContent, std::string_view aSection, std::string_view aKey,
                               std::string_view aValue)
{
    const std::vector<Detail::IniLine> lines = Detail::SplitLines(aContent);
    const std::string_view newline = Detail::PreferredBreak(lines);
    constexpr size_t kNone = static_cast<size_t>(-1);

    std::vector<bool> isKeyLine(lines.size(), false);
    bool anyKey = false;
    size_t headerLine = kNone;
    size_t lastKeyInSection = kNone;
    bool inSection = false;
    bool inFirstBlock = false; // a missing key goes after the last key of the section's first block
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::string_view header = Detail::SectionOf(lines[i].Text);
        if (!header.empty())
        {
            inSection = Detail::EqualsIgnoreCase(header, aSection);
            inFirstBlock = inSection && headerLine == kNone;
            if (inFirstBlock)
                headerLine = i;
            continue;
        }
        std::string_view key, value;
        if (!inSection || !Detail::ParseKeyValue(lines[i].Text, key, value))
            continue;
        if (inFirstBlock)
            lastKeyInSection = i;
        if (Detail::EqualsIgnoreCase(key, aKey))
        {
            isKeyLine[i] = true;
            anyKey = true;
        }
    }

    std::string out;
    out.reserve(aContent.size() + aSection.size() + aKey.size() + aValue.size() + 16);
    const std::string keyLine = std::string(aKey) + " = " + std::string(aValue);

    if (anyKey)
    {
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const std::string_view text = lines[i].Text;
            std::string_view key, oldValue;
            if (!isKeyLine[i] || !Detail::ParseKeyValue(text, key, oldValue) || oldValue == aValue)
            {
                out.append(text);
                out.append(lines[i].Break);
                continue;
            }
            const size_t equals = text.find('=');
            const size_t valueStart = text.find_first_not_of(" \t", equals + 1);
            const size_t valueEnd = valueStart == std::string_view::npos ? text.size() : (std::min)(text.size(), valueStart + oldValue.size());
            std::string_view rest = text.substr(valueEnd);
            out.append(text.substr(0, equals + 1));
            out.push_back(' ');
            out.append(aValue);
            if (!rest.empty() && (rest.front() == ';' || rest.front() == '#'))
                out.push_back(' ');
            out.append(rest);
            out.append(lines[i].Break);
        }
        return out;
    }

    if (headerLine == kNone)
    {
        out.assign(aContent);
        if (!out.empty() && out.back() != '\n')
            out.append(newline);
        out.push_back('[');
        out.append(aSection);
        out.push_back(']');
        out.append(newline);
        out.append(keyLine);
        out.append(newline);
        return out;
    }

    const size_t insertAfter = lastKeyInSection != kNone ? lastKeyInSection : headerLine;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        out.append(lines[i].Text);
        out.append(lines[i].Break);
        if (i == insertAfter)
        {
            if (lines[i].Break.empty())
                out.append(newline);
            out.append(keyLine);
            if (i + 1 < lines.size() || !lines[i].Break.empty())
                out.append(newline);
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// The rule table.
// ------------------------------------------------------------------------------------------------

namespace Transforms
{
inline std::string EngineFixes(std::string_view aOriginal)
{
    std::string content(aOriginal);
    RfabCompat::PatchEngineFixesTomlContent(content);
    return content;
}

inline std::string SkyrimSouls(std::string_view aOriginal)
{
    std::string content(aOriginal);
    RfabCompat::PatchSkyrimSoulsReIniContent(content);
    return content;
}

inline std::string RfabAutosave(std::string_view aOriginal)
{
    return RfabCompat::SuppressRfabAutosaveIniContent(aOriginal);
}

inline std::string Po3Tweaks(std::string_view aOriginal)
{
    return SetIniValue(aOriginal, "Fixes", "Load EditorIDs", "true");
}
} // namespace Transforms

inline constexpr std::string_view kEngineFixesKeys[] = {
    // v5/v6 [Patches] / [Experimental]
    "memorymanager", "scaleformallocator", "formcaching", "treelodreferencecaching", "regularquicksaves",
    "maxstdio", "savegamemaxsize", "cleansksecosaves",
    // v7 [MemoryManager] and b/i-prefixed keys
    "overridevanilla", "overridescaleform", "overridescrapheap", "bmemorymanager", "bscaleformallocator",
    "boverridescrapheap", "isavegamemaxsize", "bregularquicksaves", "imaxstdio", "bformcaching",
    "btreelodreferencecaching",
    // junk lines an early release wrote and later releases heal ("S = a", "t = r", "f = a")
    "s", "t", "f"};
inline constexpr std::string_view kSkyrimSoulsKeys[] = {
    "fslowmotionmultiplier", "bstatsmenu", "benableslowmotion*"};
inline constexpr std::string_view kRfabAutosaveKeys[] = {
    "enabled", "enable autosave", "save in combat", "save after combat", "save on location change", "save on sleep/wait"};
inline constexpr std::string_view kPo3TweaksKeys[] = {"load editorids"};

inline bool Rule::IsManagedKey(std::string_view aKey) const noexcept
{
    for (size_t i = 0; i < ManagedKeyCount; ++i)
    {
        std::string_view managed = pManagedKeys[i];
        if (!managed.empty() && managed.back() == '*')
        {
            managed.remove_suffix(1);
            if (aKey.size() >= managed.size() && Detail::EqualsIgnoreCase(aKey.substr(0, managed.size()), managed))
                return true;
        }
        else if (Detail::EqualsIgnoreCase(aKey, managed))
        {
            return true;
        }
    }
    return false;
}

inline constexpr size_t kRuleCount = 5;

inline const std::array<Rule, kRuleCount>& GetRules() noexcept
{
    static const std::array<Rule, kRuleCount> s_rules{{
        {"enginefixes", "SKSE/Plugins/EngineFixes.toml", RuleKind::kOverlay, &Transforms::EngineFixes,
         "Skyrim Together owns the allocator (MemoryManager/ScaleformAllocator off), FormCaching/TreeLOD caching "
         "crash on recycled 0xFF ids, MaxStdio 8192 for RFAB's archives, SaveGameMaxSize off without the memory manager",
         kEngineFixesKeys, std::size(kEngineFixesKeys)},
        {"skyrimsouls", "SKSE/Plugins/SkyrimSoulsRE.ini", RuleKind::kOverlay, &Transforms::SkyrimSouls,
         "menus must not slow the local game down in real-time co-op; StatsMenu stays paused for CustomSkills/RFAB perks",
         kSkyrimSoulsKeys, std::size(kSkyrimSoulsKeys)},
        {"rfab-autosave", "SKSE/Plugins/[RFAB] Autosave.ini", RuleKind::kOverlay, &Transforms::RfabAutosave,
         "RFAB's own autosave would save outside the coordinated co-op save", kRfabAutosaveKeys,
         std::size(kRfabAutosaveKeys)},
        {"po3-tweaks", "SKSE/Plugins/po3_Tweaks.ini", RuleKind::kOverlay, &Transforms::Po3Tweaks,
         "the client matches items and spells by editor id", kPo3TweaksKeys, std::size(kPo3TweaksKeys)},
        {"betterstealing", "NetScriptFramework/Plugins/BetterStealing.dll", RuleKind::kHide, nullptr,
         ".NET plugin patches the add-item prologues (SE 36525/19282) the client hooks; NetScriptFramework logs and "
         "skips a plugin it cannot open"},
    }};
    return s_rules;
}

inline const Rule* FindRule(std::string_view aId) noexcept
{
    for (const auto& rule : GetRules())
        if (rule.Id == aId)
            return &rule;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// Path matching (called from the CreateFileW redirect for every file the game opens: no allocation).
// ------------------------------------------------------------------------------------------------

namespace Detail
{
template <class TChar> constexpr char NormalizePathChar(TChar aChar) noexcept
{
    const auto code = static_cast<uint32_t>(aChar);
    if (code == '/' || code == '\\')
        return '\\';
    if (code >= 'A' && code <= 'Z')
        return static_cast<char>(code - 'A' + 'a');
    if (code > 0x7F)
        return '\x01'; // never equal to an ASCII rule character
    return static_cast<char>(code);
}
} // namespace Detail

/**
 * Does `aPath` (as handed to CreateFileW: relative or absolute, `\\?\` prefixed or not, either
 * slash, any case, repeated separators) name `<something>\Data\<aDataRelative>`? The component in
 * front of the relative part must be exactly "Data", so the real MO2 mod folder
 * (`mods\X\SKSE\Plugins\...`) and the overlay directory itself never match.
 */
template <class TChar>
[[nodiscard]] bool PathNamesDataFile(std::basic_string_view<TChar> aPath, std::string_view aDataRelative) noexcept
{
    size_t i = aPath.size();
    size_t j = aDataRelative.size();
    if (j == 0 || i <= j)
        return false;

    auto at = [&](size_t aIndex) { return Detail::NormalizePathChar(aPath[aIndex]); };

    while (j > 0)
    {
        if (i == 0)
            return false;
        const char expected = Detail::NormalizePathChar(aDataRelative[j - 1]);
        const char actual = at(i - 1);
        if (expected != actual)
            return false;
        --i;
        --j;
        if (expected == '\\')
            while (i > 0 && at(i - 1) == '\\')
                --i;
    }

    // Exactly one "Data" component in front.
    if (i == 0 || at(i - 1) != '\\')
        return false;
    while (i > 0 && at(i - 1) == '\\')
        --i;
    if (i < 4)
        return false;
    if (at(i - 4) != 'd' || at(i - 3) != 'a' || at(i - 2) != 't' || at(i - 1) != 'a')
        return false;
    return i == 4 || at(i - 5) == '\\';
}

// ------------------------------------------------------------------------------------------------
// Preparation (launcher, once, before the game loads).
// ------------------------------------------------------------------------------------------------

enum class OverlayStatus : uint8_t
{
    kAbsent,    // the game has no such file: nothing to do
    kUnchanged, // the file already says what co-op needs: no redirect
    kOverlaid,  // a co-op copy was written; opens are redirected to it
    kHidden,    // HIDE rule and the file exists: opens fail with "not found"
    kFailed,    // the file exists but the co-op copy could not be prepared
};

inline const char* ToString(OverlayStatus aStatus) noexcept
{
    switch (aStatus)
    {
    case OverlayStatus::kAbsent: return "absent";
    case OverlayStatus::kUnchanged: return "unchanged";
    case OverlayStatus::kOverlaid: return "overlaid";
    case OverlayStatus::kHidden: return "hidden";
    case OverlayStatus::kFailed: return "FAILED";
    }
    return "?";
}

struct KeyChange
{
    std::string Key; // "[Section] Key"
    std::string From;
    std::string To; // empty From: the key was added; empty To: the key was removed
};

struct OverlayOutcome
{
    const Rule* pRule{nullptr};
    OverlayStatus Status{OverlayStatus::kAbsent};
    std::filesystem::path Source;
    std::filesystem::path Overlay;
    std::vector<KeyChange> Changes;
    std::string Error;

    [[nodiscard]] bool Redirects() const noexcept { return Status == OverlayStatus::kOverlaid || Status == OverlayStatus::kHidden; }
};

/**
 * Key-level difference between two INI/TOML texts, for the report ("what does co-op change?").
 * Keys are qualified by section; only changed, added and removed keys are listed.
 */
inline std::vector<KeyChange> DiffKeys(std::string_view aBefore, std::string_view aAfter)
{
    struct Entry
    {
        std::string Key;
        std::string Value;
    };
    auto collect = [](std::string_view aText)
    {
        std::vector<Entry> entries;
        std::string section;
        for (const auto& line : Detail::SplitLines(aText))
        {
            const std::string_view header = Detail::SectionOf(line.Text);
            if (!header.empty())
            {
                section.assign(header);
                continue;
            }
            std::string_view key, value;
            if (!Detail::ParseKeyValue(line.Text, key, value))
                continue;
            std::string qualified = section.empty() ? std::string(key) : "[" + section + "] " + std::string(key);
            bool replaced = false;
            for (auto& entry : entries)
            {
                if (Detail::EqualsIgnoreCase(entry.Key, qualified))
                {
                    entry.Value.assign(value); // the last assignment wins, as in every INI reader
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                entries.push_back({std::move(qualified), std::string(value)});
        }
        return entries;
    };

    const auto before = collect(aBefore);
    const auto after = collect(aAfter);
    std::vector<KeyChange> changes;
    for (const auto& entry : after)
    {
        const Entry* pOld = nullptr;
        for (const auto& old : before)
            if (Detail::EqualsIgnoreCase(old.Key, entry.Key))
                pOld = &old;
        if (!pOld)
            changes.push_back({entry.Key, {}, entry.Value});
        else if (pOld->Value != entry.Value)
            changes.push_back({entry.Key, pOld->Value, entry.Value});
    }
    for (const auto& old : before)
    {
        bool present = false;
        for (const auto& entry : after)
            if (Detail::EqualsIgnoreCase(old.Key, entry.Key))
                present = true;
        if (!present)
            changes.push_back({old.Key, old.Value, {}});
    }
    return changes;
}

/**
 * Reads every rule's file under `aDataRoot` (the game's Data folder; under MO2 the virtual one, so
 * the winning mod's copy is read), and for overlay rules writes the transformed text into
 * `aOverlayDir` when it differs. Never writes anywhere else. Overlay files left over from an
 * earlier launch are removed first, so a rule that is no longer needed cannot redirect.
 */
inline std::vector<OverlayOutcome> PrepareOverlays(const std::filesystem::path& aDataRoot, const std::filesystem::path& aOverlayDir)
{
    std::vector<OverlayOutcome> outcomes;
    outcomes.reserve(kRuleCount);
    std::error_code ec;

    for (const auto& rule : GetRules())
    {
        OverlayOutcome outcome{};
        outcome.pRule = &rule;
        outcome.Source = aDataRoot / std::filesystem::path(std::string(rule.DataRelativePath));
        outcome.Overlay = aOverlayDir / outcome.Source.filename();

        if (rule.Kind == RuleKind::kOverlay)
            std::filesystem::remove(outcome.Overlay, ec);
        ec.clear();

        const bool exists = std::filesystem::is_regular_file(outcome.Source, ec);
        if (!exists)
        {
            outcome.Status = OverlayStatus::kAbsent;
            outcomes.push_back(std::move(outcome));
            continue;
        }

        if (rule.Kind == RuleKind::kHide)
        {
            outcome.Status = OverlayStatus::kHidden;
            outcomes.push_back(std::move(outcome));
            continue;
        }

        std::string original;
        if (!RfabCompat::Detail::ReadTextFile(outcome.Source, original))
        {
            outcome.Status = OverlayStatus::kFailed;
            outcome.Error = "cannot read the file the game would read";
            outcomes.push_back(std::move(outcome));
            continue;
        }

        std::string transformed;
        try
        {
            transformed = rule.Transform(original);
        }
        catch (...)
        {
            outcome.Status = OverlayStatus::kFailed;
            outcome.Error = "transform threw";
            outcomes.push_back(std::move(outcome));
            continue;
        }

        // Byte-identical, or different only in spacing/comments: the mod already reads what co-op
        // needs, so nothing is redirected and the mod's own file stays in effect.
        if (transformed != original)
            outcome.Changes = DiffKeys(original, transformed);
        if (outcome.Changes.empty())
        {
            outcome.Status = OverlayStatus::kUnchanged;
            outcomes.push_back(std::move(outcome));
            continue;
        }

        std::string written;
        if (!RfabCompat::Detail::WriteTextFile(outcome.Overlay, transformed) ||
            !RfabCompat::Detail::ReadTextFile(outcome.Overlay, written) || written != transformed)
        {
            std::filesystem::remove(outcome.Overlay, ec);
            outcome.Status = OverlayStatus::kFailed;
            outcome.Error = "cannot write the co-op copy into the RFAB Online folder";
            outcomes.push_back(std::move(outcome));
            continue;
        }

        outcome.Status = OverlayStatus::kOverlaid;
        outcomes.push_back(std::move(outcome));
    }
    return outcomes;
}

inline const OverlayOutcome* FindOutcome(const std::vector<OverlayOutcome>& acOutcomes, std::string_view aRuleId) noexcept
{
    for (const auto& outcome : acOutcomes)
        if (outcome.pRule && outcome.pRule->Id == aRuleId)
            return &outcome;
    return nullptr;
}

/**
 * May EngineFixes.dll load? Only when what it will read is safe for Skyrim Together: the file is
 * already compliant, absent (its built-in defaults are what upstream ships with Together), or
 * overlaid AND the redirect is actually installed. Otherwise loading it would run two allocators.
 */
[[nodiscard]] inline bool IsEngineFixesSafe(const std::vector<OverlayOutcome>& acOutcomes, bool aRedirectInstalled) noexcept
{
    const OverlayOutcome* pOutcome = FindOutcome(acOutcomes, "enginefixes");
    if (!pOutcome)
        return false;
    switch (pOutcome->Status)
    {
    case OverlayStatus::kAbsent:
    case OverlayStatus::kUnchanged: return true;
    case OverlayStatus::kOverlaid: return aRedirectInstalled;
    default: return false;
    }
}

/** Human-readable report: one block per rule, written to the overlay folder and the log. */
inline std::string FormatReport(const std::vector<OverlayOutcome>& acOutcomes, bool aRedirectInstalled)
{
    std::string report;
    report += "RFAB Online - co-op view of other mods' settings\n";
    report += "Files outside the RFAB Online folder are never modified; only this process reads the copies below.\n";
    report += aRedirectInstalled ? "Redirect: installed\n" : "Redirect: NOT installed (no copy below is in effect)\n";
    for (const auto& outcome : acOutcomes)
    {
        if (!outcome.pRule)
            continue;
        report += "\n[";
        report += outcome.pRule->Id;
        report += "] Data/";
        report += outcome.pRule->DataRelativePath;
        report += ": ";
        report += ToString(outcome.Status);
        report += "\n  why: ";
        report += outcome.pRule->Reason;
        report += "\n";
        if (!outcome.Error.empty())
        {
            report += "  error: ";
            report += outcome.Error;
            report += "\n";
        }
        for (const auto& change : outcome.Changes)
        {
            report += "  ";
            report += change.Key;
            report += ": ";
            report += change.From.empty() ? std::string("(absent)") : change.From;
            report += " -> ";
            report += change.To.empty() ? std::string("(removed)") : change.To;
            report += "\n";
        }
    }
    return report;
}
} // namespace Compat
