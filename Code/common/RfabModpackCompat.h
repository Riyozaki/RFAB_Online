#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace RfabCompat
{
// Skyrim Creation Engine spatial unit ratio: 1 meter = 70.0 game units (64 units = 3 feet = 0.9144m; 70 units = 1.0m).
inline constexpr float kSkyrimUnitsPerMeter = 70.0f;

[[nodiscard]] constexpr float MetersToSkyrimUnits(float aMeters) noexcept
{
    return aMeters * kSkyrimUnitsPerMeter;
}

[[nodiscard]] constexpr float SkyrimUnitsToMeters(float aUnits) noexcept
{
    return aUnits / kSkyrimUnitsPerMeter;
}

struct RfabBindingsConfig
{
    uint32_t SchemaVersion{1};
    std::string PluginName{"RFAB.esp"};

    std::string XpAmountGlobalEditorId{"RFAB_XP_Amount"};
    std::string XpBonusGlobalEditorId{"RFAB_XP_Multiplier"};
    std::string XpBooksFormListEditorId{"XPBooksReaded"};
    std::string NoDeathHandlingGlobalEditorId{"REQ_Combat_NoDeathHandling"};
    std::string KillCounterGlobalEditorId{"RFAB_TFOTG_Kill_Counter"};
    std::string XpHandlerQuestEditorId{"RFAB_XP_Handler"};

    // Optional last-resort base FormID only used if PO3 EditorID tables are completely absent.
    uint32_t FallbackXpAmountBaseFormId{0x00AD45BB};

    std::vector<std::string> SyncedGlobalPrefixes{
        "RFAB_Counter_",
        "RFAB_Global_",
        "RFAB_Boss_",
        "RFAB_Quest_",
        "RFAB_TFOTG_",
        "TFOTG_",
    };

    std::vector<std::string> ExcludedGlobalPrefixes{
        "RFAB_MCM_",
        "RFAB_UI_",
        "RFAB_Option_",
        "RFAB_Key_",
        "RFAB_HUD_",
        "RFAB_Sound_",
        "RFAB_Camera_",
        "RFAB_XP_Amount",
        "RFAB_XP_Bonus",
        "RFAB_XP_Multiplier",
    };

    std::string ExperienceIniRelPath{"Data/SKSE/Plugins/StorageUtilData/Experience.ini"};
    std::string QuestsIniRelPath{"Data/SKSE/Plugins/StorageUtilData/Quests.ini"};
    std::string XpSettingsJsonRelPath{"Data/SKSE/Plugins/StorageUtilData/RFAB_XP_Settings.json"};
    std::string BossListJsonRelPath{"Data/SKSE/Plugins/StorageUtilData/BossList.json"};
    std::string RaceListJsonRelPath{"Data/SKSE/Plugins/StorageUtilData/RaceList.json"};
    std::string RfabAutosaveIniRelPath{"Data/SKSE/Plugins/[RFAB] Autosave.ini"};

    bool AutoScaleEnemyHealth{true};
    float HealthExtraBase{1.0f};
    float DownedReviveDurationSec{4.0f};
    float DownedReviveHpFraction{0.30f};
    float PostReviveGraceSec{3.0f};
    float CombatLeashRadiusMeters{120.0f};
    float CombatClearHysteresisSec{4.5f};
    uint32_t PostCombatAutosaveIntervalSec{180};
    uint32_t SaveRingBufferSize{3};
    float AmbushDedupRadiusMeters{12.0f};
    uint32_t AmbushDedupWindowMs{3000};
};

struct RfabLiveXpTable
{
    std::unordered_map<uint32_t, int32_t> LevelThresholds{};
    std::unordered_map<std::string, int32_t> QuestRewards{};
    std::unordered_map<std::string, float> XpMultipliers{};

    [[nodiscard]] int32_t GetThresholdForLevel(uint32_t aLevel) const noexcept
    {
        if (const auto it = LevelThresholds.find(aLevel); it != LevelThresholds.end())
            return it->second;
        return 0;
    }
};

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

// Strips directory components, quotes, and `.ess.tmp` / `.tmp` / `.skse` / `.ess` suffixes while preserving original stem casing.
inline std::string StripSaveSlotExtensionPreserveCase(std::string_view aRawPathOrName)
{
    std::string s = Trim(aRawPathOrName);
    while (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        s = Trim(std::string_view(s).substr(1, s.size() - 2));

    if (const size_t sep = s.find_last_of("\\/"); sep != std::string::npos)
        s = s.substr(sep + 1);

    for (std::string_view ext : {
             std::string_view(".ess.tmp"),
             std::string_view(".ess.bak"),
             std::string_view(".tmp"),
             std::string_view(".bak"),
             std::string_view(".skse"),
             std::string_view(".ess")})
    {
        if (EndsWithIgnoreCase(s, ext))
        {
            s.resize(s.size() - ext.size());
            break;
        }
    }

    return Trim(s);
}

// Canonical lowercase save slot stem so `"Saves\\RFAB_MP_S1234_Auto_0.ess"` == `"RFAB_MP_S1234_Auto_0"`.
inline std::string NormalizeSaveSlotStem(std::string_view aRawPathOrName)
{
    return ToLowerAscii(StripSaveSlotExtensionPreserveCase(aRawPathOrName));
}

// Verifies that an `.ess` file on disk has a genuine Skyrim `TESV_SAVEGAME` header and non-truncated payload.
inline bool IsValidEssFileOnDisk(const std::filesystem::path& acEssPath) noexcept
{
    constexpr std::string_view kMagic = "TESV_SAVEGAME";
    std::error_code ec;
    if (!std::filesystem::exists(acEssPath, ec) || !std::filesystem::is_regular_file(acEssPath, ec))
        return false;

    const auto fileSize = std::filesystem::file_size(acEssPath, ec);
    if (ec || fileSize < 256)
        return false;

    std::ifstream ifs(acEssPath, std::ios::in | std::ios::binary);
    if (!ifs.is_open())
        return false;

    char magicBuf[13]{};
    ifs.read(magicBuf, static_cast<std::streamsize>(sizeof(magicBuf)));
    if (ifs.gcount() != static_cast<std::streamsize>(sizeof(magicBuf)))
        return false;

    return std::string_view(magicBuf, sizeof(magicBuf)) == kMagic;
}

// Safely moves truncated / headerless `.ess` files left behind by mid-save aborts into `Quarantined_Corrupt_v100/`
// alongside any matching `.skse` or `.ess.tmp` companion files (never deleting user files without backup).
inline size_t QuarantineCorruptEssFilesInDirectory(const std::filesystem::path& acSavesDir) noexcept
{
    std::error_code ec;
    if (acSavesDir.empty() || !std::filesystem::exists(acSavesDir, ec) || !std::filesystem::is_directory(acSavesDir, ec))
        return 0;

    std::vector<std::filesystem::path> corruptEssFiles;
    for (const auto& entry : std::filesystem::directory_iterator(acSavesDir, ec))
    {
        if (ec || !entry.is_regular_file(ec))
            continue;

        const std::string fname = entry.path().filename().string();
        if (EndsWithIgnoreCase(fname, ".ess") && !IsValidEssFileOnDisk(entry.path()))
        {
            corruptEssFiles.push_back(entry.path());
        }
        else if (EndsWithIgnoreCase(fname, ".ess.tmp"))
        {
            const auto sz = std::filesystem::file_size(entry.path(), ec);
            if (!ec && sz < 256)
                corruptEssFiles.push_back(entry.path());
        }
    }

    if (corruptEssFiles.empty())
        return 0;

    const std::filesystem::path quarantineDir = acSavesDir / "Quarantined_Corrupt_v100";
    std::filesystem::create_directories(quarantineDir, ec);
    if (ec)
        return 0;

    size_t movedCount = 0;
    for (const auto& badPath : corruptEssFiles)
    {
        const auto destPath = quarantineDir / badPath.filename();
        std::filesystem::rename(badPath, destPath, ec);
        if (!ec)
        {
            ++movedCount;
            if (EndsWithIgnoreCase(badPath.filename().string(), ".ess"))
            {
                auto skseCompanion = badPath;
                skseCompanion.replace_extension(".skse");
                if (std::filesystem::exists(skseCompanion, ec))
                    std::filesystem::rename(skseCompanion, quarantineDir / skseCompanion.filename(), ec);
            }
        }
    }

    return movedCount;
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

// Lightweight zero-dependency extractors for flat/nested config keys in JSON manifests.
inline bool ExtractJsonString(std::string_view aJson, std::string_view aKey, std::string& aOut)
{
    const std::string needle = "\"" + std::string(aKey) + "\"";
    const size_t keyPos = aJson.find(needle);
    if (keyPos == std::string_view::npos)
        return false;

    const size_t colonPos = aJson.find(':', keyPos + needle.size());
    if (colonPos == std::string_view::npos)
        return false;

    const size_t openQuote = aJson.find('"', colonPos + 1);
    if (openQuote == std::string_view::npos)
        return false;

    std::string result;
    for (size_t i = openQuote + 1; i < aJson.size(); ++i)
    {
        if (aJson[i] == '\\' && i + 1 < aJson.size())
        {
            result.push_back(aJson[++i]);
            continue;
        }
        if (aJson[i] == '"')
        {
            aOut = std::move(result);
            return true;
        }
        result.push_back(aJson[i]);
    }
    return false;
}

inline bool ExtractJsonFloat(std::string_view aJson, std::string_view aKey, float& aOut)
{
    const std::string needle = "\"" + std::string(aKey) + "\"";
    const size_t keyPos = aJson.find(needle);
    if (keyPos == std::string_view::npos)
        return false;

    const size_t colonPos = aJson.find(':', keyPos + needle.size());
    if (colonPos == std::string_view::npos)
        return false;

    size_t pos = colonPos + 1;
    while (pos < aJson.size() && std::isspace(static_cast<unsigned char>(aJson[pos])))
        ++pos;

    if (pos >= aJson.size())
        return false;

    char* endPtr = nullptr;
    const std::string tail(aJson.substr(pos, 64));
    const float parsed = std::strtof(tail.c_str(), &endPtr);
    if (endPtr == tail.c_str())
        return false;

    aOut = parsed;
    return true;
}

inline bool ExtractJsonBool(std::string_view aJson, std::string_view aKey, bool& aOut)
{
    const std::string needle = "\"" + std::string(aKey) + "\"";
    const size_t keyPos = aJson.find(needle);
    if (keyPos == std::string_view::npos)
        return false;

    const size_t colonPos = aJson.find(':', keyPos + needle.size());
    if (colonPos == std::string_view::npos)
        return false;

    const std::string token = ToLowerAscii(Trim(aJson.substr(colonPos + 1, 16)));
    if (StartsWithIgnoreCase(token, "true"))
    {
        aOut = true;
        return true;
    }
    if (StartsWithIgnoreCase(token, "false"))
    {
        aOut = false;
        return true;
    }
    return false;
}

inline bool ExtractJsonStringArray(std::string_view aJson, std::string_view aKey, std::vector<std::string>& aOut)
{
    const std::string needle = "\"" + std::string(aKey) + "\"";
    const size_t keyPos = aJson.find(needle);
    if (keyPos == std::string_view::npos)
        return false;

    const size_t openBracket = aJson.find('[', keyPos + needle.size());
    if (openBracket == std::string_view::npos)
        return false;

    const size_t closeBracket = aJson.find(']', openBracket + 1);
    if (closeBracket == std::string_view::npos)
        return false;

    std::vector<std::string> items;
    const std::string_view slice = aJson.substr(openBracket + 1, closeBracket - openBracket - 1);
    size_t pos = 0;
    while (pos < slice.size())
    {
        const size_t q1 = slice.find('"', pos);
        if (q1 == std::string_view::npos)
            break;
        const size_t q2 = slice.find('"', q1 + 1);
        if (q2 == std::string_view::npos)
            break;
        items.emplace_back(slice.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;
    }

    aOut = std::move(items);
    return true;
}
} // namespace Detail

inline RfabBindingsConfig ParseBindingsJson(std::string_view aJson)
{
    RfabBindingsConfig cfg{};
    float numVal = 0.0f;
    if (Detail::ExtractJsonFloat(aJson, "schema_version", numVal) && numVal >= 1.0f)
        cfg.SchemaVersion = static_cast<uint32_t>(numVal);

    Detail::ExtractJsonString(aJson, "plugin_name", cfg.PluginName);
    Detail::ExtractJsonString(aJson, "xp_amount_global", cfg.XpAmountGlobalEditorId);
    Detail::ExtractJsonString(aJson, "xp_bonus_global", cfg.XpBonusGlobalEditorId);
    Detail::ExtractJsonString(aJson, "xp_books_formlist", cfg.XpBooksFormListEditorId);
    Detail::ExtractJsonString(aJson, "no_death_handling_global", cfg.NoDeathHandlingGlobalEditorId);
    Detail::ExtractJsonString(aJson, "kill_counter_global", cfg.KillCounterGlobalEditorId);
    Detail::ExtractJsonString(aJson, "xp_handler_quest", cfg.XpHandlerQuestEditorId);

    std::string hexFallback;
    if (Detail::ExtractJsonString(aJson, "xp_amount_global", hexFallback))
    {
        // Note: editor_ids appears before fallback_base_form_ids; search fallback block explicitly if present.
        if (const size_t fbBlock = aJson.find("\"fallback_base_form_ids\""); fbBlock != std::string_view::npos)
        {
            std::string fbHex;
            if (Detail::ExtractJsonString(aJson.substr(fbBlock), "xp_amount_global", fbHex))
                cfg.FallbackXpAmountBaseFormId = static_cast<uint32_t>(std::strtoul(fbHex.c_str(), nullptr, 0));
        }
    }

    Detail::ExtractJsonStringArray(aJson, "synced_global_prefixes", cfg.SyncedGlobalPrefixes);
    Detail::ExtractJsonStringArray(aJson, "excluded_global_prefixes", cfg.ExcludedGlobalPrefixes);

    Detail::ExtractJsonString(aJson, "experience_ini", cfg.ExperienceIniRelPath);
    Detail::ExtractJsonString(aJson, "quests_ini", cfg.QuestsIniRelPath);
    Detail::ExtractJsonString(aJson, "xp_settings_json", cfg.XpSettingsJsonRelPath);
    Detail::ExtractJsonString(aJson, "boss_list_json", cfg.BossListJsonRelPath);
    Detail::ExtractJsonString(aJson, "race_list_json", cfg.RaceListJsonRelPath);
    Detail::ExtractJsonString(aJson, "rfab_autosave_ini", cfg.RfabAutosaveIniRelPath);

    Detail::ExtractJsonBool(aJson, "auto_scale_enemy_health", cfg.AutoScaleEnemyHealth);
    Detail::ExtractJsonFloat(aJson, "health_extra_base", cfg.HealthExtraBase);
    Detail::ExtractJsonFloat(aJson, "downed_revive_duration_sec", cfg.DownedReviveDurationSec);
    Detail::ExtractJsonFloat(aJson, "downed_revive_hp_fraction", cfg.DownedReviveHpFraction);
    Detail::ExtractJsonFloat(aJson, "post_revive_grace_sec", cfg.PostReviveGraceSec);
    Detail::ExtractJsonFloat(aJson, "combat_leash_radius_meters", cfg.CombatLeashRadiusMeters);
    Detail::ExtractJsonFloat(aJson, "combat_clear_hysteresis_sec", cfg.CombatClearHysteresisSec);

    if (Detail::ExtractJsonFloat(aJson, "post_combat_autosave_interval_sec", numVal) && numVal >= 0.0f)
        cfg.PostCombatAutosaveIntervalSec = static_cast<uint32_t>(numVal);
    if (Detail::ExtractJsonFloat(aJson, "save_ring_buffer_size", numVal) && numVal >= 1.0f)
        cfg.SaveRingBufferSize = std::clamp(static_cast<uint32_t>(numVal), 1U, 10U);

    Detail::ExtractJsonFloat(aJson, "ambush_dedup_radius_meters", cfg.AmbushDedupRadiusMeters);
    if (Detail::ExtractJsonFloat(aJson, "ambush_dedup_window_ms", numVal) && numVal >= 0.0f)
        cfg.AmbushDedupWindowMs = static_cast<uint32_t>(numVal);

    return cfg;
}

inline RfabBindingsConfig LoadBindingsFromDisk(const std::filesystem::path& acGameRoot)
{
    std::string raw;
    const auto candidate = acGameRoot / "Data" / "SKSE" / "Plugins" / "RFAB_Online_Bindings.json";
    if (Detail::ReadTextFile(candidate, raw))
        return ParseBindingsJson(raw);

    return RfabBindingsConfig{};
}

[[nodiscard]] inline bool ShouldAutoSyncGlobalEditorId(std::string_view aEditorId, const RfabBindingsConfig& acConfig) noexcept
{
    if (aEditorId.empty())
        return false;

    const std::string lowerEdid = Detail::ToLowerAscii(aEditorId);
    // RFAB_XP_Amount is each player's personal unspent RFAB_Menu skill-point wallet (earned deltas sync via
    // SyncRfabExperienceRequest; spending points in RFAB_Menu is per-character and must NEVER overwrite peers!).
    if (lowerEdid == "rfab_xp_amount" || lowerEdid == Detail::ToLowerAscii(acConfig.XpAmountGlobalEditorId) ||
        lowerEdid == "rfab_xp_multiplier" || lowerEdid == "rfab_xp_bonus" ||
        lowerEdid == Detail::ToLowerAscii(acConfig.XpBonusGlobalEditorId))
    {
        return false;
    }

    for (const auto& excluded : acConfig.ExcludedGlobalPrefixes)
    {
        if (Detail::StartsWithIgnoreCase(aEditorId, excluded))
            return false;
    }

    if (Detail::ToLowerAscii(aEditorId) == Detail::ToLowerAscii(acConfig.KillCounterGlobalEditorId))
        return true;

    for (const auto& prefix : acConfig.SyncedGlobalPrefixes)
    {
        if (Detail::StartsWithIgnoreCase(aEditorId, prefix))
            return true;
    }

    return false;
}

// Parses RFAB's Data/SKSE/Plugins/StorageUtilData/Experience.ini (e.g. "Level 2 = 2500" or "2 = 2500").
inline void ParseExperienceIni(std::string_view aContent, RfabLiveXpTable& aTable)
{
    std::istringstream iss{std::string(aContent)};
    std::string line;
    while (std::getline(iss, line))
    {
        const size_t commentPos = line.find_first_of(";#");
        if (commentPos != std::string::npos)
            line.erase(commentPos);

        const size_t eqPos = line.find('=');
        if (eqPos == std::string::npos)
            continue;

        std::string lhs = Detail::Trim(std::string_view(line).substr(0, eqPos));
        std::string rhs = Detail::Trim(std::string_view(line).substr(eqPos + 1));
        if (lhs.empty() || rhs.empty())
            continue;

        // Extract trailing digits from keys such as "2", "Level2", "Level 2", or "XP_2".
        size_t digitPos = lhs.size();
        while (digitPos > 0 && std::isdigit(static_cast<unsigned char>(lhs[digitPos - 1])))
            --digitPos;

        if (digitPos < lhs.size())
        {
            const uint32_t level = static_cast<uint32_t>(std::strtoul(lhs.c_str() + digitPos, nullptr, 10));
            const int32_t threshold = static_cast<int32_t>(std::strtol(rhs.c_str(), nullptr, 10));
            if (level > 0 && threshold >= 0)
                aTable.LevelThresholds[level] = threshold;
        }
    }
}

// Parses RFAB's Quests.ini lines (e.g. "MQ103 = 1500" or "[MainQuest]\nMQ103|100 = 1500").
inline void ParseQuestsIni(std::string_view aContent, RfabLiveXpTable& aTable)
{
    std::istringstream iss{std::string(aContent)};
    std::string line;
    while (std::getline(iss, line))
    {
        const size_t commentPos = line.find_first_of(";#");
        if (commentPos != std::string::npos)
            line.erase(commentPos);

        const size_t eqPos = line.find('=');
        if (eqPos == std::string::npos)
            continue;

        const std::string lhs = Detail::Trim(std::string_view(line).substr(0, eqPos));
        const std::string rhs = Detail::Trim(std::string_view(line).substr(eqPos + 1));
        if (lhs.empty() || rhs.empty() || lhs.front() == '[')
            continue;

        char* endPtr = nullptr;
        const long xp = std::strtol(rhs.c_str(), &endPtr, 10);
        if (endPtr != rhs.c_str() && xp >= 0)
            aTable.QuestRewards[lhs] = static_cast<int32_t>(xp);
    }
}

// Parses flat/nested float multipliers from RFAB_XP_Settings.json ("float" section in StorageUtil JSON).
inline void ParseXpSettingsJson(std::string_view aJson, RfabLiveXpTable& aTable)
{
    size_t pos = 0;
    while (pos < aJson.size())
    {
        const size_t q1 = aJson.find('"', pos);
        if (q1 == std::string_view::npos)
            break;
        const size_t q2 = aJson.find('"', q1 + 1);
        if (q2 == std::string_view::npos)
            break;

        const std::string key(aJson.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;

        const size_t colon = aJson.find(':', pos);
        if (colon == std::string_view::npos)
            break;

        size_t valStart = colon + 1;
        while (valStart < aJson.size() && std::isspace(static_cast<unsigned char>(aJson[valStart])))
            ++valStart;

        if (valStart < aJson.size() && (std::isdigit(static_cast<unsigned char>(aJson[valStart])) || aJson[valStart] == '-'))
        {
            const std::string slice(aJson.substr(valStart, 48));
            char* endPtr = nullptr;
            const float val = std::strtof(slice.c_str(), &endPtr);
            if (endPtr != slice.c_str())
                aTable.XpMultipliers[key] = val;
        }
    }
}

// Normalizes user-entered server addresses from CEF / console / config so all common forms work cleanly:
//   ""                        -> "127.0.0.1:10578"
//   "localhost"               -> "127.0.0.1:10578"
//   "localhost:10578"         -> "127.0.0.1:10578"
//   "26.160.34.131"           -> "26.160.34.131:10578"
//   "26.160.34.131:10578"     -> "26.160.34.131:10578"
//   "26.160.34.131:10578:10578" -> "26.160.34.131:10578"
//   "26.160.34.131:27015"     -> "26.160.34.131:27015"
//   "udp://26.160.34.131:10578/" -> "26.160.34.131:10578"
inline std::string NormalizeServerEndpoint(std::string_view aRawInput, uint16_t aFallbackPort = 10578)
{
    const uint16_t safeDefaultPort = aFallbackPort != 0 ? aFallbackPort : static_cast<uint16_t>(10578);
    std::string s = Detail::Trim(aRawInput);

    if (const size_t schemePos = s.find("://"); schemePos != std::string::npos)
        s.erase(0, schemePos + 3);

    while (!s.empty() && (s.back() == '/' || std::isspace(static_cast<unsigned char>(s.back()))))
        s.pop_back();

    if (s.empty())
        return "127.0.0.1:" + std::to_string(safeDefaultPort);

    uint16_t chosenPort = safeDefaultPort;

    // Bracketed IPv6: "[::1]:10578" or "[::1]"
    if (s.front() == '[')
    {
        const size_t closeBracket = s.find(']');
        if (closeBracket != std::string::npos)
        {
            const std::string hostPart = s.substr(0, closeBracket + 1);
            std::string tail = s.substr(closeBracket + 1);
            while (!tail.empty() && tail.front() == ':')
            {
                const size_t nextColon = tail.find(':', 1);
                const std::string portStr = tail.substr(1, nextColon == std::string::npos ? std::string::npos : nextColon - 1);
                char* endPtr = nullptr;
                const unsigned long parsed = std::strtoul(portStr.c_str(), &endPtr, 10);
                if (endPtr != portStr.c_str() && *endPtr == '\0' && parsed >= 1 && parsed <= 65535)
                    chosenPort = static_cast<uint16_t>(parsed);
                if (nextColon == std::string::npos)
                    break;
                tail.erase(0, nextColon);
            }
            return hostPart + ":" + std::to_string(chosenPort);
        }
    }

    // Strip one or more trailing ":<digits>" segments (handles "ip:port" AND accidental "ip:port:port" cleanly),
    // preserving the FIRST explicit port typed right after the host!
    std::vector<uint16_t> trailingPorts;
    while (true)
    {
        const size_t lastColon = s.rfind(':');
        if (lastColon == std::string::npos)
            break;

        const std::string portToken = Detail::Trim(std::string_view(s).substr(lastColon + 1));
        if (portToken.empty())
        {
            s.erase(lastColon);
            continue;
        }

        bool allDigits = true;
        for (char c : portToken)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
            {
                allDigits = false;
                break;
            }
        }
        if (!allDigits)
            break;

        const unsigned long parsed = std::strtoul(portToken.c_str(), nullptr, 10);
        if (parsed >= 1 && parsed <= 65535)
            trailingPorts.push_back(static_cast<uint16_t>(parsed));
        s.erase(lastColon);
    }

    // Trailing ports were popped right-to-left, so trailingPorts.back() is the port typed directly on the host!
    if (!trailingPorts.empty())
        chosenPort = trailingPorts.back();

    std::string host = Detail::Trim(s);
    if (host.empty() || Detail::ToLowerAscii(host) == "localhost")
        host = "127.0.0.1";

    return host + ":" + std::to_string(chosenPort);
}

// Idempotently patches EngineFixes.toml content (including overrides like RFAB's
// "Disable Quick Saves/skse/plugins/EngineFixes.toml") so Tilted's allocator & MO2 VFS work cleanly
// across both EngineFixes v5/v6 (Skyrim SE 1.5.97) and v7 keys while preserving user settings.
inline bool PatchEngineFixesTomlContent(std::string& aToml)
{
    bool changed = false;
    const bool hasCrLf = aToml.find("\r\n") != std::string::npos;
    const std::string newline = hasCrLf ? "\r\n" : "\n";
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
        patchKey("bMemoryManager", "false");
        patchKey("bScaleformAllocator", "false");
        patchKey("bRegularQuicksaves", "true");
        patchKey("iMaxStdio", "8192");
    }
    if (hasMemMgrSectionV7)
    {
        const bool hasOvVanilla = patchKey("OverrideVanilla", "false");
        const bool hasOvScaleform = patchKey("OverrideScaleform", "false");
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

        const bool hasMemMgr = patchKey("MemoryManager", "false");
        const bool hasScaleform = patchKey("ScaleformAllocator", "false");
        const bool hasQuicksaves = patchKey("RegularQuicksaves", "true");
        const bool hasMaxStdio = patchKey("MaxStdio", "8192");
        const bool hasSaveMax = patchKey("SaveGameMaxSize", "true");

        // Ensure partial MO2 overrides (e.g. "Disable Quick Saves/skse/plugins/EngineFixes.toml")
        // contain all required [Patches] keys inside [Patches] itself for SSE Engine Fixes v5/v6 (SE 1.5.97)!
        std::vector<std::pair<std::string_view, std::string_view>> missingPatches;
        if (!hasMemMgr && !hasMemMgrSectionV7)
            missingPatches.emplace_back("MemoryManager", "false");
        if (!hasScaleform && !hasMemMgrSectionV7)
            missingPatches.emplace_back("ScaleformAllocator", "false");
        if (!hasQuicksaves)
            missingPatches.emplace_back("RegularQuicksaves", "true");
        if (!hasMaxStdio)
            missingPatches.emplace_back("MaxStdio", "8192");
        insertUnderSection("Patches", missingPatches);

        if (!hasSaveMax)
            insertUnderSection("Experimental", {{"SaveGameMaxSize", "true"}});
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

inline size_t EnsureEngineFixesPatchedUnderRoot(const std::filesystem::path& acGameOrMo2Root) noexcept
{
    size_t patchedFiles = 0;
    std::error_code ec;

    auto patchToml = [&](const std::filesystem::path& p)
    {
        std::string content;
        if (!Detail::ReadTextFile(p, content))
            return;

        if (PatchEngineFixesTomlContent(content))
        {
            if (Detail::WriteTextFile(p, content))
                ++patchedFiles;
        }
    };

    auto patchSoulsIni = [&](const std::filesystem::path& p)
    {
        std::string content;
        if (!Detail::ReadTextFile(p, content))
            return;

        if (PatchSkyrimSoulsReIniContent(content))
        {
            if (Detail::WriteTextFile(p, content))
                ++patchedFiles;
        }
    };

    auto hideBetterStealing = [&](const std::filesystem::path& rootDir)
    {
        for (const auto& rel : {
                 std::filesystem::path("NetScriptFramework") / "Plugins" / "BetterStealing.dll",
                 std::filesystem::path("netscriptframework") / "plugins" / "BetterStealing.dll"})
        {
            const auto target = rootDir / rel;
            if (std::filesystem::exists(target, ec))
            {
                auto hiddenPath = target;
                hiddenPath += ".mohidden";
                std::filesystem::rename(target, hiddenPath, ec);
            }
        }
    };

    auto patchAutosaveIni = [&](const std::filesystem::path& p)
    {
        std::string content;
        if (!Detail::ReadTextFile(p, content))
            return;

        const std::string updated = SuppressRfabAutosaveIniContent(content);
        if (updated != content)
        {
            if (Detail::WriteTextFile(p, updated))
                ++patchedFiles;
        }
    };

    auto scanPluginTree = [&](const std::filesystem::path& baseDir)
    {
        patchToml(baseDir / "SKSE" / "Plugins" / "EngineFixes.toml");
        patchToml(baseDir / "skse" / "plugins" / "EngineFixes.toml");
        patchSoulsIni(baseDir / "SKSE" / "Plugins" / "SkyrimSoulsRE.ini");
        patchSoulsIni(baseDir / "skse" / "plugins" / "SkyrimSoulsRE.ini");
        patchAutosaveIni(baseDir / "SKSE" / "Plugins" / "[RFAB] Autosave.ini");
        patchAutosaveIni(baseDir / "skse" / "plugins" / "[RFAB] Autosave.ini");
        hideBetterStealing(baseDir);
    };

    // 1. Direct or MO2 USVFS-mapped Data/ directory (resolves straight to winning MO2 override at runtime!)
    scanPluginTree(acGameOrMo2Root / "Data");

    // 2. Scan MO2 mods/ + overwrite/ across all standard RFAB layouts:
    //    - <RFAB_Root>/MO2/{mods,overwrite}  (standard drake1ch/RFAB layout, e.g. G:\RFAB Online SkyrimSE\MO2)
    //    - <MO2_Root>/{mods,overwrite}
    //    - <Parent>/{mods,overwrite} and <Parent>/MO2/{mods,overwrite}
    const std::vector<std::filesystem::path> candidateMo2Roots = {
        acGameOrMo2Root,
        acGameOrMo2Root / "MO2",
        acGameOrMo2Root.parent_path(),
        acGameOrMo2Root.parent_path() / "MO2",
    };

    for (const auto& mo2Root : candidateMo2Roots)
    {
        if (mo2Root.empty())
            continue;

        const auto overwriteDir = mo2Root / "overwrite";
        if (std::filesystem::exists(overwriteDir, ec) && std::filesystem::is_directory(overwriteDir, ec))
            scanPluginTree(overwriteDir);

        const auto modsDir = mo2Root / "mods";
        if (!std::filesystem::exists(modsDir, ec) || !std::filesystem::is_directory(modsDir, ec))
            continue;

        for (const auto& modEntry : std::filesystem::directory_iterator(modsDir, ec))
        {
            if (!modEntry.is_directory(ec))
                continue;
            scanPluginTree(modEntry.path());
        }
    }

    return patchedFiles;
}

// Suppresses "[RFAB] Autosave.ini" combat/interval triggers while connected to multiplayer,
// preserving original file contents in memory so solo settings restore cleanly on disconnect.
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

// Extracts (major, minor) from semantic git tags such as "v1.0.0", "v1.0.0-7-gc0b262aa", "1.0.2".
// Returns false on untagged raw git hashes (e.g. "9f8e7d6c") so distinct protocol branches never collide.
[[nodiscard]] inline bool ExtractMajorMinorProtocolTag(std::string_view aVer, int& aOutMajor, int& aOutMinor) noexcept
{
    std::string s = Detail::Trim(aVer);
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V'))
        s.erase(s.begin());

    if (s.empty() || !std::isdigit(static_cast<unsigned char>(s[0])))
        return false;

    size_t pos = 0;
    int major = 0;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
    {
        major = major * 10 + (s[pos] - '0');
        ++pos;
    }
    if (pos >= s.size() || s[pos] != '.')
        return false;
    ++pos;

    if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos])))
        return false;

    int minor = 0;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
    {
        minor = minor * 10 + (s[pos] - '0');
        ++pos;
    }

    if (pos < s.size() && s[pos] != '.' && s[pos] != '-')
        return false;

    aOutMajor = major;
    aOutMinor = minor;
    return true;
}

