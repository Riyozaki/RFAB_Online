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

namespace RfabCompat
{
inline constexpr std::string_view kRfabOnlineVersion = "2.0.0";

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
    std::vector<std::string> SecondaryPluginNames{
        "RFAB_BizarreAdventure.esp",
        "Requiem for a Balance.esp",
        "Requiem.esp",
    };

    std::string XpAmountGlobalEditorId{"RFAB_XP_Amount"};
    std::string XpBonusGlobalEditorId{"RFAB_XP_Multiplier"};
    std::string XpBooksFormListEditorId{"XPBooksReaded"};
    std::string NoDeathHandlingGlobalEditorId{"REQ_Combat_NoDeathHandling"};
    std::string KillCounterGlobalEditorId{"RFAB_TFOTG_Kill_Counter"};
    std::string XpHandlerQuestEditorId{"RFAB_XP_Handler"};
    std::string AdventurerRingEditorId{"RFAB_ExperienceRing"};
    std::string RestorationImprovedHealingPerkEditorId{"RFAB_Perk_Restoration_ImprovedHealing"};
    std::string UniversalAltarEditorId{"RFAB_BA_UniversalAltar"};
    std::string NoAllyDamageAllowedSpellsEditorId{"RFAB_List_NoAllyDamage_AllowedSpells"};

    // Optional last-resort base FormIDs only used if PO3 EditorID tables are completely absent.
    uint32_t FallbackXpAmountBaseFormId{0x00AD45BB};
    uint32_t FallbackAdventurerRingBaseFormId{0x00112812};
    uint32_t FallbackImprovedHealingPerkBaseFormId{0x000581F8};
    uint32_t FallbackAltarBaseFormId{0x00239FC5};
    uint32_t FallbackNoAllyDamageAllowedSpellsBaseFormId{0x00005CAA};

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

    // Stage 4: Co-Op Party Tactical HUD & 3D Ping System settings
    uint32_t TacticalPingVirtualKey{0x79};      // VK_F10 (121)
    uint32_t TacticalHudToggleVirtualKey{0x7A}; // VK_F11 (122)
    float TacticalPingDurationSec{12.0f};
    float TacticalPingCooldownSec{0.6f};
    uint32_t TacticalHudDefaultMode{1};         // 0=Hidden, 1=Full, 2=Compact, 3=MarkersOnly
    bool ShowBossEncounterBar{true};
    bool ShowDowned3dBeacons{true};
};

struct RfabLiveXpTable
{
    std::unordered_map<uint32_t, int32_t> LevelThresholds{};
    std::unordered_map<std::string, int32_t> QuestRewards{};
    std::unordered_map<std::string, float> XpMultipliers{};
    std::unordered_set<std::string> BossEditorIds{};
    std::unordered_set<uint32_t> BossBaseFormIds{};
    std::unordered_set<std::string> BossPluginFormKeys{};
    std::unordered_set<uint32_t> UnscopedBossBaseFormIds{};

