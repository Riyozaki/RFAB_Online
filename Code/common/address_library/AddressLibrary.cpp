#include "address_library/AddressLibrary.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

namespace AddressLibrary
{
namespace
{
constexpr std::size_t kMaxModuleNameLength = 64 * 1024;
constexpr std::int32_t kMinPointerSize = 4;
constexpr std::int32_t kMaxPointerSize = 8;

// Bounds checked little-endian reader. Every read either succeeds or sets the
// failed flag; parsing then aborts with a descriptive error instead of reading
// past the buffer (the old implementation happily consumed garbage on
// truncated files).
class Reader
{
public:
    Reader(const std::uint8_t* apData, std::size_t aSize) noexcept
        : m_pData(apData)
        , m_size(aSize)
    {
    }

    template <class T> bool Read(T& aValue) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!CanRead(sizeof(T)))
            return false;

        std::memcpy(&aValue, m_pData + m_position, sizeof(T));
        m_position += sizeof(T);
        return true;
    }

    bool Skip(std::size_t aCount) noexcept
    {
        if (!CanRead(aCount))
            return false;

        m_position += aCount;
        return true;
    }

    bool CanRead(std::size_t aCount) const noexcept { return aCount <= m_size - m_position; }
    std::size_t GetPosition() const noexcept { return m_position; }
    std::size_t GetRemaining() const noexcept { return m_size - m_position; }

private:
    const std::uint8_t* m_pData{nullptr};
    std::size_t m_size{0};
    std::size_t m_position{0};
};

std::string MakeError(std::string_view aWhat, std::size_t aPosition)
{
    std::ostringstream stream;
    stream << aWhat << " (at byte " << aPosition << ')';
    return stream.str();
}

bool IsPrintableName(const std::string& acName) noexcept
{
    return std::all_of(acName.begin(), acName.end(), [](unsigned char aChar) { return aChar >= 0x20 && aChar < 0x7F; });
}

struct VersionFileNames
{
    std::string_view suffix;
    std::string_view prefix;
};
} // namespace

std::optional<Version> Version::FromString(std::string_view aText) noexcept
{
    std::array<std::uint32_t, 4> parts{0, 0, 0, 0};
    std::size_t partCount = 0;
    std::size_t i = 0;

    while (i < aText.size() && partCount < parts.size())
    {
        if (aText[i] < '0' || aText[i] > '9')
            break;

        std::uint32_t value = 0;
        while (i < aText.size() && aText[i] >= '0' && aText[i] <= '9')
        {
            value = value * 10 + static_cast<std::uint32_t>(aText[i] - '0');
            if (value > std::numeric_limits<std::uint16_t>::max())
                return std::nullopt;
            ++i;
        }

        parts[partCount++] = value;

        if (i < aText.size() && aText[i] == '.')
        {
            ++i;
            continue;
        }

        break;
    }

    if (partCount < 2 || i != aText.size())
        return std::nullopt;

    Version result;
    result.major = static_cast<std::uint16_t>(parts[0]);
    result.minor = static_cast<std::uint16_t>(parts[1]);
    result.revision = static_cast<std::uint16_t>(parts[2]);
    result.build = static_cast<std::uint16_t>(parts[3]);

    if (result.major == 0)
        return std::nullopt;

    return result;
}

std::string Version::ToString() const
{
    std::ostringstream stream;
    stream << major << '.' << minor << '.' << revision << '.' << build;
    return stream.str();
}

std::string Version::ToFileNameSuffix() const
{
    std::ostringstream stream;
    stream << major << '-' << minor << '-' << revision << '-' << build;
    return stream.str();
}

bool Version::operator==(const Version& acOther) const noexcept
{
    return major == acOther.major && minor == acOther.minor && revision == acOther.revision && build == acOther.build;
}

bool Version::operator<(const Version& acOther) const noexcept
{
    if (major != acOther.major)
        return major < acOther.major;
    if (minor != acOther.minor)
        return minor < acOther.minor;
    if (revision != acOther.revision)
        return revision < acOther.revision;
    return build < acOther.build;
}

std::string_view Database::FormatName(Format aFormat) noexcept
{
    switch (aFormat)
    {
    case Format::V1: return "1";
    case Format::V2: return "2";
    case Format::V5: return "5";
    default: return "unknown";
    }
}

void Database::Clear()
{
    m_data.clear();
    m_data.shrink_to_fit();
    m_recordStart = 0;
    m_format = Format::Unknown;
    m_version = {};
    m_pointerSize = 0;
    m_moduleName.clear();
    m_entries.clear();
    m_dense.clear();
}

