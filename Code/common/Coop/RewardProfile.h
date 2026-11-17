#pragma once
#include <Coop/RewardBridge.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Coop
{
struct RewardProfileFile
{
    std::string Path;
    std::array<uint8_t, 32> SHA256{};
};
// Only exact compiled installed overrides + explicit native DLL fingerprints. No arbitrary
// paths, network inputs, saved-wallet values or guessed compatibility from a plugin filename.
struct RewardProfile
{
    std::string Name;
    uint64_t Signature{};
    uint32_t Kinds{};
    bool Bizarre{};
    std::vector<RewardProfileFile> Files;
    bool Matches(const Rewards::Adapter& adapter) const noexcept
    {
        const auto end = std::find(std::begin(adapter.Name), std::end(adapter.Name), '\0');
        return end != std::end(adapter.Name) && adapter.Version == Rewards::kVersion && adapter.Size == sizeof(adapter) &&
            adapter.Receiver && !adapter.ApplyReward && adapter.Signature == Signature && adapter.Kinds == Kinds &&
            adapter.Flags == (Rewards::kRequiredFlags | (Bizarre ? Rewards::Bizarre : 0u)) &&
            std::string_view(adapter.Name, static_cast<size_t>(end - std::begin(adapter.Name))) == Name;
    }
    static std::optional<RewardProfile> Parse(std::string_view text)
    {
        if (text.size() > 65536 || !text.starts_with("RFAB_REWARDS_V1\t")) return {};
        const auto line = [](std::string_view& remaining) {
            const auto end = remaining.find('\n');
            auto result = remaining.substr(0, end);
            remaining = end == remaining.npos ? std::string_view{} : remaining.substr(end + 1);
            if (result.ends_with('\r')) result.remove_suffix(1);
            return result;
        };
        const auto safeName = [](std::string_view name, size_t max) {
            if (name.empty() || name.size() > max || name.front() == '.') return false;
            for (unsigned char c : name)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
            return true;
        };
        auto header = line(text);
        std::array<std::string_view, 6> fields;
        for (size_t i = 0; i < fields.size(); ++i)
        {
            const auto tab = header.find('\t');
            if ((i == fields.size() - 1) != (tab == header.npos)) return {};
            fields[i] = header.substr(0, tab);
            header = tab == header.npos ? std::string_view{} : header.substr(tab + 1);
        }
        RewardProfile out;
        if (!safeName(fields[1], 63) || fields[2].size() != 16 || fields[5] != "papyrus-float-v1" ||
            (fields[4] != "0" && fields[4] != "1")) return {};
        const auto signature = std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), out.Signature, 16);
        const auto kinds = std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), out.Kinds);
        if (signature.ec != std::errc{} || signature.ptr != fields[2].data() + fields[2].size() || !out.Signature ||
            kinds.ec != std::errc{} || kinds.ptr != fields[3].data() + fields[3].size() || !out.Kinds || (out.Kinds & ~15u)) return {};
        out.Name = fields[1]; out.Bizarre = fields[4] == "1";
        std::set<std::string> paths;
        bool api{}, receiverScript{};
        while (!text.empty())
        {
            auto entry = line(text);
            const auto tab = entry.find('\t');
            if (entry.size() > 256 || tab != 64 || out.Files.size() >= 128 || entry.find('\t', tab + 1) != entry.npos) return {};
            RewardProfileFile file;
            for (size_t i = 0; i < file.SHA256.size(); ++i)
            {
                unsigned value{};
                const auto pair = entry.substr(i * 2, 2);
                const auto parsed = std::from_chars(pair.data(), pair.data() + pair.size(), value, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != pair.data() + pair.size()) return {};
                file.SHA256[i] = static_cast<uint8_t>(value);
            }
            const auto path = entry.substr(tab + 1);
            std::string_view name;
            if (path.starts_with("scripts/") && path.ends_with(".pex")) name = path.substr(8, path.size() - 12);
            else if (path.starts_with("SKSE/Plugins/") && path.ends_with(".dll")) name = path.substr(13, path.size() - 17);
            else return {};
            if (!safeName(name, 96) || name.find("..") != name.npos) return {};
            file.Path = path;
            auto normalized = file.Path;
            for (auto& c : normalized) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (!paths.insert(normalized).second) return {};
            api |= normalized == "scripts/rfabcooprewards.pex";
            receiverScript |= normalized.starts_with("scripts/") && normalized != "scripts/rfabcooprewards.pex";
            out.Files.push_back(std::move(file));
        }
        if (!api || !receiverScript) return {};
        return out;
    }
};
}
