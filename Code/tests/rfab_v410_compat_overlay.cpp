// Architecture step 5: co-op view of other mods' settings + one menu pause policy.
//
// Covers the portable halves of common/Compat/ConfigOverlay.h (INI editing, rule table, path
// matching used by the launcher's CreateFile redirect, overlay preparation) and
// common/Compat/MenuPausePolicy.h, against the byte-exact stock files of the RFAB modpack.

#include <catch2/catch.hpp>

#include <common/Compat/ConfigOverlay.h>
#include <common/Compat/MenuPausePolicy.h>

#include "RfabStockConfigs.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace
{
std::string ValueOf(std::string_view aContent, std::string_view aSection, std::string_view aKey)
{
    std::string section;
    std::string found = "<missing>";
    for (const auto& line : Compat::Detail::SplitLines(aContent))
    {
        const std::string_view header = Compat::Detail::SectionOf(line.Text);
        if (!header.empty())
        {
            section.assign(header);
            continue;
        }
        std::string_view key, value;
        if (Compat::Detail::ParseKeyValue(line.Text, key, value) && Compat::Detail::EqualsIgnoreCase(section, aSection) &&
            Compat::Detail::EqualsIgnoreCase(key, aKey))
            found.assign(value);
    }
    return found;
}

std::string UnqualifiedKey(const std::string& aQualified)
{
    const size_t close = aQualified.find("] ");
    return close == std::string::npos ? aQualified : aQualified.substr(close + 2);
}

// Every key a transform changes must be declared in its rule's ManagedKeys (the installer's undo
// relies on that list), and a second application must change nothing.
void RequireWellBehaved(const Compat::Rule& acRule, std::string_view aInput)
{
    INFO("rule " << acRule.Id);
    const std::string once = acRule.Transform(aInput);
    const std::string twice = acRule.Transform(once);
    REQUIRE(twice == once);
    for (const auto& change : Compat::DiffKeys(aInput, once))
    {
        INFO("changed key " << change.Key);
        REQUIRE(acRule.IsManagedKey(UnqualifiedKey(change.Key)));
    }
}

void WriteFile(const fs::path& acPath, std::string_view aContent)
{
    fs::create_directories(acPath.parent_path());
    std::ofstream out(acPath, std::ios::binary | std::ios::trunc);
    out.write(aContent.data(), static_cast<std::streamsize>(aContent.size()));
}

std::string ReadFile(const fs::path& acPath)
{
    std::string content;
    REQUIRE(RfabCompat::Detail::ReadTextFile(acPath, content));
    return content;
}

struct TempTree
{
    fs::path Root;
    explicit TempTree(const char* apName)
        : Root(fs::temp_directory_path() / apName)
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
        fs::create_directories(Root);
    }
    ~TempTree()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
};
} // namespace

