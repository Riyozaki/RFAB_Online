
#include <Components.h>
#include <es_loader/ESLoader.h>

namespace
{
String NormalizeModFilenameKey(const String& acpFilename)
{
    String normalized = acpFilename;
    for (char& ch : normalized)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return normalized;
}
} // namespace

uint32_t ModsComponent::AddStandard(const String& acpFilename) noexcept
{
    const String key = NormalizeModFilenameKey(acpFilename);
    const auto itor = m_standardMods.find(key);
    if (itor != std::end(m_standardMods))
    {
        itor.value().refCount++;
        return itor->second.id;
    }

    const auto id = m_seed++;
    m_standardMods.emplace(key, Entry{id, 1});

    return id;
}

uint32_t ModsComponent::AddLite(const String& acpFilename) noexcept
{
    const String key = NormalizeModFilenameKey(acpFilename);
    const auto itor = m_liteMods.find(key);
    if (itor != std::end(m_liteMods))
    {
        itor.value().refCount++;
        return itor->second.id;
    }

    const auto id = m_seed++;
    m_liteMods.emplace(key, Entry{id, 1});

    return id;
}

void ModsComponent::AddServerMod(const ESLoader::PluginData& acData)
{
    // kind of a hack since we want to store both, so we take the two byte value
    m_serverMods.emplace(NormalizeModFilenameKey(acData.m_filename), Entry{acData.m_liteId, 1});
}

bool ModsComponent::IsInstalled(const String& acpFilename) const noexcept
{
    const String key = NormalizeModFilenameKey(acpFilename);
    return m_serverMods.find(key) != m_serverMods.end();
}