LoadOutcome Database::Load(const std::filesystem::path& acPath)
{
    std::error_code ec;
    if (!std::filesystem::exists(acPath, ec))
    {
        return {false, "file does not exist: " + acPath.string()};
    }

    const auto size = std::filesystem::file_size(acPath, ec);
    if (ec)
    {
        return {false, "unable to query file size: " + acPath.string() + ": " + ec.message()};
    }

    if (size == 0)
    {
        return {false, "file is empty: " + acPath.string()};
    }

    std::ifstream file(acPath, std::ios::binary);
    if (!file.good())
    {
        return {false, "unable to open file: " + acPath.string()};
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file.good())
    {
        return {false, "unable to read the whole file: " + acPath.string()};
    }

    auto outcome = LoadFromMemory(std::move(data));
    if (!outcome.success)
    {
        outcome.error = acPath.filename().string() + ": " + outcome.error;
    }
    return outcome;
}

LoadOutcome Database::LoadFromMemory(std::vector<std::uint8_t> aData)
{
    Clear();

    if (aData.size() < 8)
    {
        return {false, "file is too small to be an address library database"};
    }

    std::int32_t format = 0;
    {
        Reader reader(aData.data(), aData.size());
        reader.Read(format);
    }

    switch (static_cast<Format>(format))
    {
    case Format::V1:
    case Format::V2:
    case Format::V5: break;
    default:
    {
        std::ostringstream stream;
        stream << "unsupported address library format " << format << " (supported: 1, 2, 5)";
        return {false, stream.str()};
    }
    }

    m_data = std::move(aData);
    m_format = static_cast<Format>(format);
    m_recordStart = sizeof(std::int32_t);

    LoadOutcome outcome = m_format == Format::V5 ? ParseDenseV5() : ParseSparseV1V2(m_recordStart);
    if (!outcome.success)
    {
        Clear();
        return outcome;
    }

    if (m_entries.empty())
    {
        Clear();
        return {false, "database contains no address entries"};
    }

    return {true, {}};
}

