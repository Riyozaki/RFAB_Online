#include <Coop/StorageCatalog.h>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
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
std::string Canonical(const StableForm& form) { return *StorageKey("catalog", form); }
std::optional<float> Coordinate(std::string_view text)
{
    float value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        !std::isfinite(value) || std::abs(value) > 100000000.f) return {};
    return value == 0.f ? 0.f : value; // canonicalize negative zero
}
std::string Canonical(const StorageRule& rule)
{
    const auto& p = rule.Location;
    return Canonical(rule.Access) + "\t" + Canonical(rule.Backing) + "\t" + Canonical(p.Cell) + "\t" +
        (p.World ? Canonical(*p.World) : "-") + "\t" + std::to_string(std::bit_cast<uint32_t>(p.X)) + "\t" +
        std::to_string(std::bit_cast<uint32_t>(p.Y)) + "\t" + std::to_string(std::bit_cast<uint32_t>(p.Z)) + "\n";
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
        if (line.size() > 1024 || rules.size() >= 1024) return false;
        std::array<std::string_view, 15> fields;
        for (size_t i = 0; i < fields.size(); ++i)
        {
            const auto tab = line.find('\t');
            if ((i == fields.size() - 1) != (tab == line.npos)) return false;
            fields[i] = line.substr(0, tab);
            line = tab == line.npos ? std::string_view{} : line.substr(tab + 1);
        }
        auto access = Form(fields[0], fields[1], fields[2]), backing = Form(fields[3], fields[4], fields[5]);
        auto cell = Form(fields[6], fields[7], fields[8]);
        std::optional<StableForm> world;
        if (fields[9] != "-")
        {
            world = Form(fields[9], fields[10], fields[11]);
            if (!world) return false;
        }
        else if (fields[10] != "0" || fields[11] != "0") return false;
        const auto x = Coordinate(fields[12]), y = Coordinate(fields[13]), z = Coordinate(fields[14]);
        if (!access || !backing || *access == *backing || !cell || !x || !y || !z) return false;
        // A repeated access row (even an identical one) hides generation/merge mistakes.
        if (!rules.emplace(Canonical(*access), StorageRule{*access, *backing, {*cell, world, *x, *y, *z}}).second) return false;
    }
    if (rules.empty()) return false;
    uint64_t signature = 14695981039346656037ull;
    for (auto& [key, rule] : rules)
    {
        for (const unsigned char c : Canonical(rule)) { signature ^= c; signature *= 1099511628211ull; }
        m_rules.push_back(std::move(rule));
    }
    m_signature = signature ? signature : 1;
    return true;
}
const StorageRule* StorageCatalog::Find(const StableForm& form) const
{
    const auto key = StorageKey("catalog", form);
    if (!key) return nullptr;
    for (const auto& rule : m_rules) if (Canonical(rule.Access) == *key) return &rule;
    return nullptr;
}
const StableForm* StorageCatalog::Backing(const StableForm& form) const
{
    const auto* rule = Find(form);
    return rule ? &rule->Backing : nullptr;
}
bool StorageCatalog::IsBacking(const StableForm& form) const
{
    const auto key = StorageKey("catalog", form);
    if (!key) return false;
    for (const auto& rule : m_rules) if (Canonical(rule.Backing) == *key) return true;
    return false;
}
bool StorageCatalog::InReach(const StableForm& access, const StableForm& cell, const std::optional<StableForm>& world,
    float x, float y, float z, float radius) const
{
    const auto* rule = Find(access);
    const auto c = StorageKey("catalog", cell);
    // Exterior persistent refs can live in the world's persistent CELL rather than the
    // player's current grid CELL. World identity + catalog-space distance is the boundary there.
    if (!rule || !c || (!world && *c != Canonical(rule->Location.Cell)) || world.has_value() != rule->Location.World.has_value() ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(radius) || radius <= 0.f || radius > 320.f) return false;
    if (world)
    {
        const auto w = StorageKey("catalog", *world);
        if (!w || *w != Canonical(*rule->Location.World)) return false;
    }
    const double dx = double(x) - rule->Location.X, dy = double(y) - rule->Location.Y, dz = double(z) - rule->Location.Z;
    return dx * dx + dy * dy + dz * dz <= double(radius) * radius;
}
}
