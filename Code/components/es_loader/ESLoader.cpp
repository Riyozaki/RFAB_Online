

#include "ESLoader.h"
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

        // If user copied plugins.txt instead of loadorder.txt, ignore disabled lines and strip leading '*'
        if (line[0] == '*')
            line.erase(line.begin());

        if (line.empty())
            continue;

        PluginData plugin;
        plugin.m_filename = line;

        const char extensionType = static_cast<char>(std::tolower(static_cast<unsigned char>(line.back())));

        switch (extensionType)
        {
        case 'm':
            if (standardId < 0xFE)
                m_masterFiles[line] = static_cast<uint8_t>(standardId);
            [[fallthrough]];
        case 'p':
            if (standardId < 0xFE)
            {
                plugin.m_standardId = static_cast<uint8_t>(standardId);
                ++standardId;
                plugin.m_isLite = false;
                m_loadOrder.push_back(plugin);
            }
            else if (liteId < 0x0FFF)
            {
                // ESL-flagged .esp/.esm in large MO2 modpacks (like drake1ch/RFAB with 500+ plugins):
                // assign into FE light-plugin space instead of wrapping uint8_t back to 0x00 (Skyrim.esm)!
                plugin.m_liteId = liteId++;
                plugin.m_isLite = true;
                m_loadOrder.push_back(plugin);
            }
            break;
        case 'l':
            if (liteId < 0x0FFF)
            {
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