// Allows any vMAJOR.MINOR.* client and server to connect without forcing whole-pack re-downloads on hotfixes,
// while still rejecting incompatible protocol families (e.g. v1.0.x vs v1.1.x or v2.0.x).
[[nodiscard]] inline bool AreBuildVersionsProtocolCompatible(std::string_view aServerVer, std::string_view aClientVer) noexcept
{
    if (aServerVer == aClientVer)
        return true;

    int sMaj = 0, sMin = 0;
    int cMaj = 0, cMin = 0;
    if (ExtractMajorMinorProtocolTag(aServerVer, sMaj, sMin) &&
        ExtractMajorMinorProtocolTag(aClientVer, cMaj, cMin))
    {
        return (sMaj == cMaj) && (sMin == cMin);
    }

    return false;
}

enum class RfabDifficultyMode : uint8_t
{
    Strannik = 0,   // Странник:  x2.0 dealt (x0.5 if Cursed), x0.5 taken
    Dragonborn = 1, // Довакин:   x1.0 dealt (x0.5 if Cursed), x1.0 taken
    Legend = 2      // Легенда:   x1.0 dealt (x0.5 if Cursed), x2.0 taken
};

struct RfabDifficultyMultipliers
{
    float OutgoingByPlayer = 1.0f; // fDiffMultHPByPCVH
    float IncomingToPlayer = 1.0f; // fDiffMultHPToPCVH
};