TEST_CASE("SetIniValue edits one key and leaves every other byte alone", "[rfab][compat][overlay]")
{
    SECTION("existing key: value replaced, indentation, inline comment and CRLF kept")
    {
        const std::string in = "; head\r\n[Fixes]\r\n  Load EditorIDs = false   ; needed\r\nOther = 1\r\n";
        const std::string out = Compat::SetIniValue(in, "fixes", "load editorids", "true");
        REQUIRE(out == "; head\r\n[Fixes]\r\n  Load EditorIDs = true   ; needed\r\nOther = 1\r\n");
    }
    SECTION("equal value: byte-identical")
    {
        const std::string in = "[Fixes]\nLoad EditorIDs=true\n";
        REQUIRE(Compat::SetIniValue(in, "Fixes", "Load EditorIDs", "true") == in);
    }
    SECTION("missing key: added after the section's last key, not after trailing comments")
    {
        const std::string in = "[A]\nx = 1\ny = 2\n\n; about B\n[B]\nz = 3\n";
        REQUIRE(Compat::SetIniValue(in, "A", "k", "v") == "[A]\nx = 1\ny = 2\nk = v\n\n; about B\n[B]\nz = 3\n");
    }
    SECTION("missing key in an empty section, last line without newline")
    {
        REQUIRE(Compat::SetIniValue("[A]", "A", "k", "v") == "[A]\nk = v");
        REQUIRE(Compat::SetIniValue("[A]\r\n", "A", "k", "v") == "[A]\r\nk = v\r\n");
    }
    SECTION("missing section: appended with the file's line ending")
    {
        REQUIRE(Compat::SetIniValue("[A]\r\nx = 1", "B", "k", "v") == "[A]\r\nx = 1\r\n[B]\r\nk = v\r\n");
        REQUIRE(Compat::SetIniValue("", "B", "k", "v") == "[B]\nk = v\n");
    }
    SECTION("same key in another section is not touched; duplicates in the section all rewritten")
    {
        const std::string in = "[A]\nk = 0\n[B]\nk = 0\n[A]\nk = 0\n";
        REQUIRE(Compat::SetIniValue(in, "A", "k", "1") == "[A]\nk = 1\n[B]\nk = 0\n[A]\nk = 1\n");
    }
    SECTION("value containing ';' without whitespace is a value, not a comment")
    {
        REQUIRE(ValueOf("[A]\nk = a;b ; c\n", "A", "k") == "a;b");
        REQUIRE(Compat::SetIniValue("[A]\nk = a;b ; c\n", "A", "k", "x") == "[A]\nk = x ; c\n");
    }
    SECTION("UTF-8 BOM in front of the first section header (stock po3_Tweaks.ini)")
    {
        const std::string in = "\xEF\xBB\xBF[Fixes]\nLoad EditorIDs = false\n[Tweaks]\n";
        REQUIRE(Compat::SetIniValue(in, "Fixes", "Load EditorIDs", "true") == "\xEF\xBB\xBF[Fixes]\nLoad EditorIDs = true\n[Tweaks]\n");
        const std::string missing = "\xEF\xBB\xBF[Fixes]\nA = 1\n";
        REQUIRE(Compat::SetIniValue(missing, "Fixes", "Load EditorIDs", "true") == "\xEF\xBB\xBF[Fixes]\nA = 1\nLoad EditorIDs = true\n");
    }
}

TEST_CASE("DiffKeys reports semantic changes only", "[rfab][compat][overlay]")
{
    REQUIRE(Compat::DiffKeys("[A]\nk=1 ; c\n", "[A]\n  k = 1   # other comment\n").empty());
    const auto changes = Compat::DiffKeys("[A]\nk = 1\njunk = x\n", "[A]\nk = 2\n[B]\nn = 3\n");
    REQUIRE(changes.size() == 3);
    std::set<std::string> keys;
    for (const auto& change : changes)
        keys.insert(change.Key + "|" + change.From + "|" + change.To);
    REQUIRE(keys.count("[A] k|1|2") == 1);
    REQUIRE(keys.count("[B] n||3") == 1);
    REQUIRE(keys.count("[A] junk|x|") == 1);
}

TEST_CASE("Rule table: shape and stable ids", "[rfab][compat][overlay]")
{
    std::set<std::string_view> ids;
    for (const auto& rule : Compat::GetRules())
    {
        REQUIRE(ids.insert(rule.Id).second);
        REQUIRE(!rule.DataRelativePath.empty());
        REQUIRE(rule.DataRelativePath.find('\\') == std::string_view::npos);
        REQUIRE(!rule.Reason.empty());
        if (rule.Kind == Compat::RuleKind::kOverlay)
        {
            REQUIRE(rule.Transform != nullptr);
            REQUIRE(rule.ManagedKeyCount > 0);
        }
        else
        {
            REQUIRE(rule.Transform == nullptr);
        }
    }
    for (const char* id : {"enginefixes", "skyrimsouls", "rfab-autosave", "po3-tweaks", "betterstealing"})
        REQUIRE(Compat::FindRule(id) != nullptr);
    REQUIRE(Compat::FindRule("betterstealing")->Kind == Compat::RuleKind::kHide);
    REQUIRE(Compat::FindRule("skyrimsouls")->IsManagedKey("bEnableSlowMotion_MessageBoxMenu"));
    REQUIRE_FALSE(Compat::FindRule("skyrimsouls")->IsManagedKey("bInventoryMenu"));
}