LoadOutcome Database::ParseSparseV1V2(std::size_t aRecordStart)
{
    Reader reader(m_data.data(), m_data.size());

    if (!reader.Skip(aRecordStart))
    {
        return {false, "header is truncated"};
    }

    std::int32_t version[4]{};
    for (auto& value : version)
    {
        if (!reader.Read(value))
            return {false, MakeError("unable to read the game version", reader.GetPosition())};
    }

    if (version[0] <= 0 || version[0] > std::numeric_limits<std::uint16_t>::max() || version[1] < 0 ||
        version[1] > std::numeric_limits<std::uint16_t>::max() || version[2] < 0 ||
        version[2] > std::numeric_limits<std::uint16_t>::max() || version[3] < 0 ||
        version[3] > std::numeric_limits<std::uint16_t>::max())
    {
        return {false, "the file does not contain a valid game version"};
    }

    m_version = {static_cast<std::uint16_t>(version[0]), static_cast<std::uint16_t>(version[1]),
                 static_cast<std::uint16_t>(version[2]), static_cast<std::uint16_t>(version[3])};

    std::int32_t nameLength = 0;
    if (!reader.Read(nameLength))
        return {false, MakeError("unable to read the module name length", reader.GetPosition())};

    if (nameLength < 0 || static_cast<std::size_t>(nameLength) > kMaxModuleNameLength)
        return {false, "invalid module name length"};

    if (nameLength > 0)
    {
        if (!reader.CanRead(static_cast<std::size_t>(nameLength)))
            return {false, "module name is truncated"};

        m_moduleName.assign(reinterpret_cast<const char*>(m_data.data() + reader.GetPosition()),
                            static_cast<std::size_t>(nameLength));

        if (!IsPrintableName(m_moduleName))
            return {false, "module name is not valid text"};

        reader.Skip(static_cast<std::size_t>(nameLength));
    }

    std::int32_t pointerSize = 0;
    if (!reader.Read(pointerSize))
        return {false, MakeError("unable to read the pointer size", reader.GetPosition())};

    if (pointerSize < kMinPointerSize || pointerSize > kMaxPointerSize)
        return {false, "invalid pointer size in the database header"};

    m_pointerSize = static_cast<std::uint32_t>(pointerSize);

    std::int32_t addressCount = 0;
    if (!reader.Read(addressCount))
        return {false, MakeError("unable to read the entry count", reader.GetPosition())};

    if (addressCount <= 0)
        return {false, "the database contains no address entries"};

    // Every record is at least one byte, so an entry count larger than the
    // remaining file size always means the file is corrupt.
    if (static_cast<std::size_t>(addressCount) > reader.GetRemaining())
        return {false, "entry count does not match the file size (truncated or corrupt file)"};

    m_entries.reserve(static_cast<std::size_t>(addressCount));

    std::uint64_t previousId = 0;
    std::uint64_t previousOffset = 0;
    const std::uint64_t scalar = m_pointerSize;

    for (std::int32_t i = 0; i < addressCount; ++i)
    {
        std::uint8_t type = 0;
        if (!reader.Read(type))
            return {false, MakeError("record is truncated", reader.GetPosition())};

        const auto idKind = static_cast<std::uint8_t>(type & 0x0F);
        const auto offsetKind = static_cast<std::uint8_t>(type >> 4);

        std::uint64_t id = 0;
        {
            std::uint8_t u8 = 0;
            std::uint16_t u16 = 0;
            std::uint32_t u32 = 0;
            std::uint64_t u64 = 0;

            switch (idKind)
            {
            case 0:
                if (!reader.Read(u64))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                id = u64;
                break;
            case 1: id = previousId + 1; break;
            case 2:
                if (!reader.Read(u8))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                id = previousId + u8;
                break;
            case 3:
                if (!reader.Read(u8))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                if (u8 > previousId)
                    return {false, "invalid address id delta"};
                id = previousId - u8;
                break;
            case 4:
                if (!reader.Read(u16))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                id = previousId + u16;
                break;
            case 5:
                if (!reader.Read(u16))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                if (u16 > previousId)
                    return {false, "invalid address id delta"};
                id = previousId - u16;
                break;
            case 6:
                if (!reader.Read(u16))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                id = u16;
                break;
            case 7:
                if (!reader.Read(u32))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                id = u32;
                break;
            default: return {false, "invalid address id encoding"};
            }
        }

        const bool isScaled = (offsetKind & 0x8) != 0;
        const std::uint64_t baseOffset = isScaled ? (previousOffset / scalar) : previousOffset;

        std::uint64_t offset = 0;
        {
            std::uint8_t u8 = 0;
            std::uint16_t u16 = 0;
            std::uint32_t u32 = 0;
            std::uint64_t u64 = 0;

            switch (offsetKind & 0x7)
            {
            case 0:
                if (!reader.Read(u64))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                offset = u64;
                break;
            case 1: offset = baseOffset + 1; break;
            case 2:
                if (!reader.Read(u8))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                offset = baseOffset + u8;
                break;
            case 3:
                if (!reader.Read(u8))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                if (u8 > baseOffset)
                    return {false, "invalid address offset delta"};
                offset = baseOffset - u8;
                break;
            case 4:
                if (!reader.Read(u16))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                offset = baseOffset + u16;
                break;
            case 5:
                if (!reader.Read(u16))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                if (u16 > baseOffset)
                    return {false, "invalid address offset delta"};
                offset = baseOffset - u16;
                break;
            case 6:
                if (!reader.Read(u16))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                offset = u16;
                break;
            case 7:
                if (!reader.Read(u32))
                    return {false, MakeError("record is truncated", reader.GetPosition())};
                offset = u32;
                break;
            default: return {false, "invalid address offset encoding"};
            }
        }

        if (isScaled)
        {
            if (offset > std::numeric_limits<std::uint64_t>::max() / scalar)
                return {false, "address offset overflow"};
            offset *= scalar;
        }

        previousId = id;
        previousOffset = offset;

        m_entries.push_back({id, offset});
    }

    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& aLeft, const Entry& aRight) { return aLeft.id < aRight.id; });

    return {true, {}};
}