[[nodiscard]] inline float SanitizeDifficultyMultiplier(float aMult) noexcept
{
    if (!std::isfinite(aMult) || aMult <= 0.0f)
        return 1.0f;

    return std::clamp(aMult, 0.05f, 10.0f);
}

// Mirrors RFAB StartMenu.json + RFAB_StartMenu.UpdateDifficulty() + RFAB_MCM_Alias.OnPlayerLoadGame():
//   - Strannik (0):   ByPC = 2.0, ToPC = 0.5
//   - Dragonborn (1): ByPC = 1.0, ToPC = 1.0
//   - Legend (2):     ByPC = 1.0, ToPC = 2.0
//   - Cursed mark ("Проклятый"): overrides ByPC -> DamageWithCurse = 0.5 across all three modes.
[[nodiscard]] inline constexpr RfabDifficultyMultipliers ResolveRfabDifficultyMultipliers(
    RfabDifficultyMode aMode,
    bool aIsCursed) noexcept
{
    float byPlayer = 1.0f;
    float toPlayer = 1.0f;
    switch (aMode)
    {
    case RfabDifficultyMode::Strannik:
        byPlayer = 2.0f;
        toPlayer = 0.5f;
        break;
    case RfabDifficultyMode::Dragonborn:
        byPlayer = 1.0f;
        toPlayer = 1.0f;
        break;
    case RfabDifficultyMode::Legend:
        byPlayer = 1.0f;
        toPlayer = 2.0f;
        break;
    }

    if (aIsCursed)
        byPlayer = 0.5f;

    return {byPlayer, toPlayer};
}

