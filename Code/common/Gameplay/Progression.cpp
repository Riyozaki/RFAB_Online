#include <Gameplay/Progression.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace Gameplay::Progression
{
namespace
{
// ---------------------------------------------------------------------------
// Minimal JSON support for a *flat* object of string/number values.
//
// A full JSON library is not worth it here: the ledger only ever contains
// objects we wrote ourselves, and pulling a dependency into Code/common would
// make the host tests heavier than the feature. The parser is strict about
// syntax and tolerant about unknown keys.
// ---------------------------------------------------------------------------
bool IsSpace(char aChar) noexcept
{
    return aChar == ' ' || aChar == '\t' || aChar == '\r' || aChar == '\n';
}

void AppendEscaped(std::string& aOut, std::string_view acValue)
{
    aOut += '"';
    for (const char c : acValue)
    {
        switch (c)
        {
        case '"': aOut += "\\\""; break;
        case '\\': aOut += "\\\\"; break;
        case '\n': aOut += "\\n"; break;
        case '\r': aOut += "\\r"; break;
        case '\t': aOut += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                aOut += buffer;
            }
            else
            {
                aOut += c;
            }
            break;
        }
    }
    aOut += '"';
}

bool ParseString(std::string_view acText, size_t& aPos, std::string& aOut) noexcept
{
    if (aPos >= acText.size() || acText[aPos] != '"')
        return false;

    ++aPos;
    aOut.clear();

    while (aPos < acText.size())
    {
        const char c = acText[aPos++];

        if (c == '"')
            return true;

        if (c != '\\')
        {
            aOut += c;
            continue;
        }

        if (aPos >= acText.size())
            return false;

        const char escape = acText[aPos++];
        switch (escape)
        {
        case '"': aOut += '"'; break;
        case '\\': aOut += '\\'; break;
        case '/': aOut += '/'; break;
        case 'b': aOut += '\b'; break;
        case 'f': aOut += '\f'; break;
        case 'n': aOut += '\n'; break;
        case 'r': aOut += '\r'; break;
        case 't': aOut += '\t'; break;
        case 'u':
        {
            if (aPos + 4 > acText.size())
                return false;

            unsigned code = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char digit = acText[aPos++];
                code <<= 4;
                if (digit >= '0' && digit <= '9')
                    code |= static_cast<unsigned>(digit - '0');
                else if (digit >= 'a' && digit <= 'f')
                    code |= static_cast<unsigned>(digit - 'a' + 10);
                else if (digit >= 'A' && digit <= 'F')
                    code |= static_cast<unsigned>(digit - 'A' + 10);
                else
                    return false;
            }

            // Only ASCII escapes appear in values we write (control characters);
            // anything above 0x7F is encoded as UTF-8 by the writer.
            aOut += code <= 0x7F ? static_cast<char>(code) : '?';
            break;
        }
        default:
            return false;
        }
    }

    return false;
}

bool ParseNumber(std::string_view acText, size_t& aPos, double& aOut) noexcept
{
    const size_t start = aPos;

    if (aPos < acText.size() && (acText[aPos] == '-' || acText[aPos] == '+'))
        ++aPos;

    while (aPos < acText.size() && ((acText[aPos] >= '0' && acText[aPos] <= '9') || acText[aPos] == '.' || acText[aPos] == 'e' || acText[aPos] == 'E' ||
                                    acText[aPos] == '-' || acText[aPos] == '+'))
        ++aPos;

    if (aPos == start)
        return false;

    const std::string token(acText.substr(start, aPos - start));
    char* pEnd = nullptr;
    const double value = std::strtod(token.c_str(), &pEnd);

    if (pEnd == token.c_str() || *pEnd != '\0')
        return false;

    aOut = value;
    return true;
}

//! Keys the ledger stores as JSON numbers; every other key must be a string.
bool IsNumericKey(const std::string& acKey) noexcept
{
    static const char* kKeys[] = {"xp", "level", "updated", "cellBase", "cellMod", "wsBase", "wsMod", "x", "y", "z"};

    for (const char* pKey : kKeys)
    {
        if (acKey == pKey)
            return true;
    }

    return false;
}

