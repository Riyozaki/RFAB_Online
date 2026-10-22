#include <Gameplay/SaveSession.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace
{
std::string_view Trim(std::string_view acValue) noexcept
{
    size_t start = 0;
    while (start < acValue.size() && std::isspace(static_cast<unsigned char>(acValue[start])))
        ++start;

    size_t end = acValue.size();
    while (end > start && std::isspace(static_cast<unsigned char>(acValue[end - 1])))
        --end;

    return acValue.substr(start, end - start);
}

std::string ToLower(std::string_view acValue)
{
    std::string result(acValue);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool ParseBool(std::string_view acValue, bool aDefault) noexcept
{
    const std::string value = ToLower(Trim(acValue));

    if (value == "1" || value == "true" || value == "yes" || value == "on")
        return true;

    if (value == "0" || value == "false" || value == "no" || value == "off")
        return false;

    return aDefault;
}

uint32_t ParseUint(std::string_view acValue, uint32_t aDefault) noexcept
{
    const auto trimmed = Trim(acValue);
    if (trimmed.empty())
        return aDefault;

    std::string buffer(trimmed);
    char* pEnd = nullptr;
    const unsigned long long parsed = std::strtoull(buffer.c_str(), &pEnd, 10);
    if (pEnd == nullptr || *pEnd != '\0')
        return aDefault;

    return static_cast<uint32_t>(parsed);
}
} // namespace

namespace Gameplay::SaveSession
{
Policy ParsePolicy(std::string_view acText, Policy aDefaults) noexcept
{
    Policy policy = aDefaults;

    bool inSaveSection = true; // keys before any section header belong to [Save]
    size_t offset = 0;

    while (offset <= acText.size())
    {
        const size_t lineEnd = acText.find('\n', offset);
        const size_t end = (lineEnd == std::string_view::npos) ? acText.size() : lineEnd;
        std::string_view line = acText.substr(offset, end - offset);
        offset = (lineEnd == std::string_view::npos) ? acText.size() + 1 : lineEnd + 1;

        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        line = Trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#')
            continue;

        if (line.front() == '[')
        {
            const auto close = line.find(']');
            const std::string name = ToLower(Trim(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1)));
            inSaveSection = (name == "save" || name == "saves" || name == "savegame");
            continue;
        }

        if (!inSaveSection)
            continue;

        const auto equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;

        const std::string key = ToLower(Trim(line.substr(0, equals)));
        const auto value = line.substr(equals + 1);

        if (key == "bstopsessiononload")
            policy.StopSessionOnLoad = ParseBool(value, policy.StopSessionOnLoad);
        else if (key == "bindexsaves")
            policy.IndexSaves = ParseBool(value, policy.IndexSaves);
        else if (key == "bnotifyingame")
            policy.NotifyInGame = ParseBool(value, policy.NotifyInGame);
        else if (key == "upollintervalms")
            policy.PollIntervalMs = ParseUint(value, policy.PollIntervalMs);
    }

    policy.PollIntervalMs = std::clamp(policy.PollIntervalMs, kMinPollIntervalMs, kMaxPollIntervalMs);

    return policy;
}

std::string EscapeJson(std::string_view acValue)
{
    std::string out;
    out.reserve(acValue.size() + 8);

    for (const char c : acValue)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out += kHex[static_cast<unsigned char>(c) & 0xF];
            }
            else
            {
                out += c;
            }
            break;
        }
    }

    return out;
}

std::string FormatSaveIndexLine(const SaveIndexEntry& acEntry)
{
    std::ostringstream stream;
    stream << "{\"file\":\"" << EscapeJson(acEntry.File) << "\"";
    stream << ",\"createdUtc\":\"" << EscapeJson(acEntry.CreatedUtc) << "\"";
    stream << ",\"server\":\"" << EscapeJson(acEntry.Server) << "\"";
    stream << ",\"character\":\"" << EscapeJson(acEntry.Character) << "\"";
    stream << ",\"sessionId\":" << acEntry.SessionId;
    stream << ",\"sizeBytes\":" << acEntry.SizeBytes;
    stream << ",\"inSession\":" << (acEntry.InSession ? "true" : "false");
    stream << "}";

    return stream.str();
}

} // namespace Gameplay::SaveSession

namespace Gameplay::SaveSession
{
void SettleTracker::Prime(std::string_view acKey, FileStamp aStamp) noexcept
{
    m_entries[std::string(acKey)] = Entry{aStamp, 0, true};
}

bool SettleTracker::Observe(std::string_view acKey, FileStamp aStamp, int64_t aNowMs) noexcept
{
    const std::string key(acKey);
    const auto it = m_entries.find(key);

    if (it == m_entries.end())
    {
        m_entries.emplace(key, Entry{aStamp, aNowMs, false});
        return false;
    }

    Entry& entry = it->second;

    if (entry.Stamp != aStamp)
    {
        // Still being written (or rewritten): restart the settle window.
        entry.Stamp = aStamp;
        entry.StableSinceMs = aNowMs;
        entry.Reported = false;
        return false;
    }

    if (entry.Reported)
        return false;

    if (aNowMs - entry.StableSinceMs < kSettleMs)
        return false;

    entry.Reported = true;
    return true;
}

void SettleTracker::Forget(std::string_view acKey) noexcept
{
    m_entries.erase(std::string(acKey));
}

std::vector<std::string> SettleTracker::TrackedKeys() const
{
    std::vector<std::string> keys;
    keys.reserve(m_entries.size());
    for (const auto& [key, entry] : m_entries)
        keys.push_back(key);
    return keys;
}

size_t SettleTracker::TrackedCount() const noexcept
{
    return m_entries.size();
}

} // namespace Gameplay::SaveSession