TEST_CASE("Stock RFAB configs: exactly the co-op differences, nothing else", "[rfab][compat][overlay]")
{
    using namespace RfabStockConfigs;

    SECTION("SkyrimSoulsRE.ini: slow motion off; menus, StatsMenu and BOM untouched")
    {
        const Compat::Rule& rule = *Compat::FindRule("skyrimsouls");
        RequireWellBehaved(rule, kStockSkyrimSoulsIni);
        const std::string out = rule.Transform(kStockSkyrimSoulsIni);
        REQUIRE(out.rfind("\xEF\xBB\xBF", 0) == 0);
        const auto changes = Compat::DiffKeys(kStockSkyrimSoulsIni, out);
        std::set<std::string> keys;
        for (const auto& change : changes)
            keys.insert(change.Key);
        REQUIRE(keys == std::set<std::string>{"[SLOWMOTION] fSlowMotionMultiplier", "[SLOWMOTION] bEnableSlowMotion_MessageBoxMenu"});
        REQUIRE(ValueOf(out, "UNPAUSED_MENUS", "bStatsMenu") == "false");
        REQUIRE(ValueOf(out, "UNPAUSED_MENUS", "bInventoryMenu") == "true");
        REQUIRE(ValueOf(out, "SLOWMOTION", "fSlowMotionMultiplier") == "1.000000");
    }

    SECTION("EngineFixes.toml (SSE Engine Fixes and the Disable Quick Saves override)")
    {
        const Compat::Rule& rule = *Compat::FindRule("enginefixes");
        for (std::string_view stock : {kStockEngineFixesToml, kStockDisableQuickSavesToml})
        {
            RequireWellBehaved(rule, stock);
            const std::string out = rule.Transform(stock);
            REQUIRE(ValueOf(out, "Patches", "MemoryManager") == "false");
            REQUIRE(ValueOf(out, "Patches", "ScaleformAllocator") == "false");
            REQUIRE(ValueOf(out, "Patches", "FormCaching") == "false");
            REQUIRE(ValueOf(out, "Patches", "TreeLODReferenceCaching") == "false");
            REQUIRE(ValueOf(out, "Patches", "MaxStdio") == "8192");
            REQUIRE(ValueOf(out, "Patches", "RegularQuicksaves") == "true");
            REQUIRE(ValueOf(out, "Experimental", "SaveGameMaxSize") == "false");
            // Settings co-op does not care about keep RFAB's values.
            REQUIRE(ValueOf(out, "EngineFixes", "VerboseLogging") == ValueOf(stock, "EngineFixes", "VerboseLogging"));
            REQUIRE(ValueOf(out, "Patches", "SleepWaitTime") == ValueOf(stock, "Patches", "SleepWaitTime"));
        }
    }

    SECTION("[RFAB] Autosave.ini: only 'Save after combat' differs (autosave is already off in stock)")
    {
        const Compat::Rule& rule = *Compat::FindRule("rfab-autosave");
        RequireWellBehaved(rule, kStockRfabAutosaveIni);
        const auto changes = Compat::DiffKeys(kStockRfabAutosaveIni, rule.Transform(kStockRfabAutosaveIni));
        std::set<std::string> keys;
        for (const auto& change : changes)
            keys.insert(change.Key + "=" + change.To);
        REQUIRE(keys == std::set<std::string>{"[RFAB Autosave] Save after combat=0", "[RFAB Autosave Default] Save after combat=0"});
    }

    SECTION("po3_Tweaks.ini: already compliant, nothing to overlay")
    {
        const Compat::Rule& rule = *Compat::FindRule("po3-tweaks");
        REQUIRE(rule.Transform(kStockPo3TweaksIni) == std::string(kStockPo3TweaksIni));
        const std::string off = Compat::SetIniValue(kStockPo3TweaksIni, "Fixes", "Load EditorIDs", "false");
        REQUIRE(off != std::string(kStockPo3TweaksIni));
        RequireWellBehaved(rule, off);
        REQUIRE(rule.Transform(off) == std::string(kStockPo3TweaksIni));
    }

    SECTION("legacy-damaged inputs: junk lines and the misplaced [SaveGameMaxSize] section")
    {
        const Compat::Rule& rule = *Compat::FindRule("enginefixes");
        const std::string damaged = "[Patches]\r\nMemoryManager = true\r\n[Experimental]\r\nS = a\r\nt = r\r\n[SaveGameMaxSize]\r\nSaveGameMaxSize = true\r\n";
        RequireWellBehaved(rule, damaged);
        const std::string out = rule.Transform(damaged);
        REQUIRE(out.find("S = a") == std::string::npos);
        REQUIRE(ValueOf(out, "Experimental", "SaveGameMaxSize") == "false");
    }
}