LoadOutcome Database::ParseDenseV5()
{
    Reader reader(m_data.data(), m_data.size());

    // Skip the format field, which was consumed before dispatching.
    if (!reader.Skip(sizeof(std::int32_t)))
        return {false, "header is truncated"};

    std::uint32_t version[4]{};
    for (auto& value : version)
    {
        if (!reader.Read(value))
            return {false, MakeError("unable to read the game version", reader.GetPosition())};
    }

    if (version[0] == 0 || version[0] > std::numeric_limits<std::uint16_t>::max() ||
        version[1] > std::numeric_limits<std::uint16_t>::max() || version[2] > std::numeric_limits<std::uint16_t>::max() ||
        version[3] > std::numeric_limits<std::uint16_t>::max())
    {
        return {false, "the file does not contain a valid game version"};
    }

    m_version = {static_cast<std::uint16_t>(version[0]), static_cast<std::uint16_t>(version[1]),
                 static_cast<std::uint16_t>(version[2]), static_cast<std::uint16_t>(version[3])};

    char nameBuffer[64]{};
    for (auto& character : nameBuffer)
    {
        if (!reader.Read(character))
            return {false, MakeError("unable to read the module name", reader.GetPosition())};
    }

    m_moduleName.assign(nameBuffer, strnlen(nameBuffer, sizeof(nameBuffer)));
    if (!IsPrintableName(m_moduleName))
        return {false, "module name is not valid text"};

    std::int32_t pointerSize = 0;
    std::int32_t dataFormat = 0;
    std::int32_t offsetCount = 0;

    if (!reader.Read(pointerSize))
        return {false, MakeError("unable to read the pointer size", reader.GetPosition())};
    if (pointerSize < kMinPointerSize || pointerSize > kMaxPointerSize)
        return {false, "invalid pointer size in the database header"};

    if (!reader.Read(dataFormat))
        return {false, MakeError("unable to read the data format", reader.GetPosition())};

    if (!reader.Read(offsetCount))
        return {false, MakeError("unable to read the entry count", reader.GetPosition())};

    if (offsetCount <= 0)
        return {false, "the database contains no address entries"};

    if (static_cast<std::size_t>(offsetCount) > reader.GetRemaining() / sizeof(std::uint32_t))
        return {false, "entry count does not match the file size (truncated or corrupt file)"};

    m_pointerSize = static_cast<std::uint32_t>(pointerSize);
    m_dense.resize(static_cast<std::size_t>(offsetCount));
    m_entries.reserve(static_cast<std::size_t>(offsetCount));

    for (std::int32_t id = 0; id < offsetCount; ++id)
    {
        std::uint32_t offset = 0;
        if (!reader.Read(offset))
            return {false, MakeError("record is truncated", reader.GetPosition())};

        if (offset == 0)
            continue;

        m_dense[static_cast<std::size_t>(id)] = offset;
        m_entries.push_back({static_cast<std::uint64_t>(id), offset});
    }

    return {true, {}};
}

std::optional<std::uint64_t> Database::FindOffsetById(std::uint64_t aId) const noexcept
{
    if (!IsLoaded())
        return std::nullopt;

    if (m_format == Format::V5)
    {
        if (aId >= m_dense.size() || m_dense[static_cast<std::size_t>(aId)] == 0)
            return std::nullopt;

        return m_dense[static_cast<std::size_t>(aId)];
    }

    const auto it = std::lower_bound(m_entries.begin(), m_entries.end(), aId,
                                     [](const Entry& aEntry, std::uint64_t aValue) { return aEntry.id < aValue; });

    if (it == m_entries.end() || it->id != aId)
        return std::nullopt;

    return it->offset;
}

std::optional<std::uint64_t> Database::FindIdByOffset(std::uint64_t aOffset) const noexcept
{
    if (!IsLoaded())
        return std::nullopt;

    // Entries are ordered by ID, so scan is the only general option for
    // format 5; for the sparse formats build a temporary map lazily would cost
    // more than a linear scan in the rare callers that use this helper.
    for (const auto& entry : m_entries)
    {
        if (entry.offset == aOffset)
            return entry.id;
    }

    return std::nullopt;
}

std::size_t Database::CountOffsetsAbove(std::uint64_t aImageSize) const noexcept
{
    return static_cast<std::size_t>(std::count_if(m_entries.begin(), m_entries.end(),
                                                  [aImageSize](const Entry& aEntry) { return aEntry.offset >= aImageSize; }));
}

std::uint64_t Database::GetMaxOffset() const noexcept
{
    std::uint64_t maximum = 0;
    for (const auto& entry : m_entries)
    {
        maximum = std::max(maximum, entry.offset);
    }
    return maximum;
}

std::vector<std::string> GetCandidateFileNames(const Version& acVersion)
{
    const auto suffix = acVersion.ToFileNameSuffix();
    return {"versionlib-" + suffix + ".bin", "version-" + suffix + ".bin"};
}

std::string GetSupportedVersionsList()
{
    return "1.5.97.0 (SKSE64 2.0.20, Address Library format 1) and 1.6.x (Address Library format 2/5)";
}
} // namespace AddressLibrary