// Skyrim SE has no fDiffMultHPByPCN / fDiffMultHPToPCN GMST for difficulty index 2 (Adept):
// GetDifficultyMultiplier(2) returns hardcoded 1.0f. RFAB's canonical SkyrimPrefs.ini uses 0 (VE)
// and RFAB.dll reads fDiffMultHP*PCVH (index 4). Preserve 0/1/3/4/5 and remap 2 -> 4.
[[nodiscard]] inline constexpr int32_t NormalizeRfabEngineDifficultyIndex(int32_t aCurrentDifficulty) noexcept
{
    if (aCurrentDifficulty < 0 || aCurrentDifficulty > 5 || aCurrentDifficulty == 2)
        return 4;

    return aCurrentDifficulty;
}

// Hostile/detrimental permanent kHealth (AV 24) effects cast directly by the local player (0x14)
// must carry the caster's RFAB fDiffMultHPByPCVH (2.0x Странник / 1.0x Довакин,Легенда / 0.5x Проклятый)
// across the wire, because on the target owner's client the caster is RemotePlayer (0xFFxxxxxx != 0x14)
// so RFAB.dll will NOT re-apply ByPCVH (zero double-multiplication). Summons (caster != 0x14) stay 1.0x.
[[nodiscard]] inline bool ShouldScalePlayerOutgoingHealthEffect(
    bool aIsRfabActive,
    uint32_t aCasterFormId,
    uint32_t aEffectFlags,
    int32_t aPrimaryAv,
    float aMagnitude) noexcept
{
    constexpr uint32_t kHostileOrDetrimental = (1u << 0) | (1u << 2);
    constexpr uint32_t kRecoverFlag = (1u << 1);
    constexpr int32_t kHealthAv = 24;

    if (!aIsRfabActive || aCasterFormId != 0x14u)
        return false;
    if (!std::isfinite(aMagnitude) || aMagnitude <= 0.0f)
        return false;
    if (aPrimaryAv != kHealthAv)
        return false;
    if ((aEffectFlags & kHostileOrDetrimental) == 0 || (aEffectFlags & kRecoverFlag) != 0)
        return false;

    return true;
}