//! Parses `{"key":value,...}` into a key -> raw string map (numbers as text).
bool ParseFlatObject(std::string_view acText, std::map<std::string, std::string>& aOut) noexcept
{
    size_t pos = 0;
    const auto skipSpace = [&]()
    {
        while (pos < acText.size() && IsSpace(acText[pos]))
            ++pos;
    };

    skipSpace();

    if (pos >= acText.size() || acText[pos] != '{')
        return false;
    ++pos;

    skipSpace();

    if (pos < acText.size() && acText[pos] == '}')
    {
        ++pos;
    }
    else
    {
        for (;;)
        {
            skipSpace();

            std::string key;
            if (!ParseString(acText, pos, key))
                return false;

            skipSpace();

            if (pos >= acText.size() || acText[pos] != ':')
                return false;
            ++pos;

            skipSpace();

            if (pos >= acText.size())
                return false;

            std::string value;
            bool isString = false;

            if (acText[pos] == '"')
            {
                if (!ParseString(acText, pos, value))
                    return false;
                isString = true;
            }
            else if (acText[pos] == 't' || acText[pos] == 'f')
            {
                const bool isTrue = acText[pos] == 't';
                const std::string_view literal = isTrue ? "true" : "false";
                if (acText.compare(pos, literal.size(), literal) != 0)
                    return false;
                pos += literal.size();
                value = isTrue ? "1" : "0";
            }
            else
            {
                // Nested objects/arrays are not part of this format.
                if (acText[pos] == '{' || acText[pos] == '[')
                    return false;

                double number = 0.0;
                if (!ParseNumber(acText, pos, number))
                    return false;

                char buffer[64]{};
                std::snprintf(buffer, sizeof(buffer), "%.17g", number);
                value = buffer;
            }

            // Known keys have a fixed type: "user" is a name, the rest are
            // numbers. This rejects a hand-edited or corrupted line instead of
            // silently turning a number into a username.
            if (IsNumericKey(key) && isString)
                return false;
            if (!IsNumericKey(key) && key == "user" && !isString)
                return false;

            aOut[key] = value;

            skipSpace();

            if (pos < acText.size() && acText[pos] == ',')
            {
                ++pos;
                skipSpace();

                // A trailing comma is malformed: a key must follow.
                if (pos >= acText.size() || acText[pos] == '}')
                    return false;

                continue;
            }

            if (pos < acText.size() && acText[pos] == '}')
            {
                ++pos;
                break;
            }

            return false;
        }
    }

    // Trailing garbage after the object is not ours.
    skipSpace();

    return pos == acText.size();
}

int64_t ReadInt(const std::map<std::string, std::string>& acValues, const char* apKey, int64_t aDefault) noexcept
{
    const auto it = acValues.find(apKey);
    if (it == acValues.end())
        return aDefault;

    char* pEnd = nullptr;
    const long long value = std::strtoll(it->second.c_str(), &pEnd, 10);

    if (pEnd == it->second.c_str() || *pEnd != '\0')
        return aDefault;

    return value;
}

float ReadFloat(const std::map<std::string, std::string>& acValues, const char* apKey, float aDefault) noexcept
{
    const auto it = acValues.find(apKey);
    if (it == acValues.end())
        return aDefault;

    char* pEnd = nullptr;
    const double value = std::strtod(it->second.c_str(), &pEnd);

    if (pEnd == it->second.c_str() || *pEnd != '\0' || !std::isfinite(value))
        return aDefault;

    return static_cast<float>(value);
}

//! Starts a key/value pair, adding the separating comma only when needed.
void AppendKey(std::string& aOut, bool& aFirst, const char* apKey)
{
    if (!aFirst)
        aOut += ',';

    aFirst = false;
    aOut += '"';
    aOut += apKey;
    aOut += "\":";
}

void AppendNumber(std::string& aOut, bool& aFirst, const char* apKey, int64_t aValue)
{
    AppendKey(aOut, aFirst, apKey);
    aOut += std::to_string(aValue);
}

void AppendFloat(std::string& aOut, bool& aFirst, const char* apKey, float aValue)
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(aValue));

    AppendKey(aOut, aFirst, apKey);
    aOut += buffer;
}

void AppendString(std::string& aOut, bool& aFirst, const char* apKey, std::string_view acValue)
{
    AppendKey(aOut, aFirst, apKey);
    AppendEscaped(aOut, acValue);
}
} // namespace

int32_t ComputeCatchUp(int32_t aServerTotal, int32_t aClientTotal) noexcept
{
    if (aServerTotal <= 0 || aClientTotal < 0)
        return 0;

    if (aClientTotal >= aServerTotal)
        return 0;

    const int32_t server = std::min(aServerTotal, kMaxTotalXp);
    const int32_t client = std::min(aClientTotal, kMaxTotalXp);

    const int64_t delta = static_cast<int64_t>(server) - static_cast<int64_t>(client);
    if (delta <= 0)
        return 0;

    return static_cast<int32_t>(delta);
}

std::string FormatRecord(const PlayerRecord& acRecord)
{
    std::string line = "{";
    bool first = true;

    AppendString(line, first, "user", acRecord.Username);
    AppendNumber(line, first, "xp", acRecord.RfabXp);
    AppendNumber(line, first, "level", acRecord.Level);
    AppendNumber(line, first, "updated", static_cast<int64_t>(acRecord.UpdatedUnixMs));
    AppendNumber(line, first, "cellBase", acRecord.CellBaseId);
    AppendNumber(line, first, "cellMod", acRecord.CellModId);
    AppendNumber(line, first, "wsBase", acRecord.WorldSpaceBaseId);
    AppendNumber(line, first, "wsMod", acRecord.WorldSpaceModId);
    AppendFloat(line, first, "x", acRecord.PositionX);
    AppendFloat(line, first, "y", acRecord.PositionY);
    AppendFloat(line, first, "z", acRecord.PositionZ);
    line += '}';

    return line;
}