TEST_CASE("PathNamesDataFile: what the CreateFile redirect matches", "[rfab][compat][overlay]")
{
    constexpr std::string_view rel = "SKSE/Plugins/EngineFixes.toml";
    using WV = std::wstring_view;

    // As SKSE plugins open it: cwd-relative (the launcher sets cwd to the game folder) or absolute.
    REQUIRE(Compat::PathNamesDataFile(WV(L"Data\\SKSE\\Plugins\\EngineFixes.toml"), rel));
    REQUIRE(Compat::PathNamesDataFile(WV(L"Data/SKSE/Plugins/EngineFixes.toml"), rel));
    REQUIRE(Compat::PathNamesDataFile(WV(L".\\Data\\SKSE\\Plugins\\EngineFixes.toml"), rel));
    REQUIRE(Compat::PathNamesDataFile(WV(L"C:\\Games\\RFAB\\data\\skse\\plugins\\ENGINEFIXES.TOML"), rel));
    REQUIRE(Compat::PathNamesDataFile(WV(L"\\\\?\\C:\\Games\\RFAB\\Data\\SKSE\\Plugins\\EngineFixes.toml"), rel));
    REQUIRE(Compat::PathNamesDataFile(WV(L"C:\\Games\\RFAB\\Data\\\\SKSE//Plugins\\EngineFixes.toml"), rel));
    REQUIRE(Compat::PathNamesDataFile(WV(L"D:\\\u0418\u0433\u0440\u044b\\RFAB\\Data\\SKSE\\Plugins\\EngineFixes.toml"), rel));
    REQUIRE(Compat::PathNamesDataFile(std::string_view("Data\\SKSE\\Plugins\\EngineFixes.toml"), rel));

    // Never: the real MO2 mod folder, our own overlay, look-alike names, other folders called *data.
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L"C:\\RFAB\\MO2\\mods\\SSE Engine Fixes\\skse\\plugins\\EngineFixes.toml"), rel));
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L"C:\\RFAB\\MO2\\mods\\RFAB_Online\\SkyrimTogetherReborn\\rfab_coop_config\\EngineFixes.toml"), rel));
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L"Data\\SKSE\\Plugins\\xEngineFixes.toml"), rel));
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L"Data\\SKSE\\Plugins\\EngineFixes.toml.bak"), rel));
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L"C:\\MyData\\SKSE\\Plugins\\EngineFixes.toml"), rel));
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L"C:\\Data\\X\\SKSE\\Plugins\\EngineFixes.toml"), rel));
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L"SKSE\\Plugins\\EngineFixes.toml"), rel));
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L"EngineFixes.toml"), rel));
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L""), rel));
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L"Data\\SKSE\\Plugins\\EngineFixes.toml"), std::string_view{}));

    // Bracketed names and spaces (the RFAB autosave file) and the NSF plugin folder.
    REQUIRE(Compat::PathNamesDataFile(WV(L"C:\\RFAB\\Data\\SKSE\\Plugins\\[RFAB] Autosave.ini"), Compat::FindRule("rfab-autosave")->DataRelativePath));
    REQUIRE(Compat::PathNamesDataFile(WV(L"C:\\RFAB\\Data\\NetScriptFramework\\Plugins\\BetterStealing.dll"),
                                      Compat::FindRule("betterstealing")->DataRelativePath));
    REQUIRE_FALSE(Compat::PathNamesDataFile(WV(L"C:\\RFAB\\Data\\NetScriptFramework\\Plugins\\BetterStealing.config.txt"),
                                            Compat::FindRule("betterstealing")->DataRelativePath));
}

