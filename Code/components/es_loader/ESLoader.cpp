

#include "ESLoader.h"
#include <cstring>
#include <filesystem>
#include <fstream>

#include <Records/CLMT.h>
#include <Records/NPC.h>
#include <Records/REFR.h>

namespace ESLoader
{
String ReadZString(Buffer::Reader& aReader) noexcept
{
    String zstring = String(reinterpret_cast<const char*>(aReader.GetDataAtPosition()));
    aReader.Advance(zstring.size() + 1);
    return zstring;
}

String ReadWString(Buffer::Reader& aReader) noexcept
{
    uint16_t stringLength = 0;
    aReader.ReadBytes(reinterpret_cast<uint8_t*>(&stringLength), 2);
    String wstring = String(reinterpret_cast<const char*>(aReader.GetDataAtPosition()), stringLength);
    aReader.Advance(stringLength);
    return wstring;
}

ESLoader::ESLoader()
{
    m_directory = fs::current_path() / "Data"; //< Keep upper case to match Skyrim's file system
}

UniquePtr<RecordCollection> ESLoader::BuildRecordCollection() noexcept
{
    if (!fs::is_directory(m_directory))
    {
        // spdlog::warn("Data directory not found.");
        return nullptr;
    }

    if (!LoadLoadOrder())
    {
        return nullptr;
    }

    return MakeUnique<RecordCollection>();

    /*
    auto recordCollection = LoadFiles();
    recordCollection->BuildReferences();

    return std::move(recordCollection);
    */
}

bool ESLoader::LoadLoadOrder()
{
    std::ifstream loadOrderFile;
    auto loadOrderPath = m_directory / "loadorder.txt";
    loadOrderFile.open(loadOrderPath.c_str());
    if (loadOrderFile.fail())
    {
        spdlog::warn("Failed to open loadorder.txt");
        return false;
    }

    uint32_t standardId = 0x0;
    uint16_t liteId = 0x0;
    bool firstLine = true;

    struct ParsedLine
    {
        String Filename;
        bool HasStar{false};
    };

    auto toLowerCopy = [](const String& s) -> String
    {
        String out = s;
        for (char& ch : out)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return out;
    };

    auto isImplicitBaseMaster = [&](const String& s) noexcept -> bool
    {
        const String lower = toLowerCopy(s);
        return lower == "skyrim.esm" ||
               lower == "update.esm" ||
               lower == "dawnguard.esm" ||
               lower == "hearthfires.esm" ||
               lower == "dragonborn.esm";
    };

    Vector<ParsedLine> parsedLines;
    bool anyStarLine = false;

    String line;
    while (std::getline(loadOrderFile, line))
    {
        // Strip UTF-8 BOM on line 1 if present (MO2 / Notepad saves)
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

        // Strip CR (\r\n) and surrounding whitespace BEFORE copying into PluginData!
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.erase(line.begin());
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();

        if (line.empty() || line[0] == '#')
            continue;

        bool hasStar = false;
        if (line[0] == '*')
        {
            hasStar = true;
            anyStarLine = true;
            line.erase(line.begin());
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.erase(line.begin());
        }

        if (line.empty())
            continue;

        parsedLines.push_back(ParsedLine{line, hasStar});
    }

    Set<String> seenLowerPlugins;
    for (const auto& entry : parsedLines)
    {
        // If user copied SSE plugins.txt (which marks active mods with '*'), ignore disabled lines without '*'
        // except the 5 implicit vanilla SSE base masters.
        if (anyStarLine && !entry.HasStar && !isImplicitBaseMaster(entry.Filename))
            continue;

        const String lowerName = toLowerCopy(entry.Filename);
        if (!seenLowerPlugins.insert(lowerName).second)
            continue;

        line = entry.Filename;
        PluginData plugin;
        plugin.m_filename = line;

        const char extensionType = static_cast<char>(std::tolower(static_cast<unsigned char>(line.back())));

        // Peek TES4 record header flags (bytes 8..11: 0x001 = ESM master, 0x200 = ESL light plugin) when the plugin
        // file exists in Data/, matching Skyrim SE 1.5.97 TESDataHandler & MO2 for ESL-flagged .esp files
        // (e.g. SkyrimTogetherQuestPatches.esp, MCMHelper.esp, QuickLootIE.esp) and ESM-flagged RFAB.esp!
        bool hasTes4Flags = false;
        uint32_t tes4Flags = 0;
        {
            std::error_code ec;
            const fs::path candidatePath = m_directory / line.c_str();
            if (fs::is_regular_file(candidatePath, ec))
            {
                std::ifstream hdrStream(candidatePath, std::ios::binary);
                char rawHdr[12]{};
                if (hdrStream.read(rawHdr, sizeof(rawHdr)) && std::memcmp(rawHdr, "TES4", 4) == 0)
                {
                    std::memcpy(&tes4Flags, rawHdr + 8, sizeof(uint32_t));
                    hasTes4Flags = true;
                }
            }
        }

        const bool isEslFlagged = (extensionType == 'l') || (hasTes4Flags && (tes4Flags & 0x00000200u) != 0);
        const bool isEsmFlagged = (extensionType == 'm') || (hasTes4Flags && (tes4Flags & 0x00000001u) != 0);

        if (isEslFlagged)
        {
            if (liteId < 0x0FFF)
            {
                plugin.m_liteId = liteId++;
                plugin.m_isLite = true;
                m_loadOrder.push_back(plugin);
            }
            continue;
        }

        switch (extensionType)
        {
        case 'm':
        case 'p':
            if (standardId < 0xFE)
            {
                if (isEsmFlagged)
                    m_masterFiles[line] = static_cast<uint8_t>(standardId);
                plugin.m_standardId = static_cast<uint8_t>(standardId);
                ++standardId;
                plugin.m_isLite = false;
                m_loadOrder.push_back(plugin);
            }
            else if (liteId < 0x0FFF)
            {
                // ESL-flagged .esp/.esm in large MO2 modpacks (when Data/ files are absent on headless server):
                // assign into FE light-plugin space instead of wrapping uint8_t back to 0x00 (Skyrim.esm)!
                plugin.m_liteId = liteId++;
                plugin.m_isLite = true;
                m_loadOrder.push_back(plugin);
            }
            break;
        default:
            spdlog::warn("Extension in loadorder.txt not recognized: {}", line);
            break;
        }
    }

    return true;
}

UniquePtr<RecordCollection> ESLoader::LoadFiles()
{
    auto recordCollection = MakeUnique<RecordCollection>();

    for (PluginData& plugin : m_loadOrder)
    {
        fs::path pluginPath = GetPath(plugin.m_filename);
        if (pluginPath.empty())
        {
            spdlog::warn("Path to plugin file not found: {}", plugin.m_filename);
            continue;
        }

        TESFile pluginFile(m_masterFiles);
        if (plugin.IsLite())
            pluginFile.Setup(plugin.m_liteId);
        else
            pluginFile.Setup(plugin.m_standardId);

        bool loadResult = pluginFile.LoadFile(pluginPath);

        if (!loadResult)
            continue;

        pluginFile.IndexRecords(*recordCollection);
    }

    return recordCollection;
}

fs::path ESLoader::GetPath(String& aFilename)
{
    for (const auto& entry : fs::directory_iterator(m_directory))
    {
        String filename = entry.path().filename().string().c_str();
        if (filename == aFilename)
            return entry.path();
    }

    return fs::path();
}

} // namespace ESLoader
