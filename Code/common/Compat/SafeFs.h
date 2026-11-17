#pragma once

// Filesystem calls that cannot throw (work plan 4.3, defect M14).
//
// std::filesystem reports errors by exception unless every call is given an std::error_code, and
// three of its habits turned into crashes at start-up for some players:
//
//   * path::string() converts through the ANSI code page of the machine and THROWS when the name has
//     a character that page cannot hold: a Cyrillic save name or user folder on a non-Cyrillic
//     Windows, an emoji, a CJK character. Inside a noexcept function that is std::terminate.
//   * `for (auto& e : directory_iterator(dir, ec))` only protects the construction. The loop advances
//     with operator++, which throws when the walk fails midway (a folder that vanishes, a virtual
//     file system that answers with an error).
//   * exists(p), is_directory(p), current_path(), entry.is_regular_file() without an error_code throw
//     on anything that is not a plain "not found" (access denied, a broken reparse point).
//
// Everything here answers with a value instead. Tools/rfab/check_filesystem_calls.py keeps the
// throwing forms out of the client, the launcher, the server and common/.
//
// Portable: the Linux suite runs it against a real temporary directory, and the UTF-16 conversion
// against literals (on Linux path::value_type is char and no conversion happens).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace Compat::SafeFs
{
namespace detail
{
template <class TChar> [[nodiscard]] constexpr uint32_t CodeUnit(TChar aChar) noexcept
{
    return static_cast<uint32_t>(static_cast<std::make_unsigned_t<TChar>>(aChar));
}

[[nodiscard]] constexpr uint32_t AsciiLower(uint32_t aUnit) noexcept
{
    return (aUnit >= 'A' && aUnit <= 'Z') ? aUnit + ('a' - 'A') : aUnit;
}

inline void AppendUtf8(std::string& aOut, uint32_t aCodePoint)
{
    if (aCodePoint < 0x80u)
        aOut.push_back(static_cast<char>(aCodePoint));
    else if (aCodePoint < 0x800u)
    {
        aOut.push_back(static_cast<char>(0xC0u | (aCodePoint >> 6)));
        aOut.push_back(static_cast<char>(0x80u | (aCodePoint & 0x3Fu)));
    }
    else if (aCodePoint < 0x10000u)
    {
        aOut.push_back(static_cast<char>(0xE0u | (aCodePoint >> 12)));
        aOut.push_back(static_cast<char>(0x80u | ((aCodePoint >> 6) & 0x3Fu)));
        aOut.push_back(static_cast<char>(0x80u | (aCodePoint & 0x3Fu)));
    }
    else
    {
        aOut.push_back(static_cast<char>(0xF0u | (aCodePoint >> 18)));
        aOut.push_back(static_cast<char>(0x80u | ((aCodePoint >> 12) & 0x3Fu)));
        aOut.push_back(static_cast<char>(0x80u | ((aCodePoint >> 6) & 0x3Fu)));
        aOut.push_back(static_cast<char>(0x80u | (aCodePoint & 0x3Fu)));
    }
}
} // namespace detail

inline constexpr uint32_t kReplacementCharacter = 0xFFFDu;

// UTF-16 code units to UTF-8. Bad input never fails the conversion: an unpaired surrogate (NTFS
// allows them in file names) and anything above U+10FFFF become U+FFFD. Throws only std::bad_alloc.
template <class TChar> [[nodiscard]] std::string Utf16ToUtf8Lossy(std::basic_string_view<TChar> aText)
{
    std::string out;
    out.reserve(aText.size());
    for (size_t i = 0; i < aText.size(); ++i)
    {
        uint32_t unit = detail::CodeUnit(aText[i]);
        if (unit >= 0xD800u && unit <= 0xDBFFu)
        {
            const uint32_t next = i + 1 < aText.size() ? detail::CodeUnit(aText[i + 1]) : 0u;
            if (next >= 0xDC00u && next <= 0xDFFFu)
            {
                unit = 0x10000u + ((unit - 0xD800u) << 10) + (next - 0xDC00u);
                ++i;
            }
            else
                unit = kReplacementCharacter;
        }
        else if ((unit >= 0xDC00u && unit <= 0xDFFFu) || unit > 0x10FFFFu)
            unit = kReplacementCharacter;
        detail::AppendUtf8(out, unit);
    }
    return out;
}

// A path as UTF-8, for logs, for comparing names and for APIs that take UTF-8. Never throws and
// never fails on the name itself; returns an empty string only when memory ran out.
[[nodiscard]] inline std::string ToUtf8Lossy(const std::filesystem::path& acPath) noexcept
{
    try
    {
        const auto& native = acPath.native();
        if constexpr (sizeof(std::filesystem::path::value_type) == 1)
            return std::string(reinterpret_cast<const char*>(native.data()), native.size());
        else
            return Utf16ToUtf8Lossy(std::basic_string_view<std::filesystem::path::value_type>(native));
    }
    catch (...)
    {
        return {};
    }
}

// The last component of a path as UTF-8 ("" for a path that ends in a separator).
[[nodiscard]] inline std::string FileNameUtf8Lossy(const std::filesystem::path& acPath) noexcept
{
    try
    {
        return ToUtf8Lossy(acPath.filename());
    }
    catch (...)
    {
        return {};
    }
}

// Does the text end with an ASCII suffix, ignoring ASCII case? Works on the native code units, so
// it needs no conversion and no memory: the way to test an extension (".ess", ".dll").
template <class TChar>
[[nodiscard]] constexpr bool TextEndsWithAsciiNoCase(std::basic_string_view<TChar> aText, std::string_view aAsciiSuffix) noexcept
{
    if (aAsciiSuffix.size() > aText.size())
        return false;
    const size_t offset = aText.size() - aAsciiSuffix.size();
    for (size_t i = 0; i < aAsciiSuffix.size(); ++i)
    {
        if (detail::AsciiLower(detail::CodeUnit(aText[offset + i])) != detail::AsciiLower(detail::CodeUnit(aAsciiSuffix[i])))
            return false;
    }
    return true;
}

[[nodiscard]] inline bool EndsWithAsciiNoCase(const std::filesystem::path& acPath, std::string_view aAsciiSuffix) noexcept
{
    return TextEndsWithAsciiNoCase(std::basic_string_view<std::filesystem::path::value_type>(acPath.native()), aAsciiSuffix);
}

// Where the last component starts in a native path: after the last separator, 0 when there is none.
template <class TChar> [[nodiscard]] constexpr size_t FileNameOffset(std::basic_string_view<TChar> aNativePath) noexcept
{
    size_t i = aNativePath.size();
    while (i > 0)
    {
        const uint32_t unit = detail::CodeUnit(aNativePath[i - 1]);
        if (unit == '/' || unit == detail::CodeUnit(std::filesystem::path::preferred_separator))
            break;
        --i;
    }
    return i;
}

template <class TChar>
[[nodiscard]] constexpr bool TextStartsWithAsciiNoCase(std::basic_string_view<TChar> aText, std::string_view aAsciiPrefix) noexcept
{
    if (aAsciiPrefix.size() > aText.size())
        return false;
    for (size_t i = 0; i < aAsciiPrefix.size(); ++i)
    {
        if (detail::AsciiLower(detail::CodeUnit(aText[i])) != detail::AsciiLower(detail::CodeUnit(aAsciiPrefix[i])))
            return false;
    }
    return true;
}

// Does the last component of the path start with an ASCII prefix, ignoring ASCII case? Like
// EndsWithAsciiNoCase it works on the native text: no path::filename(), no conversion, no memory.
[[nodiscard]] inline bool FileNameStartsWithAsciiNoCase(const std::filesystem::path& acPath, std::string_view aAsciiPrefix) noexcept
{
    const std::basic_string_view<std::filesystem::path::value_type> native(acPath.native());
    return TextStartsWithAsciiNoCase(native.substr(FileNameOffset(native)), aAsciiPrefix);
}

// ---- queries: false on "no" and on any error ----

[[nodiscard]] inline bool Exists(const std::filesystem::path& acPath) noexcept
{
    std::error_code ec;
    return std::filesystem::exists(acPath, ec) && !ec;
}

[[nodiscard]] inline bool IsDirectory(const std::filesystem::path& acPath) noexcept
{
    std::error_code ec;
    return std::filesystem::is_directory(acPath, ec) && !ec;
}

[[nodiscard]] inline bool IsRegularFile(const std::filesystem::path& acPath) noexcept
{
    std::error_code ec;
    return std::filesystem::is_regular_file(acPath, ec) && !ec;
}

[[nodiscard]] inline bool IsDirectory(const std::filesystem::directory_entry& acEntry) noexcept
{
    std::error_code ec;
    return acEntry.is_directory(ec) && !ec;
}

[[nodiscard]] inline bool IsRegularFile(const std::filesystem::directory_entry& acEntry) noexcept
{
    std::error_code ec;
    return acEntry.is_regular_file(ec) && !ec;
}

// The current directory, or aFallback when the system cannot name it (it was deleted under the
// process, or memory ran out).
[[nodiscard]] inline std::filesystem::path CurrentPathOr(const std::filesystem::path& acFallback) noexcept
{
    try
    {
        std::error_code ec;
        std::filesystem::path current = std::filesystem::current_path(ec);
        if (ec || current.empty())
            return acFallback;
        return current;
    }
    catch (...)
    {
        try
        {
            return acFallback;
        }
        catch (...)
        {
            return {};
        }
    }
}

// Creates the directory and its parents. True when it exists afterwards.
inline bool CreateDirectories(const std::filesystem::path& acPath) noexcept
{
    std::error_code ec;
    std::filesystem::create_directories(acPath, ec);
    return IsDirectory(acPath);
}

// ---- walking a directory ----

// Calls aVisit(const std::filesystem::directory_entry&) for every entry of acDir (not recursive, no
// order). aVisit returns void, or bool where false stops the walk.
//
// Returns true when the walk reached the end or was stopped by aVisit; false when the directory
// could not be opened or the walk failed midway (the entries seen before that were delivered).
// Never throws: an exception from aVisit (out of memory in a push_back) ends the walk with false.
template <class TVisit> bool ForEachEntry(const std::filesystem::path& acDir, TVisit&& aVisit) noexcept
{
    try
    {
        std::error_code ec;
        std::filesystem::directory_iterator it(acDir, ec);
        if (ec)
            return false;

        const std::filesystem::directory_iterator end;
        while (it != end)
        {
            const std::filesystem::directory_entry& entry = *it;
            if constexpr (std::is_void_v<std::invoke_result_t<TVisit&, const std::filesystem::directory_entry&>>)
            {
                aVisit(entry);
            }
            else
            {
                if (!aVisit(entry))
                    return true;
            }

            it.increment(ec);
            if (ec)
                return false;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace Compat::SafeFs