    [[nodiscard]] static std::string FormatPluginFormKey(std::string_view aLowerPluginName, uint32_t aLocalFormId)
    {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "|%06X", aLocalFormId & 0x00FFFFFFu);
        std::string out(aLowerPluginName);
        out.append(buf);
        return out;
    }

    [[nodiscard]] int32_t GetThresholdForLevel(uint32_t aLevel) const noexcept
    {
        if (const auto it = LevelThresholds.find(aLevel); it != LevelThresholds.end())
            return it->second;
        return 0;
    }

    [[nodiscard]] bool ContainsBossBaseFormId(uint32_t aFormId) const noexcept
    {
        const uint32_t baseId = aFormId & 0x00FFFFFFu;
        return baseId != 0 && BossBaseFormIds.find(baseId) != BossBaseFormIds.end();
    }

    [[nodiscard]] bool ContainsBossForm(std::string_view aPluginName, uint32_t aLocalFormId) const noexcept
    {
        const uint32_t baseId = aLocalFormId & 0x00FFFFFFu;
        if (baseId == 0)
            return false;

        if (!aPluginName.empty())
        {
            std::string lowerPlugin(aPluginName);
            std::transform(lowerPlugin.begin(), lowerPlugin.end(), lowerPlugin.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (BossPluginFormKeys.find(FormatPluginFormKey(lowerPlugin, baseId)) != BossPluginFormKeys.end())
                return true;
            return UnscopedBossBaseFormIds.find(baseId) != UnscopedBossBaseFormIds.end();
        }

        return ContainsBossBaseFormId(baseId);
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

// Verifies that an `.ess` file on disk has a genuine Skyrim `TESV_SAVEGAME` header, intact screenshot,
// and complete non-truncated payload (catching 0-byte, header-only, screenshot-only, and ScrapHeap-failed saves).
inline bool IsValidEssFileOnDisk(const std::filesystem::path& acEssPath) noexcept
{
    constexpr std::string_view kMagic = "TESV_SAVEGAME";
    std::error_code ec;
    if (!std::filesystem::exists(acEssPath, ec) || !std::filesystem::is_regular_file(acEssPath, ec))
        return false;

    const uint64_t fileSize = std::filesystem::file_size(acEssPath, ec);
    if (ec || fileSize < 256)
        return false;

    std::ifstream ifs(acEssPath, std::ios::in | std::ios::binary);
    if (!ifs.is_open())
        return false;

    uint8_t prefix[17]{};
    ifs.read(reinterpret_cast<char*>(prefix), static_cast<std::streamsize>(sizeof(prefix)));
    if (ifs.gcount() != static_cast<std::streamsize>(sizeof(prefix)))
        return false;

    if (std::string_view(reinterpret_cast<const char*>(prefix), 13) != kMagic)
        return false;

    auto readU16Le = [](const uint8_t* p) noexcept -> uint16_t
    {
        return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
    };
    auto readU32Le = [](const uint8_t* p) noexcept -> uint32_t
    {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    };

    const uint32_t headerSize = readU32Le(prefix + 13);
    if (headerSize < 36 || headerSize > 65536 || (17ULL + headerSize) >= fileSize)
        return false;

    std::vector<uint8_t> hdr(headerSize);
    ifs.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(headerSize));
    if (ifs.gcount() != static_cast<std::streamsize>(headerSize))
        return false;

    size_t pos = 0;
    auto skipBytes = [&](size_t n) noexcept -> bool
    {
        if (pos + n > hdr.size())
            return false;
        pos += n;
        return true;
    };
    auto skipStr16 = [&]() noexcept -> bool
    {
        if (pos + 2 > hdr.size())
            return false;
        const uint16_t len = readU16Le(hdr.data() + pos);
        pos += 2;
        return skipBytes(len);
    };

    if (hdr.size() < 8)
        return false;
    const uint32_t version = readU32Le(hdr.data());
    if (version < 7 || version > 15)
        return false;

    pos = 8; // version (4) + saveNumber (4)
    if (!skipStr16()) // playerName
        return false;
    if (!skipBytes(4)) // playerLevel
        return false;
    if (!skipStr16() || !skipStr16() || !skipStr16()) // playerLocation, gameDate, playerRaceEditorId
        return false;
    if (!skipBytes(2 + 4 + 4 + 8)) // playerSex (2) + curExp (4) + lvlExp (4) + filetime (8)
        return false;

    if (pos + 8 > hdr.size())
        return false;
    const uint32_t ssWidth = readU32Le(hdr.data() + pos);
    const uint32_t ssHeight = readU32Le(hdr.data() + pos + 4);
    pos += 8;

    if (ssWidth > 16384 || ssHeight > 16384)
        return false;

    uint16_t compressionType = 0;
    if (version >= 12)
    {
        if (pos + 2 > hdr.size())
            return false;
        compressionType = readU16Le(hdr.data() + pos);
        pos += 2;
        if (compressionType > 2)
            return false;
    }

    if (pos != hdr.size())
        return false;

    const uint64_t bpp = (version >= 12) ? 4ULL : 3ULL;
    const uint64_t screenshotBytes = bpp * static_cast<uint64_t>(ssWidth) * static_cast<uint64_t>(ssHeight);
    const uint64_t bodyOffset = 17ULL + static_cast<uint64_t>(headerSize) + screenshotBytes;
    if (fileSize <= bodyOffset + 16ULL)
        return false;

    if (version >= 12 && compressionType != 0)
    {
        ifs.seekg(static_cast<std::streamoff>(bodyOffset), std::ios::beg);
        if (!ifs.good())
            return false;

        uint8_t sizesBuf[8]{};
        ifs.read(reinterpret_cast<char*>(sizesBuf), static_cast<std::streamsize>(sizeof(sizesBuf)));
        if (ifs.gcount() != static_cast<std::streamsize>(sizeof(sizesBuf)))
            return false;

        const uint32_t uncompressedLen = readU32Le(sizesBuf);
        const uint32_t compressedLen = readU32Le(sizesBuf + 4);
        if (uncompressedLen < 64 || compressedLen < 16)
            return false;

        if (bodyOffset + 8ULL + static_cast<uint64_t>(compressedLen) > fileSize)
            return false;
    }
    else if (fileSize < bodyOffset + 64ULL)
    {
        return false;
    }

    return true;
}

// Safely moves truncated / headerless / ScrapHeap-aborted `.ess` files into `_RFAB_CorruptQuarantine/`
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
        else if (EndsWithIgnoreCase(fname, ".ess.tmp") && !IsValidEssFileOnDisk(entry.path()))
        {
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
    Detail::ExtractJsonStringArray(aJson, "secondary_plugin_names", cfg.SecondaryPluginNames);
    Detail::ExtractJsonString(aJson, "xp_amount_global", cfg.XpAmountGlobalEditorId);
    Detail::ExtractJsonString(aJson, "xp_bonus_global", cfg.XpBonusGlobalEditorId);
    Detail::ExtractJsonString(aJson, "xp_books_formlist", cfg.XpBooksFormListEditorId);
    Detail::ExtractJsonString(aJson, "no_death_handling_global", cfg.NoDeathHandlingGlobalEditorId);
    Detail::ExtractJsonString(aJson, "kill_counter_global", cfg.KillCounterGlobalEditorId);
    Detail::ExtractJsonString(aJson, "xp_handler_quest", cfg.XpHandlerQuestEditorId);
    Detail::ExtractJsonString(aJson, "adventurer_ring", cfg.AdventurerRingEditorId);
    Detail::ExtractJsonString(aJson, "restoration_improved_healing_perk", cfg.RestorationImprovedHealingPerkEditorId);
    Detail::ExtractJsonString(aJson, "universal_altar", cfg.UniversalAltarEditorId);
    Detail::ExtractJsonString(aJson, "no_ally_damage_allowed_spells", cfg.NoAllyDamageAllowedSpellsEditorId);

    if (const size_t fbBlock = aJson.find("\"fallback_base_form_ids\""); fbBlock != std::string_view::npos)
    {
        const std::string_view fbSlice = aJson.substr(fbBlock);
        std::string fbHex;
        if (Detail::ExtractJsonString(fbSlice, "xp_amount_global", fbHex))
            cfg.FallbackXpAmountBaseFormId = static_cast<uint32_t>(std::strtoul(fbHex.c_str(), nullptr, 0));
        if (Detail::ExtractJsonString(fbSlice, "adventurer_ring", fbHex))
            cfg.FallbackAdventurerRingBaseFormId = static_cast<uint32_t>(std::strtoul(fbHex.c_str(), nullptr, 0));
        if (Detail::ExtractJsonString(fbSlice, "restoration_improved_healing_perk", fbHex))
            cfg.FallbackImprovedHealingPerkBaseFormId = static_cast<uint32_t>(std::strtoul(fbHex.c_str(), nullptr, 0));
        if (Detail::ExtractJsonString(fbSlice, "universal_altar", fbHex))
            cfg.FallbackAltarBaseFormId = static_cast<uint32_t>(std::strtoul(fbHex.c_str(), nullptr, 0));
        if (Detail::ExtractJsonString(fbSlice, "no_ally_damage_allowed_spells", fbHex))
            cfg.FallbackNoAllyDamageAllowedSpellsBaseFormId = static_cast<uint32_t>(std::strtoul(fbHex.c_str(), nullptr, 0));
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

    if (Detail::ExtractJsonFloat(aJson, "tactical_ping_virtual_key", numVal) && numVal > 0.0f && numVal <= 255.0f)
        cfg.TacticalPingVirtualKey = static_cast<uint32_t>(numVal);
    if (Detail::ExtractJsonFloat(aJson, "tactical_hud_toggle_virtual_key", numVal) && numVal > 0.0f && numVal <= 255.0f)
        cfg.TacticalHudToggleVirtualKey = static_cast<uint32_t>(numVal);
    if (Detail::ExtractJsonFloat(aJson, "tactical_ping_duration_sec", numVal) && numVal >= 2.0f && numVal <= 120.0f)
        cfg.TacticalPingDurationSec = numVal;
    if (Detail::ExtractJsonFloat(aJson, "tactical_ping_cooldown_sec", numVal) && numVal >= 0.1f && numVal <= 30.0f)
        cfg.TacticalPingCooldownSec = numVal;
    if (Detail::ExtractJsonFloat(aJson, "tactical_hud_default_mode", numVal) && numVal >= 0.0f && numVal <= 3.0f)
        cfg.TacticalHudDefaultMode = static_cast<uint32_t>(numVal);
    Detail::ExtractJsonBool(aJson, "show_boss_encounter_bar", cfg.ShowBossEncounterBar);
    Detail::ExtractJsonBool(aJson, "show_downed_3d_beacons", cfg.ShowDowned3dBeacons);

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

// Parses RFAB's Data/SKSE/Plugins/StorageUtilData/Experience.ini (e.g. "Level 2 = 2500", "2 = 2500", or "[Bosses]" entries).
inline void ParseExperienceIni(std::string_view aContent, RfabLiveXpTable& aTable)
{
    std::istringstream iss{std::string(aContent)};
    std::string line;
    std::string currentSection;
    while (std::getline(iss, line))
    {
        const size_t commentPos = line.find_first_of(";#");
        if (commentPos != std::string::npos)
            line.erase(commentPos);

        const std::string trimmedLine = Detail::Trim(line);
        if (trimmedLine.size() >= 2 && trimmedLine.front() == '[' && trimmedLine.back() == ']')
        {
            currentSection = Detail::ToLowerAscii(Detail::Trim(std::string_view(trimmedLine).substr(1, trimmedLine.size() - 2)));
            continue;
        }

        const size_t eqPos = line.find('=');
        if (eqPos == std::string::npos)
            continue;

        std::string lhs = Detail::Trim(std::string_view(line).substr(0, eqPos));
        std::string rhs = Detail::Trim(std::string_view(line).substr(eqPos + 1));
        if (lhs.empty() || rhs.empty())
            continue;

        if (currentSection.find("boss") != std::string::npos)
        {
            const std::string lowerLhs = Detail::ToLowerAscii(lhs);
            if (const size_t hexPos = lowerLhs.find("0x"); hexPos != std::string::npos)
            {
                const uint32_t formId = static_cast<uint32_t>(std::strtoul(lowerLhs.c_str() + hexPos, nullptr, 16)) & 0x00FFFFFFu;
                if (formId != 0)
                {
                    aTable.BossBaseFormIds.insert(formId);
                    std::string pluginPart = Detail::Trim(std::string_view(lowerLhs).substr(0, hexPos));
                    if (!pluginPart.empty() && (pluginPart.back() == '|' || pluginPart.back() == ':'))
                        pluginPart.pop_back();
                    if (const size_t sep = pluginPart.rfind('|'); sep != std::string::npos)
                        pluginPart = Detail::Trim(std::string_view(pluginPart).substr(sep + 1));
                    if (!pluginPart.empty() &&
                        (Detail::EndsWithIgnoreCase(pluginPart, ".esp") ||
                         Detail::EndsWithIgnoreCase(pluginPart, ".esm") ||
                         Detail::EndsWithIgnoreCase(pluginPart, ".esl")))
                    {
                        aTable.BossPluginFormKeys.insert(RfabLiveXpTable::FormatPluginFormKey(pluginPart, formId));
                    }
                    else
                    {
                        aTable.UnscopedBossBaseFormIds.insert(formId);
                    }
                }
            }
            else
            {
                aTable.BossEditorIds.insert(lowerLhs);
            }
            continue;
        }

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

// Parses RFAB's BossList.json / RFAB_BossList.json (StorageUtil JSON with __formData|Plugin|0xHEX or EditorID keys).
inline void ParseBossListJson(std::string_view aJson, RfabLiveXpTable& aTable)
{
    static constexpr std::string_view kIgnoredJsonKeys[] = {
        "int", "float", "string", "form", "intlist", "floatlist", "stringlist", "formlist"
    };

    size_t pos = 0;
    while (pos < aJson.size())
    {
        const size_t q1 = aJson.find('"', pos);
        if (q1 == std::string_view::npos)
            break;
        const size_t q2 = aJson.find('"', q1 + 1);
        if (q2 == std::string_view::npos)
            break;

        const std::string token = Detail::Trim(aJson.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;
        if (token.empty())
            continue;

        const std::string lower = Detail::ToLowerAscii(token);
        bool isStructuralKey = false;
        for (const auto& ignored : kIgnoredJsonKeys)
        {
            if (lower == ignored)
            {
                isStructuralKey = true;
                break;
            }
        }
        if (isStructuralKey)
            continue;

        if (const size_t hexPos = lower.find("0x"); hexPos != std::string::npos)
        {
            const uint32_t baseId = static_cast<uint32_t>(std::strtoul(lower.c_str() + hexPos, nullptr, 16)) & 0x00FFFFFFu;
            if (baseId != 0)
            {
                aTable.BossBaseFormIds.insert(baseId);
                std::string pluginPart = Detail::Trim(std::string_view(lower).substr(0, hexPos));
                if (!pluginPart.empty() && (pluginPart.back() == '|' || pluginPart.back() == ':'))
                    pluginPart.pop_back();
                if (const size_t sep = pluginPart.rfind('|'); sep != std::string::npos)
                    pluginPart = Detail::Trim(std::string_view(pluginPart).substr(sep + 1));
                if (!pluginPart.empty() &&
                    (Detail::EndsWithIgnoreCase(pluginPart, ".esp") ||
                     Detail::EndsWithIgnoreCase(pluginPart, ".esm") ||
                     Detail::EndsWithIgnoreCase(pluginPart, ".esl")))
                {
                    aTable.BossPluginFormKeys.insert(RfabLiveXpTable::FormatPluginFormKey(pluginPart, baseId));
                }
                else
                {
                    aTable.UnscopedBossBaseFormIds.insert(baseId);
                }
            }
        }
        else if (lower.find(".esp") == std::string::npos &&
                 lower.find(".esm") == std::string::npos &&
                 lower.find(".esl") == std::string::npos)
        {
            aTable.BossEditorIds.insert(lower);
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

        const auto profilesDir = mo2Root / "profiles";
        if (std::filesystem::exists(profilesDir, ec) && std::filesystem::is_directory(profilesDir, ec))
        {
            for (const auto& profEntry : std::filesystem::directory_iterator(profilesDir, ec))
            {
                if (!ec && profEntry.is_directory(ec))
                {
                    Detail::QuarantineCorruptEssFilesInDirectory(profEntry.path() / "saves");
                }
            }
        }

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
// while still rejecting incompatible protocol families (e.g. v1.0.x vs v1.1.x+ or v2.0.x).
[[nodiscard]] inline bool AreBuildVersionsProtocolCompatible(std::string_view aServerVer, std::string_view aClientVer) noexcept
{
    if (aServerVer == aClientVer)
        return true;

    int sMaj = 0, sMin = 0;
    int cMaj = 0, cMin = 0;
    if (ExtractMajorMinorProtocolTag(aServerVer, sMaj, sMin) &&
        ExtractMajorMinorProtocolTag(aClientVer, cMaj, cMin))
    {
        if (sMaj != cMaj)
            return false;
        if (sMin == cMin)
            return true;
        // v1.1.x and v1.2.x share the exact same network packet schema (including AllyProtectionToggle).
        if (sMaj == 1 && sMin >= 1 && cMin >= 1)
            return true;
        // v2.0.x and v2.1.x share the exact same wire network packet schema (no ActorData or opcode wire changes).
        if (sMaj == 2 && sMin <= 1 && cMin <= 1)
            return true;
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

// Cross-version RFAB & Skyrim Boss EditorID recognizer (supplements BossList.json, Dragon check, and MaxHP threshold).
[[nodiscard]] inline bool IsRfabBossEditorId(
    std::string_view aEditorId,
    const RfabLiveXpTable* apLiveTable = nullptr) noexcept
{
    const std::string lower = Detail::ToLowerAscii(Detail::Trim(aEditorId));
    if (lower.empty())
        return false;

    if (apLiveTable && apLiveTable->BossEditorIds.find(lower) != apLiveTable->BossEditorIds.end())
        return true;

    static constexpr std::string_view kBossMarkers[] = {
        "rfab_boss_",
        "req_boss_",
        "dragonpriest",
        "miraak",
        "harkon",
        "alduin",
        "vyrthur",
        "malkoran",
        "durnehviir",
        "ebonywarrior",
        "karstaag",
        "forgemaster",
        "centurion",
        "soulcairnkeeper",
        "dlc01boneyardkeeper",
        "reaper",
        "orchendor",
        "movarth",
        "potema",
        "gauldur",
        "jyrik",
        "mikrul",
        "sigdis",
        "kvenel",
        "redeagle",
        "curalmil",
        "krosis",
        "morokei",
        "nahkriin",
        "otar",
        "rahgot",
        "vokun",
        "volsung",
        "hevnoraak",
        "ahzidal",
        "dukaan",
        "zakhriisos",
        "vahlok",
    };

    for (const auto& marker : kBossMarkers)
    {
        if (lower.find(marker) != std::string::npos)
            return true;
    }

    return false;
}

// Normalizes transitional TESObjectREFR::OpenState values (kOpening=2 -> kOpen=1, kClosing=4 -> kClosed=3)
// so door activations never permanently desync when one client is 1 frame into a transition.
[[nodiscard]] inline constexpr uint8_t NormalizeDoorOpenState(uint8_t aOpenState) noexcept
{
    if (aOpenState == 2u) // kOpening -> kOpen
        return 1u;
    if (aOpenState == 4u) // kClosing -> kClosed
        return 3u;
    return aOpenState;
}

[[nodiscard]] inline constexpr bool AreDoorStatesCompatibleForSync(uint8_t aRemotePreState, uint8_t aLocalCurrentState) noexcept
{
    if (aRemotePreState == 0u || aLocalCurrentState == 0u)
        return true;
    return NormalizeDoorOpenState(aRemotePreState) == NormalizeDoorOpenState(aLocalCurrentState);
}

// Cross-version RFAB & Skyrim quest filter: blocks personal transformation, MCM, UI, widget, and background
// maintenance quests from being synced across the party, regardless of load order.
[[nodiscard]] inline bool IsQuestNonSyncableByEditorIdOrBaseId(
    std::string_view aEditorId,
    uint32_t aFormId) noexcept
{
    const std::string lower = Detail::ToLowerAscii(Detail::Trim(aEditorId));
    if (lower.empty())
    {
        // Only fall back to 24-bit base FormID when EditorID is unavailable AND the form is NOT an ESL light record
        // (`0xFExxxxxx`), preventing cross-mod 24-bit collisions when a story quest provides a real EditorID!
        if ((aFormId & 0xFF000000u) != 0xFE000000u)
        {
            const uint32_t baseId = aFormId & 0x00FFFFFFu;
            switch (baseId)
            {
            case 0x02BA16u: // PlayerWerewolfQuest
            case 0x0071D0u: // DLC1PlayerVampireQuest
            case 0x03AC44u: // MS13BleakFallsBarrowLeverScene
            case 0x0F2593u: // SkillExperienceQuest
                return true;
            default:
                break;
            }
        }
        return false;
    }

    if (lower == "playerwerewolfquest" ||
        lower == "dlc1playervampirequest" ||
        lower == "ms13bleakfallsbarrowleverscene" ||
        lower == "skillexperiencequest")
    {
        return true;
    }

    static constexpr std::string_view kBlockedPrefixes[] = {
        "ski_",
        "req_",
        "rfab_mcm",
        "rfab_xp",
        "rfab_init",
        "rfab_setup",
        "rfab_menu",
        "rfab_settings",
        "rfab_updater",
        "rfab_smelter",
        "rfab_system",
        "rfab_keybind",
        "rfab_hotkey",
        "rfab_autocast",
        "rfab_ui",
        "rfab_widget",
        "rfab_allyhit",
        "xpmse",
        "fnis",
        "racemenu",
        "hdtsmp",
        "nemesis",
        "morehud",
        "ahzmorehud",
        "ihud",
        "truehud",
        "wheeler",
        "soulsre",
        "skyrimsouls",
        "customskills",
        "dyndolod",
        "uiextensions",
        "dialoguefollower",
        "relationshipmarriage",
        "byohhouse",
        "wicastmagic",
        "wispell",
        "wikill",
        "wiassault",
        "witheft",
        "wicraftitem",
        "wichange",
        "tutorials",
    };

    for (const auto& prefix : kBlockedPrefixes)
    {
        if (lower.rfind(prefix, 0) == 0)
            return true;
    }

    static constexpr std::string_view kBlockedSubstrings[] = {
        "playerwerewolf",
        "playervampire",
        "vampiretracking",
        "werewolftracking",
        "_mcm",
        "mcm_",
        "configmanager",
        "widgetmanager",
        "favoritesmanager",
    };

    for (const auto& sub : kBlockedSubstrings)
    {
        if (lower.find(sub) != std::string::npos)
            return true;
    }

    return false;
}

// Identifies real dungeon, puzzle, claw-door, Dragon Priest, bounty, and story quests that Skyrim/RFAB
// tag with TESQuest::Type::None (0) or TESQuest::Type::Miscellaneous (6) so they sync in co-op.
[[nodiscard]] inline bool IsDungeonOrStoryQuestEditorId(std::string_view aEditorId) noexcept
{
    const std::string lower = Detail::ToLowerAscii(Detail::Trim(aEditorId));
    if (lower.empty())
        return false;

    if (IsQuestNonSyncableByEditorIdOrBaseId(lower, 0))
        return false;

    static constexpr std::string_view kStoryOrDungeonPrefixes[] = {
        "dun",
        "ms",
        "mq",
        "da",
        "mg",
        "tg",
        "db",
        "c0",
        "cr",
        "cw",
        "dlc1",
        "dlc2",
        "bq",
        "freeform",
        "t01",
        "t02",
        "t03",
        "bards",
        "rfab_quest",
        "rfab_qst",
        "rfab_boss",
        "rfab_arena",
        "rfab_contract",
        "rfab_bounty",
        "rfab_dungeon",
    };

    for (const auto& prefix : kStoryOrDungeonPrefixes)
    {
        if (lower.rfind(prefix, 0) == 0)
            return true;
    }

    static constexpr std::string_view kStoryOrDungeonSubstrings[] = {
        "qst",
        "quest",
        "puzzle",
        "claw",
        "dragonpriest",
        "bounty",
        "arena",
        "gauldur",
        "forelhost",
        "valthume",
        "ragnvald",
        "folgunthur",
        "bleakfalls",
        "labyrinthian",
        "blackreach",
        "arkngthamz",
        "saarthal",
        "midden",
        "potema",
        "wolfskull",
        "frostflow",
        "ansilvund",
        "angarvunde",
        "highgate",
        "volunruud",
        "shroudhearth",
        "yngol",
        "redeagle",
        "kagrumez",
        "kolbjorn",
        "bloodskal",
        "fahlbtharz",
        "nchardak",
        "gyldenhul",
        "karstaag",
    };

    for (const auto& sub : kStoryOrDungeonSubstrings)
    {
        if (lower.find(sub) != std::string::npos)
            return true;
    }

    return false;
}

// Cross-version RFAB / Requiem / Skyrim helper for non-hostile kScript MagicEffects:
// distinguishes genuine party buffs, auras, heals, shields, and summon empowerments from
// unflagged crowd-control / knockdown / token / internal maintenance script effects.
[[nodiscard]] inline bool IsFriendlyScriptBuffEffectEditorId(std::string_view aEditorId) noexcept
{
    const std::string lower = Detail::ToLowerAscii(Detail::Trim(aEditorId));
    if (lower.empty())
        return false;

    static constexpr std::string_view kHostileOrInternalMarkers[] = {
        "damage",
        "drain",
        "push",
        "stagger",
        "knock",
        "curse",
        "debuff",
        "poison",
        "fear",
        "disarm",
        "paraly",
        "slow",
        "freeze",
        "burn",
        "shock",
        "bleed",
        "disease",
        "rot",
        "decay",
        "hex",
        "kill",
        "summonspirit",
        "token",
        "mcm",
        "smelter",
    };

    for (const auto& bad : kHostileOrInternalMarkers)
    {
        if (lower.find(bad) != std::string::npos)
            return false;
    }

    static constexpr std::string_view kFriendlyBuffMarkers[] = {
        "buff",
        "heal",
        "mend",
        "restore",
        "shield",
        "ward",
        "armor",
        "flesh",
        "protect",
        "bless",
        "rally",
        "courage",
        "lionheart",
        "inspire",
        "aura",
        "valor",
        "haste",
        "speed",
        "fortify",
        "regen",
        "empower",
        "sanctuary",
        "grace",
        "aegis",
        "barrier",
        "resist",
        "immun",
        "cleanse",
        "cure",
        "totem",
        "banner",
        "voice",
        "pact",
        "bond",
        "link",
        "ally",
        "party",
        "companion",
        "minion",
        "undead",
        "automaton",
        "stance",
    };

    for (const auto& good : kFriendlyBuffMarkers)
    {
        if (lower.find(good) != std::string::npos)
            return true;
    }

    return false;
}

// ============================================================================
// Stage 4: Co-Op Party Tactical HUD & 3D Ping System
// ============================================================================

enum class TacticalPingKind : uint8_t
{
    Rally = 0,
    Enemy = 1,
    EnemyBoss = 2,
    Danger = 3,
    Loot = 4,
    Objective = 5,
    AssistAlly = 6,
    ClearSenderPings = 7,
};

enum class TacticalHudMode : uint8_t
{
    Hidden = 0,
    Full = 1,
    Compact = 2,
    MarkersOnly = 3,
};

[[nodiscard]] inline TacticalHudMode CycleTacticalHudMode(TacticalHudMode aCurrent) noexcept
{
    switch (aCurrent)
    {
    case TacticalHudMode::Full: return TacticalHudMode::Compact;
    case TacticalHudMode::Compact: return TacticalHudMode::MarkersOnly;
    case TacticalHudMode::MarkersOnly: return TacticalHudMode::Hidden;
    case TacticalHudMode::Hidden:
    default: return TacticalHudMode::Full;
    }
}

[[nodiscard]] inline const char* GetTacticalHudModeNameRu(TacticalHudMode aMode) noexcept
{
    switch (aMode)
    {
    case TacticalHudMode::Full: return "Полный (Отряд + Босс + 3D-метки)";
    case TacticalHudMode::Compact: return "Компактный (Здоровье + 3D-метки)";
    case TacticalHudMode::MarkersOnly: return "Только 3D-метки и реанимация";
    case TacticalHudMode::Hidden:
    default: return "Скрыт";
    }
}

[[nodiscard]] inline const char* GetTacticalPingTitleRu(TacticalPingKind aKind) noexcept
{
    switch (aKind)
    {
    case TacticalPingKind::Enemy: return "АТАКОВАТЬ ЦЕЛЬ";
    case TacticalPingKind::EnemyBoss: return "БОСС / ГЛАВНАЯ ЦЕЛЬ";
    case TacticalPingKind::Danger: return "ОПАСНОСТЬ!";
    case TacticalPingKind::Loot: return "ДОБЫЧА / СУНДУК";
    case TacticalPingKind::Objective: return "МЕХАНИЗМ / ОБЪЕКТ";
    case TacticalPingKind::AssistAlly: return "ИДУ НА ПОМОЩЬ!";
    case TacticalPingKind::ClearSenderPings: return "МЕТКИ СНЯТЫ";
    case TacticalPingKind::Rally:
    default: return "ТОЧКА СБОРА";
    }
}

[[nodiscard]] inline const char* GetTacticalPingBadgeAscii(TacticalPingKind aKind) noexcept
{
    switch (aKind)
    {
    case TacticalPingKind::Enemy: return "[!]";
    case TacticalPingKind::EnemyBoss: return "[БОСС]";
    case TacticalPingKind::Danger: return "[!!]";
    case TacticalPingKind::Loot: return "[*]";
    case TacticalPingKind::Objective: return "[?]";
    case TacticalPingKind::AssistAlly: return "[+]";
    case TacticalPingKind::ClearSenderPings: return "[-]";
    case TacticalPingKind::Rally:
    default: return "[^]";
    }
}

struct TacticalPingPayload
{
    TacticalPingKind Kind{TacticalPingKind::Rally};
    uint32_t SenderPlayerId{0};
    uint32_t TargetServerId{0};
    uint32_t WorldSpaceBaseId{0};
    uint32_t CellBaseId{0};
    float PosX{0.0f};
    float PosY{0.0f};
    float PosZ{0.0f};
    std::string SenderName{};
    std::string TargetLabel{};
};

namespace Detail
{
inline std::string SanitizeTacticalField(std::string_view aInput, size_t aMaxBytes = 64)
{
    std::string out;
    out.reserve(std::min(aInput.size(), aMaxBytes));
    for (char ch : aInput)
    {
        if (out.size() >= aMaxBytes)
            break;
        if (ch == '|' || ch == '\r' || ch == '\n' || ch == '\t' || static_cast<unsigned char>(ch) < 0x20)
            out.push_back(' ');
        else
            out.push_back(ch);
    }
    return Trim(out);
}
} // namespace Detail

[[nodiscard]] inline std::string SerializeTacticalPingPayload(const TacticalPingPayload& acPayload)
{
    char buf[256]{};
    const float safeX = std::isfinite(acPayload.PosX) ? acPayload.PosX : 0.0f;
    const float safeY = std::isfinite(acPayload.PosY) ? acPayload.PosY : 0.0f;
    const float safeZ = std::isfinite(acPayload.PosZ) ? acPayload.PosZ : 0.0f;

    std::snprintf(
        buf,
        sizeof(buf),
        "tp1|%u|%u|%u|%u|%u|%.2f|%.2f|%.2f|",
        static_cast<unsigned>(acPayload.Kind),
        static_cast<unsigned>(acPayload.SenderPlayerId),
        static_cast<unsigned>(acPayload.TargetServerId),
        static_cast<unsigned>(acPayload.WorldSpaceBaseId),
        static_cast<unsigned>(acPayload.CellBaseId),
        safeX,
        safeY,
        safeZ);

    std::string encoded(buf);
    encoded += Detail::SanitizeTacticalField(acPayload.SenderName, 48);
    encoded.push_back('|');
    encoded += Detail::SanitizeTacticalField(acPayload.TargetLabel, 64);
    return encoded;
}

[[nodiscard]] inline bool DeserializeTacticalPingPayload(std::string_view aEncoded, TacticalPingPayload& aOut) noexcept
{
    aOut = {};
    if (aEncoded.size() < 16 || aEncoded.substr(0, 4) != "tp1|")
        return false;

    std::vector<std::string_view> parts;
    parts.reserve(11);
    size_t start = 0;
    while (start <= aEncoded.size())
    {
        const size_t sep = aEncoded.find('|', start);
        if (sep == std::string_view::npos)
        {
            parts.push_back(aEncoded.substr(start));
            break;
        }
        parts.push_back(aEncoded.substr(start, sep - start));
        start = sep + 1;
    }

    if (parts.size() < 11)
        return false;

    const auto parseU32 = [](std::string_view sv, uint32_t& outVal) noexcept -> bool
    {
        if (sv.empty())
            return false;
        char tmp[32]{};
        const size_t n = std::min(sv.size(), sizeof(tmp) - 1);
        std::memcpy(tmp, sv.data(), n);
        char* endPtr = nullptr;
        const unsigned long val = std::strtoul(tmp, &endPtr, 10);
        if (!endPtr || endPtr == tmp)
            return false;
        outVal = static_cast<uint32_t>(val);
        return true;
    };

    const auto parseFloat = [](std::string_view sv, float& outVal) noexcept -> bool
    {
        if (sv.empty())
            return false;
        char tmp[48]{};
        const size_t n = std::min(sv.size(), sizeof(tmp) - 1);
        std::memcpy(tmp, sv.data(), n);
        char* endPtr = nullptr;
        const float val = std::strtof(tmp, &endPtr);
        if (!endPtr || endPtr == tmp || !std::isfinite(val))
            return false;
        outVal = val;
        return true;
    };

    uint32_t rawKind = 0;
    if (!parseU32(parts[1], rawKind) || rawKind > static_cast<uint32_t>(TacticalPingKind::ClearSenderPings))
        return false;
    aOut.Kind = static_cast<TacticalPingKind>(rawKind);

    if (!parseU32(parts[2], aOut.SenderPlayerId))
        return false;
    if (!parseU32(parts[3], aOut.TargetServerId))
        return false;
    if (!parseU32(parts[4], aOut.WorldSpaceBaseId))
        return false;
    if (!parseU32(parts[5], aOut.CellBaseId))
        return false;
    if (!parseFloat(parts[6], aOut.PosX))
        return false;
    if (!parseFloat(parts[7], aOut.PosY))
        return false;
    if (!parseFloat(parts[8], aOut.PosZ))
        return false;

    aOut.SenderName = Detail::SanitizeTacticalField(parts[9], 48);
    aOut.TargetLabel = Detail::SanitizeTacticalField(parts[10], 64);
    return true;
}

[[nodiscard]] inline TacticalPingKind ClassifyCrosshairTacticalPingKind(
    bool aShiftHeld,
    bool aIsTargetPlayer,
    bool aIsTargetDownedPlayer,
    bool aIsTargetBossActor,
    bool aIsTargetHostileActor,
    bool aIsTargetDeadActor,
    uint8_t aFormType) noexcept
{
    if (aShiftHeld)
        return TacticalPingKind::Danger;

    if (aIsTargetDownedPlayer)
        return TacticalPingKind::AssistAlly;

    if (aIsTargetPlayer)
        return TacticalPingKind::Rally;

    if (aIsTargetDeadActor)
        return TacticalPingKind::Loot;

    if (aIsTargetBossActor)
        return TacticalPingKind::EnemyBoss;

    if (aIsTargetHostileActor)
        return TacticalPingKind::Enemy;

    // Skyrim FormType values:
    // Activator = 24, TalkingActivator = 25, Book = 27, Container = 28, Door = 29,
    // Ingredient = 30, Misc = 32, Apparatus = 33, Ammo = 42, Key = 45, Potion = 46, Scroll = 23, Armor = 26, Weapon = 41
    switch (aFormType)
    {
    case 23: // Scroll
    case 26: // Armor
    case 27: // Book
    case 28: // Container (Chest / Safe / Urn)
    case 30: // Ingredient
    case 32: // Misc
    case 41: // Weapon
    case 42: // Ammo
    case 45: // Key
    case 46: // Potion
    case 52: // SoulGem
        return TacticalPingKind::Loot;

    case 24: // Activator (Lever / Button / Puzzle / Shrine / Altar)
    case 25: // TalkingActivator
    case 29: // Door / Gate
    case 40: // Furniture
        return TacticalPingKind::Objective;

    default:
        return TacticalPingKind::Rally;
    }
}

struct ScreenMarkerProjection
{
    float ScreenX{0.0f};
    float ScreenY{0.0f};
    float DirX{0.0f};
    float DirY{-1.0f};
    bool IsOnScreen{false};
    bool IsClampedToEdge{false};
};

// Projects normalized WorldPtToScreenPt3 coordinates (aNormX in [0..1], aNormY in [0..1] bottom-up, aDepthZ)
// onto viewport pixels (aViewportW x aViewportH), clamping off-screen and behind-camera (aDepthZ < 0)
// targets onto the screen border with an accurate directional unit vector (DirX, DirY).
[[nodiscard]] inline ScreenMarkerProjection ComputeTacticalMarkerScreenPlacement(
    float aNormX,
    float aNormY,
    float aDepthZ,
    float aViewportW,
    float aViewportH,
    float aMarginPx = 48.0f) noexcept
{
    ScreenMarkerProjection out{};
    const float w = std::max(aViewportW, 320.0f);
    const float h = std::max(aViewportH, 200.0f);
    const float margin = std::clamp(aMarginPx, 16.0f, std::min(w, h) * 0.35f);

    const float centerX = w * 0.5f;
    const float centerY = h * 0.5f;

    // Skyrim's WorldPtToScreenPt3 outputs Y in [0..1] bottom-to-top, so top-down screen Y is (1.0f - aNormY).
    const float rawX = aNormX * w;
    const float rawY = (1.0f - aNormY) * h;

    const bool inFront = (aDepthZ >= 0.0f);
    if (inFront && rawX >= margin && rawX <= (w - margin) && rawY >= margin && rawY <= (h - margin))
    {
        out.ScreenX = rawX;
        out.ScreenY = rawY;
        out.DirX = 0.0f;
        out.DirY = 1.0f;
        out.IsOnScreen = true;
        out.IsClampedToEdge = false;
        return out;
    }

    // When behind the camera (aDepthZ < 0), perspective division by negative W mirrors rawX/rawY across center.
    // Invert the center-relative offset so the edge arrow always points toward the true 3D location!
    float relX = rawX - centerX;
    float relY = rawY - centerY;
    if (!inFront)
    {
        relX = -relX;
        relY = -relY;
        // If directly behind camera center, bias slightly upward so direction is well-defined
        if (std::fabs(relX) < 1.0f && std::fabs(relY) < 1.0f)
            relY = -1.0f;
    }

    const float len = std::sqrt(relX * relX + relY * relY);
    if (len > 1e-4f)
    {
        out.DirX = relX / len;
        out.DirY = relY / len;
    }
    else
    {
        out.DirX = 0.0f;
        out.DirY = -1.0f;
    }

    const float halfW = std::max(centerX - margin, 16.0f);
    const float halfH = std::max(centerY - margin, 16.0f);

    const float scaleX = (std::fabs(out.DirX) > 1e-5f) ? (halfW / std::fabs(out.DirX)) : 1e9f;
    const float scaleY = (std::fabs(out.DirY) > 1e-5f) ? (halfH / std::fabs(out.DirY)) : 1e9f;
    const float edgeScale = std::min(scaleX, scaleY);

    out.ScreenX = std::clamp(centerX + out.DirX * edgeScale, margin, w - margin);
    out.ScreenY = std::clamp(centerY + out.DirY * edgeScale, margin, h - margin);
    out.IsOnScreen = false;
    out.IsClampedToEdge = true;
    return out;
}

struct TacticalChatCommand
{
    enum class Action : uint8_t
    {
        None = 0,
        TriggerContextualPing = 1,
        TriggerExplicitPing = 2,
        ClearPings = 3,
        CycleHudMode = 4,
        SetHudMode = 5,
    };

    Action Kind{Action::None};
    TacticalPingKind PingKind{TacticalPingKind::Rally};
    TacticalHudMode HudMode{TacticalHudMode::Full};
    std::string CustomLabel{};
};

[[nodiscard]] inline bool ParseTacticalChatCommand(std::string_view aRawChatText, TacticalChatCommand& aOut)
{
    aOut = {};
    const std::string trimmed = Detail::Trim(aRawChatText);
    if (trimmed.empty() || trimmed.front() != '/')
        return false;

    std::string cmdToken;
    std::string argRest;
    if (const size_t sp = trimmed.find_first_of(" \t"); sp != std::string::npos)
    {
        cmdToken = Detail::ToLowerAscii(trimmed.substr(0, sp));
        argRest = Detail::Trim(trimmed.substr(sp + 1));
    }
    else
    {
        cmdToken = Detail::ToLowerAscii(trimmed);
    }

    if (cmdToken == "/hud" || cmdToken == "/tacticalhud")
    {
        const std::string argLower = Detail::ToLowerAscii(argRest);
        if (argLower.empty() || argLower == "toggle" || argLower == "cycle")
        {
            aOut.Kind = TacticalChatCommand::Action::CycleHudMode;
            return true;
        }
        if (argLower == "full" || argLower == "on" || argLower == "1")
        {
            aOut.Kind = TacticalChatCommand::Action::SetHudMode;
            aOut.HudMode = TacticalHudMode::Full;
            return true;
        }
        if (argLower == "compact" || argLower == "mini" || argLower == "2")
        {
            aOut.Kind = TacticalChatCommand::Action::SetHudMode;
            aOut.HudMode = TacticalHudMode::Compact;
            return true;
        }
        if (argLower == "markers" || argLower == "pings" || argLower == "3")
        {
            aOut.Kind = TacticalChatCommand::Action::SetHudMode;
            aOut.HudMode = TacticalHudMode::MarkersOnly;
            return true;
        }
        if (argLower == "off" || argLower == "hide" || argLower == "hidden" || argLower == "0")
        {
            aOut.Kind = TacticalChatCommand::Action::SetHudMode;
            aOut.HudMode = TacticalHudMode::Hidden;
            return true;
        }
        aOut.Kind = TacticalChatCommand::Action::CycleHudMode;
        return true;
    }

    if (cmdToken == "/ping" || cmdToken == "/mark" || cmdToken == "/метка")
    {
        if (argRest.empty())
        {
            aOut.Kind = TacticalChatCommand::Action::TriggerContextualPing;
            return true;
        }

        std::string subToken;
        std::string customNote;
        if (const size_t sp2 = argRest.find_first_of(" \t"); sp2 != std::string::npos)
        {
            subToken = Detail::ToLowerAscii(argRest.substr(0, sp2));
            customNote = Detail::Trim(argRest.substr(sp2 + 1));
        }
        else
        {
            subToken = Detail::ToLowerAscii(argRest);
        }

        if (subToken == "clear" || subToken == "off" || subToken == "none" || subToken == "снять")
        {
            aOut.Kind = TacticalChatCommand::Action::ClearPings;
            return true;
        }
        if (subToken == "danger" || subToken == "warn" || subToken == "опасность")
        {
            aOut.Kind = TacticalChatCommand::Action::TriggerExplicitPing;
            aOut.PingKind = TacticalPingKind::Danger;
            aOut.CustomLabel = customNote;
            return true;
        }
        if (subToken == "enemy" || subToken == "attack" || subToken == "враг" || subToken == "атака")
        {
            aOut.Kind = TacticalChatCommand::Action::TriggerExplicitPing;
            aOut.PingKind = TacticalPingKind::Enemy;
            aOut.CustomLabel = customNote;
            return true;
        }
        if (subToken == "boss" || subToken == "босс")
        {
            aOut.Kind = TacticalChatCommand::Action::TriggerExplicitPing;
            aOut.PingKind = TacticalPingKind::EnemyBoss;
            aOut.CustomLabel = customNote;
            return true;
        }
        if (subToken == "loot" || subToken == "chest" || subToken == "лут" || subToken == "сундук")
        {
            aOut.Kind = TacticalChatCommand::Action::TriggerExplicitPing;
            aOut.PingKind = TacticalPingKind::Loot;
            aOut.CustomLabel = customNote;
            return true;
        }
        if (subToken == "obj" || subToken == "objective" || subToken == "puzzle" || subToken == "рычаг")
        {
            aOut.Kind = TacticalChatCommand::Action::TriggerExplicitPing;
            aOut.PingKind = TacticalPingKind::Objective;
            aOut.CustomLabel = customNote;
            return true;
        }
        if (subToken == "help" || subToken == "assist" || subToken == "revive" || subToken == "помощь")
        {
            aOut.Kind = TacticalChatCommand::Action::TriggerExplicitPing;
            aOut.PingKind = TacticalPingKind::AssistAlly;
            aOut.CustomLabel = customNote;
            return true;
        }
        if (subToken == "rally" || subToken == "here" || subToken == "сбор")
        {
            aOut.Kind = TacticalChatCommand::Action::TriggerExplicitPing;
            aOut.PingKind = TacticalPingKind::Rally;
            aOut.CustomLabel = customNote;
            return true;
        }

        // Custom text note on contextual ping: "/ping Осторожно ловушка!"
        aOut.Kind = TacticalChatCommand::Action::TriggerContextualPing;
        aOut.CustomLabel = argRest;
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Stage 5: World Containers, Locks, Shared Keys/Claws, Projectiles & Aggro Sync
// ---------------------------------------------------------------------------

/**
 * Excludes items that must NEVER be duplicated across party members via quest-item replication
 * (Gold 0xF, Lockpick 0xA, RFAB XP Books, potions/poisons/food, alchemy ingredients, ammo, and internal tokens).
 */
[[nodiscard]] inline bool IsExcludedFromQuestItemReplication(
    std::string_view aEditorId,
    uint8_t aFormType,
    uint32_t aFormId) noexcept
{
    const uint32_t baseId = aFormId & 0x00FFFFFFu;
    if (aFormId == 0xFu || aFormId == 0xAu || baseId == 0xFu || baseId == 0xAu)
        return true;

    // Ingredient (30), Ammo (42), Alchemy/Potion/Poison/Food (46), LeveledItem (53)
    if (aFormType == 30u || aFormType == 42u || aFormType == 46u || aFormType == 53u)
        return true;

    const std::string lower = Detail::ToLowerAscii(aEditorId);
    if (lower.empty())
        return false;

    if (lower.find("xpbook") != std::string::npos ||
        lower.find("rfab_book") != std::string::npos ||
        lower.find("rfab_xp") != std::string::npos ||
        lower.find("gold001") != std::string::npos ||
        lower.find("lockpick") != std::string::npos ||
        lower.find("soulgem") != std::string::npos ||
        lower.find("dummy") != std::string::npos ||
        lower.find("token") != std::string::npos)
    {
        return true;
    }

    return false;
}

/**
 * Identifies dungeon keys (FormType::Key == 45), Dragon Claws, Dwemer puzzle mechanisms,
 * and quest-critical dungeon progression artifacts that lack ExtraAliasInstanceArray in base containers
 * so that any co-op party member can unlock dungeon doors, solve claw doors, and activate pedestals.
 */
[[nodiscard]] inline bool IsSharedCoopKeyOrClaw(
    std::string_view aEditorId,
    uint8_t aFormType,
    uint32_t aFormId,
    bool aSyncPlayerHomes = false) noexcept
{
    if (IsExcludedFromQuestItemReplication(aEditorId, aFormType, aFormId))
        return false;

    const std::string lower = Detail::ToLowerAscii(aEditorId);

    // FormType::Key (45)
    if (aFormType == 45u)
    {
        if (!aSyncPlayerHomes && !lower.empty())
        {
            if (lower.find("breezehome") != std::string::npos ||
                lower.find("honeyside") != std::string::npos ||
                lower.find("proudspire") != std::string::npos ||
                lower.find("vindrel") != std::string::npos ||
                lower.find("hjerim") != std::string::npos ||
                lower.find("lakeview") != std::string::npos ||
                lower.find("windstad") != std::string::npos ||
                lower.find("heljarchen") != std::string::npos ||
                lower.find("playerhouse") != std::string::npos)
            {
                return false;
            }
        }
        return true;
    }

    if (lower.empty())
        return false;

    // Dragon Claws (Golden, Emerald, Sapphire, Ruby, Ebony, Diamond, Ivory, Coral, Glass, Iron)
    if (lower.find("dragonclaw") != std::string::npos)
        return true;

    if (lower.find("claw") != std::string::npos)
    {
        if (lower.find("dun") != std::string::npos ||
            lower.find("ms13") != std::string::npos ||
            lower.find("mq") != std::string::npos ||
            lower.find("dlc") != std::string::npos ||
            lower.find("golden") != std::string::npos ||
            lower.find("emerald") != std::string::npos ||
            lower.find("sapphire") != std::string::npos ||
            lower.find("ruby") != std::string::npos ||
            lower.find("ebony") != std::string::npos ||
            lower.find("diamond") != std::string::npos ||
            lower.find("ivory") != std::string::npos ||
            lower.find("coral") != std::string::npos ||
            lower.find("glass") != std::string::npos ||
            lower.find("iron") != std::string::npos)
        {
            return true;
        }
    }

    // Core dungeon & main-quest puzzle artifacts
    if (lower.find("dragonstone") != std::string::npos ||
        lower.find("lexicon") != std::string::npos ||
        lower.find("attunementsphere") != std::string::npos ||
        lower.find("dweattunement") != std::string::npos ||
        lower.find("dwemercube") != std::string::npos ||
        lower.find("controlcube") != std::string::npos ||
        lower.find("paragon") != std::string::npos ||
        lower.find("hornofjurgen") != std::string::npos ||
        lower.find("elderscroll") != std::string::npos ||
        lower.find("gauldur") != std::string::npos ||
        lower.find("da09gem") != std::string::npos ||
        lower.find("meridiabeacon") != std::string::npos)
    {
        return true;
    }

    return false;
}

struct CoopAggroDecisionInput
{
    bool IsHitteeValidNpc{false};
    bool IsHitteeRemote{false};
    bool IsHitteeDead{false};
    bool IsHitterPlayer{false};
    bool IsHitterDowned{false};
    bool HasActiveTargetLock{false};
    bool IsHitteeInCombat{false};
    bool HasValidCurrentTarget{false};
    bool IsCurrentTargetDeadOrDowned{false};
    float DistToCurrentTarget{0.0f};
    float DistToAttacker{0.0f};
};

/**
 * Pure decision policy for Co-Op Smart Threat / Aggro Switching:
 * Allows a melee Tank or active co-op attacker to pull aggro on local enemies/bosses
 * without causing target ping-ponging or retargeting onto downed players.
 */
[[nodiscard]] inline bool ShouldSwitchCoopCombatTarget(const CoopAggroDecisionInput& acIn) noexcept
{
    if (!acIn.IsHitteeValidNpc || acIn.IsHitteeRemote || acIn.IsHitteeDead)
        return false;

    if (!acIn.IsHitterPlayer || acIn.IsHitterDowned)
        return false;

    // Always acquire target immediately if the enemy is not yet in combat, has no valid target,
    // or its current target is dead or downed (even if a previous target lock timer has not expired).
    if (!acIn.IsHitteeInCombat || !acIn.HasValidCurrentTarget || acIn.IsCurrentTargetDeadOrDowned)
        return true;

    // Respect the anti-ping-pong target lock window when the current target is still alive & standing.
    if (acIn.HasActiveTargetLock)
        return false;

    if (!std::isfinite(acIn.DistToAttacker) || !std::isfinite(acIn.DistToCurrentTarget))
        return false;

    // Switch aggro onto the attacking player if they are significantly closer (e.g., Tank engaging in melee
    // while the previous target is kiting or casting from range).
    constexpr float kMeleeAggroPullMarginUnits = 220.0f;
    return (acIn.DistToAttacker + kMeleeAggroPullMarginUnits) < acIn.DistToCurrentTarget;
}

/**
 * Validates projectile launch parameters before passing to SkyrimSE.exe Havok Projectile::Launch.
 */
[[nodiscard]] inline bool AreProjectileLaunchParamsSafe(
    float aOriginX,
    float aOriginY,
    float aOriginZ,
    float aXAngle,
    float aYAngle,
    float aZAngle,
    float aPower,
    float aScale) noexcept
{
    if (!std::isfinite(aOriginX) || !std::isfinite(aOriginY) || !std::isfinite(aOriginZ) ||
        !std::isfinite(aXAngle) || !std::isfinite(aYAngle) || !std::isfinite(aZAngle) ||
        !std::isfinite(aPower) || !std::isfinite(aScale))
    {
        return false;
    }

    constexpr float kMaxWorldCoord = 2000000.0f;
    if (std::fabs(aOriginX) > kMaxWorldCoord ||
        std::fabs(aOriginY) > kMaxWorldCoord ||
        std::fabs(aOriginZ) > kMaxWorldCoord)
    {
        return false;
    }

    return aPower >= 0.0f && aScale >= 0.0f;
}

// ---------------------------------------------------------------------------
// Stage 6: World Systems, Teleportation, Calendar/Weather, Beast Forms & Reconnect Polish
// ---------------------------------------------------------------------------

/**
 * Losslessly encodes (hours, minutes) into Skyrim's GameHour float [0.0f, 24.0f).
 * Fixes the legacy * 0.17f / 10 bug where 23:59 produced 24.003f (>= 24.0f overflow).
 */
[[nodiscard]] inline float EncodeSkyrimGameHour(int aHours, int aMinutes) noexcept
{
    const int clampedHours = std::clamp(aHours, 0, 23);
    const int clampedMinutes = std::clamp(aMinutes, 0, 59);
    return static_cast<float>(clampedHours) + (static_cast<float>(clampedMinutes) / 60.0f);
}

/**
 * Losslessly decodes Skyrim's GameHour float into (hours [0..23], minutes [0..59]).
 */
inline void DecodeSkyrimGameHour(float aGameHour, int& aOutHours, int& aOutMinutes) noexcept
{
    if (!std::isfinite(aGameHour))
    {
        aOutHours = 8;
        aOutMinutes = 0;
        return;
    }

    float normalized = std::fmod(aGameHour, 24.0f);
    if (normalized < 0.0f)
        normalized += 24.0f;

    int hours = static_cast<int>(std::floor(normalized));
    int minutes = static_cast<int>(std::lround((normalized - static_cast<float>(hours)) * 60.0f));
    if (minutes >= 60)
    {
        minutes = 0;
        hours = (hours + 1) % 24;
    }

    aOutHours = std::clamp(hours, 0, 23);
    aOutMinutes = std::clamp(minutes, 0, 59);
}

/**
 * Cross-version detector for Werewolf, Vampire Lord, Werebear, and RFAB transformation races,
 * independent of Dawnguard.esm / Dragonborn.esm load-order index.
 */
[[nodiscard]] inline bool IsBeastFormRace(std::string_view aEditorId, uint32_t aFormId) noexcept
{
    const uint32_t baseId = aFormId & 0x00FFFFFFu;
    // 0x000CDD84 = WerewolfBeastRace (Skyrim.esm)
    // 0x0000283A = DLC1VampireBeastRace (Dawnguard.esm)
    // 0x00023FE9 = DLC2WerebearBeastRace (Dragonborn.esm)
    if (baseId == 0x000CDD84u || baseId == 0x0000283Au || baseId == 0x00023FE9u)
        return true;

    const std::string lower = Detail::ToLowerAscii(aEditorId);
    if (lower.empty())
        return false;

    return lower.find("vampirebeast") != std::string::npos ||
           lower.find("werewolfbeast") != std::string::npos ||
           lower.find("werebearbeast") != std::string::npos ||
           lower.find("lichrace") != std::string::npos;
}

/**
 * Validates whether a co-op teleport destination can be safely executed on the client.
 * Blocks teleportation while downed/loading, rejects NaN/Inf coordinates, and ensures
 * temporary 0xFFxxxxxx wilderness cells have a valid WorldSpaceId to load by grid coordinates.
 */
[[nodiscard]] inline bool CanResolveTeleportDestination(
    bool aHasCellId,
    bool aIsTempCellId,
    bool aHasWorldSpaceId,
    float aPosX,
    float aPosY,
    float aPosZ,
    bool aIsLocalPlayerDowned,
    bool aIsLoadingMenuOpen) noexcept
{
    if (aIsLocalPlayerDowned || aIsLoadingMenuOpen)
        return false;

    if (!std::isfinite(aPosX) || !std::isfinite(aPosY) || !std::isfinite(aPosZ))
        return false;

    constexpr float kMaxWorldCoord = 2000000.0f;
    if (std::fabs(aPosX) > kMaxWorldCoord ||
        std::fabs(aPosY) > kMaxWorldCoord ||
        std::fabs(aPosZ) > kMaxWorldCoord)
    {
        return false;
    }

    // If the cell is an unnamed 0xFFxxxxxx wilderness cell, we MUST have a valid WorldSpaceId
    // to resolve the cell via TESWorldSpace::LoadCell(gridX, gridY) rather than TESForm::GetById(0xFFxxxxxx).
    if (aIsTempCellId)
        return aHasWorldSpaceId;

    return aHasCellId || aHasWorldSpaceId;
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

inline void AppendCodePointUtf8(std::string& aOut, uint32_t aCp)
{
    if (aCp <= 0x7Fu)
    {
        aOut.push_back(static_cast<char>(aCp));
    }
    else if (aCp <= 0x7FFu)
    {
        aOut.push_back(static_cast<char>(0xC0u | ((aCp >> 6) & 0x1Fu)));
        aOut.push_back(static_cast<char>(0x80u | (aCp & 0x3Fu)));
    }
    else if (aCp <= 0xFFFFu)
    {
        aOut.push_back(static_cast<char>(0xE0u | ((aCp >> 12) & 0x0Fu)));
        aOut.push_back(static_cast<char>(0x80u | ((aCp >> 6) & 0x3Fu)));
        aOut.push_back(static_cast<char>(0x80u | (aCp & 0x3Fu)));
    }
    else if (aCp <= 0x10FFFFu)
    {
        aOut.push_back(static_cast<char>(0xF0u | ((aCp >> 18) & 0x07u)));
        aOut.push_back(static_cast<char>(0x80u | ((aCp >> 12) & 0x3Fu)));
        aOut.push_back(static_cast<char>(0x80u | ((aCp >> 6) & 0x3Fu)));
        aOut.push_back(static_cast<char>(0x80u | (aCp & 0x3Fu)));
    }
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

[[nodiscard]] inline std::string PathToUtf8Safe(const std::filesystem::path& acPath)
{
    const std::u8string u8 = acPath.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
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

    ec.clear();
    if (std::filesystem::exists(sksePlugins, ec) && std::filesystem::is_directory(sksePlugins, ec))
    {
        for (const auto& entry : std::filesystem::directory_iterator(sksePlugins, ec))
        {
            if (ec)
                break;
            const std::string lowerName = Detail::ToLowerAscii(Detail::PathToUtf8Safe(entry.path().filename()));
            if (lowerName.starts_with("customskills") ||
                lowerName.starts_with("rfab") ||
                lowerName.starts_with("[rfab]"))
            {
                return true;
            }
        }
    }

    // 2. CustomSkills v1 (`Data/NetScriptFramework/Plugins/customskill.*.config.txt`)
    const std::filesystem::path nsfPlugins = acDataDir / "NetScriptFramework" / "Plugins";
    ec.clear();
    if (std::filesystem::exists(nsfPlugins, ec) && std::filesystem::is_directory(nsfPlugins, ec))
    {
        for (const auto& entry : std::filesystem::directory_iterator(nsfPlugins, ec))
        {
            if (ec)
                break;
            const std::string lowerName = Detail::ToLowerAscii(Detail::PathToUtf8Safe(entry.path().filename()));
            if (lowerName.starts_with("customskill."))
                return true;
        }
    }

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

[[nodiscard]] inline std::filesystem::path ResolveRfabConfigPath(
    const std::filesystem::path& acGameRoot,
    std::string_view aPreferredRelPath,
    std::initializer_list<std::string_view> aCandidateFilenames) noexcept
{
    std::error_code ec;
    if (acGameRoot.empty())
        return {};

    if (!aPreferredRelPath.empty())
    {
        const std::filesystem::path directPath = acGameRoot / std::filesystem::path(aPreferredRelPath);
        if (std::filesystem::is_regular_file(directPath, ec) && !ec)
            return directPath;
    }

    std::vector<std::string> lowerCandidates;
    if (!aPreferredRelPath.empty())
    {
        const std::string preferredFilename = Detail::ToLowerAscii(
            Detail::PathToUtf8Safe(std::filesystem::path(aPreferredRelPath).filename()));
        if (!preferredFilename.empty())
            lowerCandidates.push_back(preferredFilename);
    }
    for (std::string_view cand : aCandidateFilenames)
    {
        const std::string lower = Detail::ToLowerAscii(Detail::Trim(cand));
        if (!lower.empty() && std::find(lowerCandidates.begin(), lowerCandidates.end(), lower) == lowerCandidates.end())
            lowerCandidates.push_back(lower);
    }
    if (lowerCandidates.empty())
        return {};

    const std::filesystem::path sksePluginsDir = acGameRoot / "Data" / "SKSE" / "Plugins";
    ec.clear();
    if (!std::filesystem::is_directory(sksePluginsDir, ec) || ec)
        return {};

    std::filesystem::recursive_directory_iterator it(
        sksePluginsDir,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::recursive_directory_iterator end;

    std::filesystem::path bestMatch{};
    int bestRank = 9999;

    while (!ec && it != end)
    {
        if (it.depth() > 4)
        {
            it.disable_recursion_pending();
        }

        std::error_code entryEc;
        if (it->is_regular_file(entryEc) && !entryEc)
        {
            const std::string fname = Detail::ToLowerAscii(Detail::PathToUtf8Safe(it->path().filename()));
            for (size_t idx = 0; idx < lowerCandidates.size(); ++idx)
            {
                if (fname == lowerCandidates[idx] && static_cast<int>(idx) < bestRank)
                {
                    bestRank = static_cast<int>(idx);
                    bestMatch = it->path();
                    if (bestRank == 0)
                        return bestMatch;
                }
            }
        }

        it.increment(ec);
    }

    return bestMatch;
}

struct EspFormRef
{
    std::string PluginName{};
    uint32_t LocalFormId{0};
    char RecordType[5]{};
    bool IsSelfLite{false};
};

class EspEdidIndex
{
public:
    void Clear() noexcept
    {
        m_edidToRef.clear();
        m_refToEdid.clear();
        m_indexedPluginsLower.clear();
    }

    [[nodiscard]] size_t Size() const noexcept { return m_edidToRef.size(); }
    [[nodiscard]] size_t IndexedPluginCount() const noexcept { return m_indexedPluginsLower.size(); }

    [[nodiscard]] const EspFormRef* LookupByEditorId(std::string_view aEditorId) const noexcept
    {
        if (aEditorId.empty())
            return nullptr;
        const std::string lower = Detail::ToLowerAscii(Detail::Trim(aEditorId));
        const auto it = m_edidToRef.find(lower);
        return it != m_edidToRef.end() ? &it->second : nullptr;
    }

    [[nodiscard]] std::string LookupEditorId(std::string_view aPluginName, uint32_t aLocalFormId) const
    {
        if (aPluginName.empty() || (aLocalFormId & 0x00FFFFFFu) == 0)
            return {};
        const std::string lowerPlugin = Detail::ToLowerAscii(Detail::Trim(aPluginName));
        const auto it = m_refToEdid.find(RfabLiveXpTable::FormatPluginFormKey(lowerPlugin, aLocalFormId));
        return it != m_refToEdid.end() ? it->second : std::string{};
    }

    bool IndexPluginBuffer(std::string_view aPluginFilename, const uint8_t* apData, size_t aSize)
    {
        if (aPluginFilename.empty() || !apData || aSize < 24)
            return false;

        if (std::memcmp(apData, "TES4", 4) != 0)
            return false;

        uint32_t tes4DataSize = 0;
        uint32_t tes4Flags = 0;
        std::memcpy(&tes4DataSize, apData + 4, sizeof(uint32_t));
        std::memcpy(&tes4Flags, apData + 8, sizeof(uint32_t));

        const size_t tes4End = 24ull + static_cast<size_t>(tes4DataSize);
        if (tes4End > aSize)
            return false;

        const bool isSelfLite = Detail::EndsWithIgnoreCase(aPluginFilename, ".esl") || ((tes4Flags & 0x00000200u) != 0);
        std::vector<std::string> masters;

        // Parse MAST subrecords inside TES4 header to resolve master-owned vs self-defined records accurately.
        {
            size_t subPos = 24;
            uint32_t extendedSubSize = 0;
            while (subPos + 6 <= tes4End)
            {
                char subType[4]{};
                uint16_t subSize16 = 0;
                std::memcpy(subType, apData + subPos, 4);
                std::memcpy(&subSize16, apData + subPos + 4, 2);
                subPos += 6;

                uint32_t subSize = extendedSubSize != 0 ? extendedSubSize : static_cast<uint32_t>(subSize16);
                extendedSubSize = 0;

                if (std::memcmp(subType, "XXXX", 4) == 0 && subSize == 4 && subPos + 4 <= tes4End)
                {
                    std::memcpy(&extendedSubSize, apData + subPos, 4);
                    subPos += 4;
                    continue;
                }

                if (subPos + subSize > tes4End)
                    break;

                if (std::memcmp(subType, "MAST", 4) == 0 && subSize > 0)
                {
                    size_t strLen = subSize;
                    while (strLen > 0 && apData[subPos + strLen - 1] == 0)
                        --strLen;
                    if (strLen > 0)
                    {
                        masters.emplace_back(reinterpret_cast<const char*>(apData + subPos), strLen);
                    }
                }

                subPos += subSize;
            }
        }

        m_indexedPluginsLower.insert(Detail::ToLowerAscii(aPluginFilename));

        // Walk top-level and nested GRUPs iteratively; skip heavy spatial/dialogue top-level groups (WRLD, CELL, DIAL, NAVM, LAND)
        // in O(1) so indexing an entire RFAB plugin suite takes a few milliseconds.
        size_t pos = tes4End;
        while (pos + 24 <= aSize)
        {
            char recType[5]{};
            std::memcpy(recType, apData + pos, 4);
            recType[4] = '\0';

            uint32_t fieldSize = 0;
            std::memcpy(&fieldSize, apData + pos + 4, sizeof(uint32_t));

            if (std::memcmp(recType, "GRUP", 4) == 0)
            {
                if (fieldSize < 24 || pos + fieldSize > aSize)
                    break;

                uint32_t groupType = 0;
                std::memcpy(&groupType, apData + pos + 12, sizeof(uint32_t));
                if (groupType == 0)
                {
                    const char* label = reinterpret_cast<const char*>(apData + pos + 8);
                    if (std::memcmp(label, "WRLD", 4) == 0 ||
                        std::memcmp(label, "CELL", 4) == 0 ||
                        std::memcmp(label, "DIAL", 4) == 0 ||
                        std::memcmp(label, "NAVM", 4) == 0 ||
                        std::memcmp(label, "LAND", 4) == 0)
                    {
                        pos += fieldSize;
                        continue;
                    }
                }

                // Step into the GRUP header (24 bytes); inner records end naturally inside `aSize`.
                pos += 24;
                continue;
            }

            const size_t recEnd = pos + 24ull + static_cast<size_t>(fieldSize);
            if (recEnd > aSize)
                break;

            uint32_t recFlags = 0;
            uint32_t rawFormId = 0;
            std::memcpy(&recFlags, apData + pos + 8, sizeof(uint32_t));
            std::memcpy(&rawFormId, apData + pos + 12, sizeof(uint32_t));

            // Uncompressed record (`0x00040000` == compressed): inspect subrecords for `EDID`.
            if ((recFlags & 0x00040000u) == 0 && rawFormId != 0 && fieldSize >= 6)
            {
                size_t subPos = pos + 24;
                uint32_t extendedSubSize = 0;
                while (subPos + 6 <= recEnd)
                {
                    char subType[4]{};
                    uint16_t subSize16 = 0;
                    std::memcpy(subType, apData + subPos, 4);
                    std::memcpy(&subSize16, apData + subPos + 4, 2);
                    subPos += 6;

                    uint32_t subSize = extendedSubSize != 0 ? extendedSubSize : static_cast<uint32_t>(subSize16);
                    extendedSubSize = 0;

                    if (std::memcmp(subType, "XXXX", 4) == 0 && subSize == 4 && subPos + 4 <= recEnd)
                    {
                        std::memcpy(&extendedSubSize, apData + subPos, 4);
                        subPos += 4;
                        continue;
                    }

                    if (subPos + subSize > recEnd)
                        break;

                    if (std::memcmp(subType, "EDID", 4) == 0)
                    {
                        size_t strLen = subSize;
                        while (strLen > 0 && apData[subPos + strLen - 1] == 0)
                            --strLen;
                        if (strLen > 0)
                        {
                            std::string edid(reinterpret_cast<const char*>(apData + subPos), strLen);
                            const uint32_t modIdx = (rawFormId >> 24) & 0xFFu;
                            const bool ownedByMaster = modIdx < masters.size();
                            const std::string& ownerPlugin = ownedByMaster ? masters[modIdx] : std::string(aPluginFilename);
                            const bool ownerIsSelfLite = !ownedByMaster && isSelfLite;
                            const uint32_t localId = ownerIsSelfLite
                                ? (rawFormId & 0x00000FFFu)
                                : (rawFormId & 0x00FFFFFFu);

                            if (localId != 0)
                            {
                                EspFormRef ref{};
                                ref.PluginName = ownerPlugin;
                                ref.LocalFormId = localId;
                                std::memcpy(ref.RecordType, recType, 5);
                                ref.IsSelfLite = ownerIsSelfLite;

                                const std::string lowerEdid = Detail::ToLowerAscii(edid);
                                const std::string lowerOwner = Detail::ToLowerAscii(ownerPlugin);
                                m_edidToRef[lowerEdid] = std::move(ref);
                                m_refToEdid[RfabLiveXpTable::FormatPluginFormKey(lowerOwner, localId)] = std::move(edid);
                            }
                        }
                        break; // EDID appears at most once per TES4 record
                    }

                    // Optimization: in TES4 format EDID is always the very first subrecord (or second after XXXX);
                    // stop scanning large subrecord bodies once non-EDID data starts.
                    break;
                }
            }

            pos = recEnd;
        }

        return true;
    }

    bool IndexPluginFile(const std::filesystem::path& acPluginPath)
    {
        std::error_code ec;
        if (acPluginPath.empty() || !std::filesystem::is_regular_file(acPluginPath, ec) || ec)
            return false;

        const std::string pluginFilename = Detail::PathToUtf8Safe(acPluginPath.filename());
        if (pluginFilename.empty())
            return false;

        std::ifstream file(acPluginPath, std::ios::binary);
        if (!file.is_open())
            return false;

        // Stream top-level TES4 + GRUP chunks rather than reading multi-hundred-MB spatial groups into RAM!
        char hdr[24]{};
        if (!file.read(hdr, 24) || std::memcmp(hdr, "TES4", 4) != 0)
            return false;

        uint32_t tes4DataSize = 0;
        std::memcpy(&tes4DataSize, hdr + 4, sizeof(uint32_t));
        if (tes4DataSize > (64u * 1024u * 1024u))
            return false;

        std::vector<uint8_t> compactBuf(24ull + tes4DataSize);
        std::memcpy(compactBuf.data(), hdr, 24);
        if (tes4DataSize > 0 && !file.read(reinterpret_cast<char*>(compactBuf.data() + 24), tes4DataSize))
            return false;

        while (file.read(hdr, 24))
        {
            uint32_t fieldSize = 0;
            std::memcpy(&fieldSize, hdr + 4, sizeof(uint32_t));
            if (std::memcmp(hdr, "GRUP", 4) == 0)
            {
                if (fieldSize < 24)
                    break;
                uint32_t groupType = 0;
                std::memcpy(&groupType, hdr + 12, sizeof(uint32_t));
                if (groupType == 0)
                {
                    const char* label = hdr + 8;
                    if (std::memcmp(label, "WRLD", 4) == 0 ||
                        std::memcmp(label, "CELL", 4) == 0 ||
                        std::memcmp(label, "DIAL", 4) == 0 ||
                        std::memcmp(label, "NAVM", 4) == 0 ||
                        std::memcmp(label, "LAND", 4) == 0)
                    {
                        file.seekg(static_cast<std::streamoff>(fieldSize - 24), std::ios::cur);
                        if (!file.good())
                            break;
                        continue;
                    }
                }

                const uint32_t payloadBytes = fieldSize - 24;
                if (payloadBytes > (128u * 1024u * 1024u))
                    break;
                const size_t oldSize = compactBuf.size();
                compactBuf.resize(oldSize + fieldSize);
                std::memcpy(compactBuf.data() + oldSize, hdr, 24);
                if (payloadBytes > 0 && !file.read(reinterpret_cast<char*>(compactBuf.data() + oldSize + 24), payloadBytes))
                {
                    compactBuf.resize(oldSize);
                    break;
                }
            }
            else
            {
                if (fieldSize > (64u * 1024u * 1024u))
                    break;
                const size_t oldSize = compactBuf.size();
                compactBuf.resize(oldSize + 24ull + fieldSize);
                std::memcpy(compactBuf.data() + oldSize, hdr, 24);
                if (fieldSize > 0 && !file.read(reinterpret_cast<char*>(compactBuf.data() + oldSize + 24), fieldSize))
                {
                    compactBuf.resize(oldSize);
                    break;
                }
            }
        }

        return IndexPluginBuffer(pluginFilename, compactBuf.data(), compactBuf.size());
    }

    size_t IndexRfabPluginsInDataDir(const std::filesystem::path& acDataDir, const RfabBindingsConfig& acBindings)
    {
        std::error_code ec;
        if (acDataDir.empty() || !std::filesystem::is_directory(acDataDir, ec) || ec)
            return 0;

        size_t indexed = 0;
        std::unordered_set<std::string> visitedLower;

        auto tryIndexByName = [&](const std::string& pluginName)
        {
            if (pluginName.empty())
                return;
            const std::string lower = Detail::ToLowerAscii(pluginName);
            if (!visitedLower.insert(lower).second)
                return;
            if (IndexPluginFile(acDataDir / pluginName))
                ++indexed;
        };

        // 1.Configured primary & secondary RFAB / Requiem plugin names
        tryIndexByName(acBindings.PluginName);
        for (const auto& sec : acBindings.SecondaryPluginNames)
            tryIndexByName(sec);

        // 2. Auto-discover any RFAB / Requiem / SkyrimTogether plugins in Data/ regardless of future version renames
        //    (e.g. `RFAB_Core.esp`, `RFAB_Perks.esp`, `RFAB_Bosses.esp`, `Requiem for the Indifferent.esp`, etc.)
        for (const auto& entry : std::filesystem::directory_iterator(
                 acDataDir, std::filesystem::directory_options::skip_permission_denied, ec))
        {
            if (ec)
                break;
            std::error_code fEc;
            if (!entry.is_regular_file(fEc) || fEc)
                continue;

            const std::string fname = Detail::PathToUtf8Safe(entry.path().filename());
            const std::string lower = Detail::ToLowerAscii(fname);
            if (!Detail::EndsWithIgnoreCase(lower, ".esp") &&
                !Detail::EndsWithIgnoreCase(lower, ".esm") &&
                !Detail::EndsWithIgnoreCase(lower, ".esl"))
            {
                continue;
            }

            if (lower.find("rfab") != std::string::npos ||
                lower.find("requiem") != std::string::npos ||
                lower.find("skyrimtogether") != std::string::npos ||
                lower.find("fomod") != std::string::npos)
            {
                if (visitedLower.insert(lower).second && IndexPluginFile(entry.path()))
                    ++indexed;
            }
        }

        return indexed;
    }

private:
    std::unordered_map<std::string, EspFormRef> m_edidToRef{};
    std::unordered_map<std::string, std::string> m_refToEdid{};
    std::unordered_set<std::string> m_indexedPluginsLower{};
};

} // namespace RfabCompat
