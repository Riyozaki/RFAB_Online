
#include <Components.h>
#include <es_loader/ESLoader.h>
#include <Coop/InventoryLedger.h>

uint32_t ModsComponent::AddStandard(const String& acpFilename) noexcept
{
    const auto normalized = Coop::NormalizePlugin(std::string_view(acpFilename.data(), acpFilename.size()));
    if (!normalized) return 0xFFFFFFFEu;
    const String key(normalized->data(), normalized->size());
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
    const auto normalized = Coop::NormalizePlugin(std::string_view(acpFilename.data(), acpFilename.size()));
    if (!normalized) return 0xFFFFFFFEu;
    const String key(normalized->data(), normalized->size());
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
    m_serverMods.emplace(acData.m_filename, Entry{acData.m_liteId, 1});
}

bool ModsComponent::IsInstalled(const String& acpFilename) const noexcept
{
    auto it = std::find_if(m_serverMods.begin(), m_serverMods.end(), [&](const TModList::value_type& aEntry) { return aEntry.first == acpFilename; });

    return it != m_serverMods.end();
}