TEST_CASE("PrepareOverlays writes only into its own folder", "[rfab][compat][overlay]")
{
    using namespace RfabStockConfigs;
    TempTree tree("rfab_compat_overlay_test");
    const fs::path data = tree.Root / "Data";
    const fs::path overlay = tree.Root / "overlay";

    SECTION("stock RFAB: Souls, EngineFixes and Autosave overlaid, po3 left alone, BetterStealing hidden")
    {
        WriteFile(data / "SKSE/Plugins/SkyrimSoulsRE.ini", kStockSkyrimSoulsIni);
        WriteFile(data / "SKSE/Plugins/EngineFixes.toml", kStockDisableQuickSavesToml);
        WriteFile(data / "SKSE/Plugins/[RFAB] Autosave.ini", kStockRfabAutosaveIni);
        WriteFile(data / "SKSE/Plugins/po3_Tweaks.ini", kStockPo3TweaksIni);
        WriteFile(data / "NetScriptFramework/Plugins/BetterStealing.dll", "MZ");

        const auto outcomes = Compat::PrepareOverlays(data, overlay);
        REQUIRE(outcomes.size() == Compat::kRuleCount);
        REQUIRE(Compat::FindOutcome(outcomes, "skyrimsouls")->Status == Compat::OverlayStatus::kOverlaid);
        REQUIRE(Compat::FindOutcome(outcomes, "enginefixes")->Status == Compat::OverlayStatus::kOverlaid);
        REQUIRE(Compat::FindOutcome(outcomes, "rfab-autosave")->Status == Compat::OverlayStatus::kOverlaid);
        REQUIRE(Compat::FindOutcome(outcomes, "po3-tweaks")->Status == Compat::OverlayStatus::kUnchanged);
        REQUIRE(Compat::FindOutcome(outcomes, "betterstealing")->Status == Compat::OverlayStatus::kHidden);

        // The mods' files are byte-identical; nothing was renamed.
        REQUIRE(ReadFile(data / "SKSE/Plugins/SkyrimSoulsRE.ini") == std::string(kStockSkyrimSoulsIni));
        REQUIRE(ReadFile(data / "SKSE/Plugins/EngineFixes.toml") == std::string(kStockDisableQuickSavesToml));
        REQUIRE(ReadFile(data / "SKSE/Plugins/[RFAB] Autosave.ini") == std::string(kStockRfabAutosaveIni));
        REQUIRE(ReadFile(data / "SKSE/Plugins/po3_Tweaks.ini") == std::string(kStockPo3TweaksIni));
        REQUIRE(fs::exists(data / "NetScriptFramework/Plugins/BetterStealing.dll"));

        // The overlay holds exactly the transformed text; nothing for unchanged rules.
        REQUIRE(ReadFile(overlay / "SkyrimSoulsRE.ini") == Compat::FindRule("skyrimsouls")->Transform(kStockSkyrimSoulsIni));
        REQUIRE_FALSE(fs::exists(overlay / "po3_Tweaks.ini"));

        REQUIRE(Compat::IsEngineFixesSafe(outcomes, true));
        REQUIRE_FALSE(Compat::IsEngineFixesSafe(outcomes, false));

        const std::string report = Compat::FormatReport(outcomes, true);
        REQUIRE(report.find("[skyrimsouls] Data/SKSE/Plugins/SkyrimSoulsRE.ini: overlaid") != std::string::npos);
        REQUIRE(report.find("[SLOWMOTION] fSlowMotionMultiplier: 0.500000 -> 1.000000") != std::string::npos);
        REQUIRE(report.find("[po3-tweaks] Data/SKSE/Plugins/po3_Tweaks.ini: unchanged") != std::string::npos);
    }

    SECTION("a stale overlay from an earlier launch cannot outlive its need")
    {
        WriteFile(overlay / "SkyrimSoulsRE.ini", "stale");
        const std::string compliant = Compat::FindRule("skyrimsouls")->Transform(kStockSkyrimSoulsIni);
        WriteFile(data / "SKSE/Plugins/SkyrimSoulsRE.ini", compliant);
        const auto outcomes = Compat::PrepareOverlays(data, overlay);
        REQUIRE(Compat::FindOutcome(outcomes, "skyrimsouls")->Status == Compat::OverlayStatus::kUnchanged);
        REQUIRE_FALSE(fs::exists(overlay / "SkyrimSoulsRE.ini"));
    }

    SECTION("nothing installed: everything absent, EngineFixes defaults allowed, no redirect needed")
    {
        const auto outcomes = Compat::PrepareOverlays(data, overlay);
        for (const auto& outcome : outcomes)
        {
            REQUIRE(outcome.Status == Compat::OverlayStatus::kAbsent);
            REQUIRE_FALSE(outcome.Redirects());
        }
        REQUIRE(Compat::IsEngineFixesSafe(outcomes, false));
    }

    SECTION("co-op copy cannot be written: EngineFixes kept out")
    {
        WriteFile(data / "SKSE/Plugins/EngineFixes.toml", kStockEngineFixesToml);
        WriteFile(tree.Root / "blocker", "a file where the overlay folder should be");
        const auto outcomes = Compat::PrepareOverlays(data, tree.Root / "blocker");
        REQUIRE(Compat::FindOutcome(outcomes, "enginefixes")->Status == Compat::OverlayStatus::kFailed);
        REQUIRE_FALSE(Compat::IsEngineFixesSafe(outcomes, true));
        REQUIRE(ReadFile(data / "SKSE/Plugins/EngineFixes.toml") == std::string(kStockEngineFixesToml));
    }
}

