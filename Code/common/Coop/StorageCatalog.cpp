#include <Coop/StorageCatalog.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <map>

namespace Coop
{
namespace
{
std::optional<StableForm> Form(std::string_view plugin, std::string_view local, std::string_view light)
{
    if (light != "0" && light != "1") return {};
    const auto name = NormalizePlugin(plugin);
    if (!name || plugin.find_first_of("\t\r\n") != std::string_view::npos) return {};
    if (local.starts_with("0x") || local.starts_with("0X")) local.remove_prefix(2);
    uint32_t id{};
    const auto result = std::from_chars(local.data(), local.data() + local.size(), id, 16);
    StableForm form{*name, id, light == "1"};
    if (local.empty() || local.size() > 6 || result.ec != std::errc{} || result.ptr != local.data() + local.size() ||
        !StorageKey("catalog", form)) return {};
    return form;
}
std::string Canonical(const StableForm& form)
{
    return *StorageKey("catalog", form);
}
}
bool StorageCatalog::Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::string text;
    char bytes[4096];
    while (file.read(bytes, sizeof(bytes)) || file.gcount())
    {
        if (text.size() + file.gcount() > 1024 * 1024) return Parse({});
        text.append(bytes, static_cast<size_t>(file.gcount()));
    }
    if (!file.eof()) return Parse({});
    return Parse(text);
}
bool StorageCatalog::Parse(std::string_view text)
{
    m_rules.clear(); m_signature = 0;
    if (text.size() > 1024 * 1024) return false;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::map<std::string, StorageRule> rules;
    while (!text.empty())
    {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() || line.starts_with('#')) continue;
        if (line.size() > 512 || rules.size() >= 1024) return false;
        std::array<std::string_view, 6> fields;
        for (size_t i = 0; i < fields.size(); ++i)
        {
            const auto tab = line.find('\t');
            if ((i == 5) != (tab == line.npos)) return false;
            fields[i] = line.substr(0, tab);
            line = tab == line.npos ? std::string_view{} : line.substr(tab + 1);
        }
        auto access = Form(fields[0], fields[1], fields[2]), backing = Form(fields[3], fields[4], fields[5]);
        if (!access || !backing || *access == *backing) return false;
        // A repeated access row (even an identical one) hides generation/merge mistakes.
        if (!rules.emplace(Canonical(*access), StorageRule{*access, *backing}).second) return false;
    }
    if (rules.empty()) return false;
    uint64_t signature = 14695981039346656037ull;
    for (auto& [key, rule] : rules)
    {
        for (const unsigned char c : key + "\t" + Canonical(rule.Backing) + "\n")
        { signature ^= c; signature *= 1099511628211ull; }
        m_rules.push_back(std::move(rule));
    }
    m_signature = signature ? signature : 1;
    return true;
}
const StableForm* StorageCatalog::Backing(const StableForm& form) const
{
    const auto key = StorageKey("catalog", form);
    if (!key) return nullptr;
    for (const auto& rule : m_rules) if (Canonical(rule.Access) == *key) return &rule.Backing;
    return nullptr;
}
bool StorageCatalog::IsBacking(const StableForm& form) const
{
    const auto key = StorageKey("catalog", form);
    if (!key) return false;
    for (const auto& rule : m_rules) if (Canonical(rule.Backing) == *key) return true;
    return false;
}
}