bool ParseRecord(std::string_view acLine, PlayerRecord& aOut) noexcept
{
    std::map<std::string, std::string> values;
    if (!ParseFlatObject(acLine, values))
        return false;

    const auto userIt = values.find("user");
    if (userIt == values.end() || userIt->second.empty())
        return false;

    aOut = PlayerRecord{};
    aOut.Username = userIt->second;
    aOut.RfabXp = static_cast<int32_t>(std::clamp<int64_t>(ReadInt(values, "xp", 0), 0, kMaxTotalXp));
    aOut.Level = static_cast<uint16_t>(std::clamp<int64_t>(ReadInt(values, "level", 0), 0, 0xFFFF));
    aOut.UpdatedUnixMs = static_cast<uint64_t>(std::max<int64_t>(ReadInt(values, "updated", 0), 0));
    aOut.CellBaseId = static_cast<uint32_t>(std::clamp<int64_t>(ReadInt(values, "cellBase", 0), 0, 0xFFFFFFFFll));
    aOut.CellModId = static_cast<uint32_t>(std::clamp<int64_t>(ReadInt(values, "cellMod", 0), 0, 0xFFFFFFFFll));
    aOut.WorldSpaceBaseId = static_cast<uint32_t>(std::clamp<int64_t>(ReadInt(values, "wsBase", 0), 0, 0xFFFFFFFFll));
    aOut.WorldSpaceModId = static_cast<uint32_t>(std::clamp<int64_t>(ReadInt(values, "wsMod", 0), 0, 0xFFFFFFFFll));
    aOut.PositionX = ReadFloat(values, "x", 0.0f);
    aOut.PositionY = ReadFloat(values, "y", 0.0f);
    aOut.PositionZ = ReadFloat(values, "z", 0.0f);

    return aOut.IsValid();
}

PlayerRecord Merge(const PlayerRecord& acStored, const PlayerRecord& acCurrent) noexcept
{
    if (!acStored.IsValid())
        return acCurrent;

    if (!acCurrent.IsValid())
        return acStored;

    PlayerRecord merged = acCurrent;

    // Experience never goes backwards on the server side: a player who loads an
    // older save must not lose what everyone else already saw them earn.
    merged.RfabXp = std::max({acStored.RfabXp, acCurrent.RfabXp, 0});

    // The snapshot is "the last place we saw them", the newest observation wins.
    if (acStored.UpdatedUnixMs > acCurrent.UpdatedUnixMs)
    {
        merged.UpdatedUnixMs = acStored.UpdatedUnixMs;
        merged.CellBaseId = acStored.CellBaseId;
        merged.CellModId = acStored.CellModId;
        merged.WorldSpaceBaseId = acStored.WorldSpaceBaseId;
        merged.WorldSpaceModId = acStored.WorldSpaceModId;
        merged.PositionX = acStored.PositionX;
        merged.PositionY = acStored.PositionY;
        merged.PositionZ = acStored.PositionZ;
        merged.Level = acStored.Level;
    }

    return merged;
}

const PlayerRecord& Ledger::Upsert(const PlayerRecord& acRecord) noexcept
{
    const auto it = std::find_if(m_records.begin(), m_records.end(), [&](const PlayerRecord& record) { return record.Username == acRecord.Username; });

    if (it != m_records.end())
    {
        *it = Merge(*it, acRecord);
        return *it;
    }

    // A full ledger is a runaway (bots, or a very long lived server): drop the
    // oldest entry rather than growing without bound.
    if (m_records.size() >= kMaxPlayers)
        m_records.erase(m_records.begin());

    m_records.push_back(acRecord);
    return m_records.back();
}

const PlayerRecord* Ledger::Find(std::string_view acUsername) const noexcept
{
    const auto it = std::find_if(m_records.begin(), m_records.end(), [&](const PlayerRecord& record) { return record.Username == acUsername; });

    return it != m_records.end() ? &*it : nullptr;
}

uint64_t Ledger::TotalXp() const noexcept
{
    uint64_t total = 0;

    for (const auto& record : m_records)
    {
        if (record.RfabXp > 0)
            total += static_cast<uint64_t>(record.RfabXp);
    }

    return total;
}

std::string Ledger::Serialize() const
{
    std::string text;
    text.reserve(m_records.size() * 160);

    for (const auto& record : m_records)
    {
        text += FormatRecord(record);
        text += '\n';
    }

    return text;
}

size_t Ledger::Deserialize(std::string_view acText)
{
    m_records.clear();

    size_t pos = 0;
    while (pos < acText.size())
    {
        size_t end = acText.find('\n', pos);
        if (end == std::string_view::npos)
            end = acText.size();

        const std::string_view line = acText.substr(pos, end - pos);
        pos = end + 1;

        PlayerRecord record;
        if (ParseRecord(line, record))
            Upsert(record);
    }

    return m_records.size();
}

} // namespace Gameplay::Progression