TEST_CASE("Menu pause policy: one decision for Souls, perk trees and vanilla", "[rfab][compat][menus]")
{
    SECTION("Skyrim Souls loaded (RFAB): Souls owns pausing, Together patches nothing")
    {
        for (const bool perkTrees : {false, true})
        {
            const auto policy = Compat::DecideMenuPausePolicy({true, perkTrees});
            REQUIRE(policy.Owner == Compat::MenuPauseOwner::kSkyrimSouls);
            REQUIRE_FALSE(policy.PatchStatsMenuInput);
            REQUIRE_FALSE(policy.PatchFavoritesInput);
            for (const char* menu : {"StatsMenu", "TweenMenu", "InventoryMenu", "ContainerMenu"})
                REQUIRE_FALSE(policy.TogetherUnpauses(menu));
        }
    }
    SECTION("perk-tree mods without Souls: Together unpauses all but StatsMenu, no StatsMenu patch")
    {
        const auto policy = Compat::DecideMenuPausePolicy({false, true});
        REQUIRE(policy.Owner == Compat::MenuPauseOwner::kTogether);
        REQUIRE(policy.StatsMenuStaysPaused);
        REQUIRE_FALSE(policy.PatchStatsMenuInput);
        REQUIRE(policy.PatchFavoritesInput);
        REQUIRE_FALSE(policy.TogetherUnpauses("StatsMenu"));
        REQUIRE(policy.TogetherUnpauses("TweenMenu"));
    }
    SECTION("vanilla: StatsMenu unpaused with its input patch; late RFAB evidence only tightens")
    {
        auto policy = Compat::DecideMenuPausePolicy({false, false});
        REQUIRE(policy.TogetherUnpauses("StatsMenu"));
        REQUIRE(policy.PatchStatsMenuInput);
        REQUIRE(policy.NoteLatePerkTreeEvidence());
        REQUIRE_FALSE(policy.TogetherUnpauses("StatsMenu"));
        REQUIRE(policy.TogetherUnpauses("MagicMenu"));
        REQUIRE_FALSE(policy.NoteLatePerkTreeEvidence());
        REQUIRE(policy.Describe().find("StatsMenu stays paused") != std::string::npos);
    }
}

namespace
{
// The launcher and client are Windows-only, so Linux CI cannot compile them; their wiring is
// pinned by source checks instead.
fs::path FindCompatRepoRoot()
{
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "Compat" / "ConfigOverlay.h"))
            return cur;
        if (!cur.has_parent_path() || cur.parent_path() == cur)
            break;
        cur = cur.parent_path();
    }
    return {};
}

std::string ReadRepoSource(const char* acpRelative)
{
    const auto root = FindCompatRepoRoot();
    REQUIRE_FALSE(root.empty());
    std::string text;
    REQUIRE(RfabCompat::Detail::ReadTextFile(root / acpRelative, text));
    return text;
}

bool Contains(const std::string& acText, std::string_view acNeedle)
{
    return acText.find(acNeedle) != std::string::npos;
}
} // namespace