[[nodiscard]] inline float ScalePlayerOutgoingSpellMagnitude(
    float aBaseMagnitude,
    float aCasterOutgoingMult,
    bool aShouldScale) noexcept
{
    if (!aShouldScale || !std::isfinite(aBaseMagnitude))
        return aBaseMagnitude;

    return aBaseMagnitude * SanitizeDifficultyMultiplier(aCasterOutgoingMult);
}

// PERK 0x000581F8:
//   - Vanilla Skyrim (Regeneration): 1.50f (+50%)
//   - RFAB.esp (RFAB_Perk_Restoration_ImprovedHealing / «Улучшенное лечение»): EPFD = 1.25f (+25%),
//     strictly on non-hostile permanent kHealth healing effects (excluding Absorb / Fortify).
[[nodiscard]] inline constexpr float GetRestorationImprovedHealingMultiplier(bool aIsRfabActive) noexcept
{
    return aIsRfabActive ? 1.25f : 1.5f;
}

[[nodiscard]] inline bool ShouldApplyImprovedHealingPerk(
    uint32_t aEffectFlags,
    int32_t aPrimaryAv,
    int32_t aArchetypeId,
    float aMagnitude) noexcept
{
    constexpr uint32_t kHostileOrDetrimental = (1u << 0) | (1u << 2);
    constexpr uint32_t kRecoverFlag = (1u << 1);
    constexpr int32_t kHealthAv = 24;
    constexpr int32_t kAbsorbArchetype = 8;

    if (!std::isfinite(aMagnitude) || aMagnitude <= 0.0f)
        return false;
    if (aPrimaryAv != kHealthAv || aArchetypeId == kAbsorbArchetype)
        return false;
    if ((aEffectFlags & (kHostileOrDetrimental | kRecoverFlag)) != 0)
        return false;

    return true;
}

struct CoopAllyActionPolicy
{
    bool BlockHarmfulAction{false};
    bool AllowPartySparring{false};
    bool AllowBeneficialMagic{false};
};

// Mirrors RFAB's Speechcraft 50 «Лидерство» -> «Единство в битве» (RFAB_Power_NoAllyDamage / RFAB_Perk_NoAllyDamage),
// scoped strictly to distinct Co-Op Party Allies (other players in the same party and their summons/minions),
// leaving solo NPC followers and personal summons governed by RFAB's native Speechcraft perk.
[[nodiscard]] inline constexpr CoopAllyActionPolicy EvaluateCoopAllyActionPolicy(
    bool aAreDistinctCoopPartyAllies,
    bool aAttackerAllyProtectionEnabled,
    bool aIsBeneficialHealOrBuff) noexcept
{
    if (!aAreDistinctCoopPartyAllies)
        return {};

    if (aIsBeneficialHealOrBuff)
        return {false, !aAttackerAllyProtectionEnabled, true};

    if (aAttackerAllyProtectionEnabled)
        return {true, false, false};

    return {false, true, false};
}

} // namespace RfabCompat