TEST_CASE("Co-op config overlay wiring: launcher, client and installers", "[rfab][compat][overlay]")
{
    SECTION("launcher: redirect is armed after path routing and before the game is loaded")
    {
        const std::string launcher = ReadRepoSource("Code/immersive_launcher/Launcher.cpp");
        const size_t routing = launcher.find("loader::InstallPathRouting(LC->gamePath);");
        const size_t overlay = launcher.find("stubs::InitCoopConfigOverlay(LC->gamePath);");
        const size_t load = launcher.find("if (!LoadProgram(*LC))");
        REQUIRE(routing != std::string::npos);
        REQUIRE(overlay != std::string::npos);
        REQUIRE(load != std::string::npos);
        REQUIRE(routing < overlay);
        REQUIRE(overlay < load);
        REQUIRE_FALSE(Contains(launcher, "RfabCompat::"));
    }
    SECTION("launcher: no code path writes another mod's file")
    {
        // ConfigOverlayRedirect.cpp writes only into its own overlay folder (PrepareOverlays above
        // proves the sources stay byte-identical).
        for (const char* file : {"Code/immersive_launcher/Launcher.cpp", "Code/immersive_launcher/stubs/DllBlocklist.cpp",
                                 "Code/client/Games/Skyrim/Interface/Menus/SkillsMenu.cpp",
                                 "Code/client/Services/Rfab/RfabXpAndBookSync.cpp", "Code/client/Services/RfabAdapterService.cpp"})
        {
            INFO(file);
            const std::string text = ReadRepoSource(file);
            REQUIRE_FALSE(Contains(text, "WriteTextFile("));
            REQUIRE_FALSE(Contains(text, "EnsureEngineFixesPatchedUnderRoot"));
            REQUIRE_FALSE(Contains(text, "SuppressRfabAutosave"));
        }
        const std::string compat = ReadRepoSource("Code/common/RfabModpackCompat.h");
        REQUIRE_FALSE(Contains(compat, "EnsureEngineFixesPatchedUnderRoot"));
    }
    SECTION("launcher: EngineFixes loads only when its co-op copy is in place")
    {
        const std::string blocklist = ReadRepoSource("Code/immersive_launcher/stubs/DllBlocklist.cpp");
        REQUIRE(Contains(blocklist, "IsEngineFixesAllowed()"));
        const std::string redirect = ReadRepoSource("Code/immersive_launcher/stubs/ConfigOverlayRedirect.cpp");
        REQUIRE(Contains(redirect, "Compat::PrepareOverlays("));
        REQUIRE(Contains(redirect, "Compat::IsEngineFixesSafe("));
        REQUIRE(Contains(redirect, "\"CreateFileW\""));
        REQUIRE(Contains(redirect, "\"CreateFileA\""));
    }
    SECTION("client: one menu pause decision, made after SKSE loaded its plugins")
    {
        const std::string ui = ReadRepoSource("Code/client/Games/Skyrim/Interface/UI.cpp");
        REQUIRE(Contains(ui, "Compat::DecideMenuPausePolicy(environment)"));
        REQUIRE(Contains(ui, "if (s_menuPolicy.PatchStatsMenuInput)"));
        REQUIRE(Contains(ui, "if (s_menuPolicy.PatchFavoritesInput)"));
        REQUIRE(Contains(ui, "if (s_menuPolicy.TogetherUnpauses(menuName))"));
        REQUIRE(Contains(ui, "stubs::GetCoopConfigReport()"));
        const size_t register_ = ui.find("static void Hook_UI_Register(");
        REQUIRE(register_ != std::string::npos);
        REQUIRE(ui.find("DecideMenuPolicyPostSkse();", register_) != std::string::npos);
        // Nothing else may patch StatsMenu input behind the policy's back.
        const std::string skills = ReadRepoSource("Code/client/Games/Skyrim/Interface/Menus/SkillsMenu.cpp");
        REQUIRE_FALSE(Contains(skills, "ShouldKeepStatsMenuPausedRuntime"));
        REQUIRE_FALSE(Contains(skills, "ApplyFallbackStatsMenuPatchesPostSkse"));
    }
    SECTION("installers only undo earlier releases' edits")
    {
        const std::string setup = ReadRepoSource("Tools/rfab/Setup-RfabOnline.ps1");
        REQUIRE(Contains(setup, "Invoke-RfabOnlineConfigRestore"));
        REQUIRE_FALSE(Contains(setup, "Ensure-SectionKeys"));
        REQUIRE_FALSE(Contains(setup, "-NewName \"BetterStealing.dll.mohidden\""));
        const std::string package = ReadRepoSource("Tools/rfab/Package-Release.ps1");
        REQUIRE(Contains(package, "Restore-ModConfigs.ps1"));
        REQUIRE_FALSE(Contains(package, "Filter \"EngineFixes.toml\""));
        REQUIRE_FALSE(Contains(package, "Filter \"SkyrimSoulsRE.ini\""));
        const std::string restore = ReadRepoSource("Tools/rfab/Restore-ModConfigs.ps1");
        REQUIRE(restore.rfind("\xEF\xBB\xBF", 0) == 0);
        // Every rule's file is known to the undo script (key lists: check_release_invariants.py).
        for (const auto& rule : Compat::GetRules())
        {
            if (rule.Kind != Compat::RuleKind::kOverlay)
                continue;
            const std::string_view path = rule.DataRelativePath;
            const std::string name(path.substr(path.find_last_of('/') + 1));
            INFO(name);
            REQUIRE(Contains(restore, "\"" + name + "\""));
        }
    }
}
