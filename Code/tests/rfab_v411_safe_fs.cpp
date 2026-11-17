// Work plan 4.3 / defect M14: filesystem calls that cannot throw (common/Compat/SafeFs.h), and the
// save quarantine that used the throwing forms inside noexcept functions.
//
// The file names below are the ones that ended the game at start-up: a name that the machine's ANSI
// code page cannot hold makes path::string() throw. On the Windows build machine (code page 1252)
// the Cyrillic, CJK and emoji names here are exactly such names, so this suite reproduces the crash
// there; on Linux the same cases check the logic.

#include <catch2/catch.hpp>

#include <common/Compat/SafeFs.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace Compat::SafeFs;

namespace
{
// Names as UTF-8 bytes, turned into paths without going through the ANSI code page.
fs::path Utf8Path(std::string_view aUtf8)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(aUtf8.data()), aUtf8.size()));
}

constexpr std::string_view kCyrillic = "\xD0\xA1\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 12";  // "Сохранение 12"
constexpr std::string_view kCjk = "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x98\xE6\xA1\xA3";                                              // "中文存档"
constexpr std::string_view kEmoji = "save \xF0\x9F\x98\x80";                                                                        // "save 😀"

struct TempDir
{
    fs::path Path;

    explicit TempDir(const char* apName)
    {
        std::error_code ec;
        Path = fs::temp_directory_path(ec) / apName;
        fs::remove_all(Path, ec);
        fs::create_directories(Path, ec);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(Path, ec);
    }

    void Write(const fs::path& acRelative, std::string_view aContent) const
    {
        std::ofstream out(Path / acRelative, std::ios::binary);
        out.write(aContent.data(), static_cast<std::streamsize>(aContent.size()));
    }
};

} // namespace

TEST_CASE("v4.1.1 safe fs: UTF-16 to UTF-8 never fails on the text itself", "[rfab][v411][safefs]")
{
    SECTION("ASCII, Cyrillic, CJK")
    {
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(u"Save 1.ess")) == "Save 1.ess");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(u"\u0421\u043e\u0445")) == "\xD0\xA1\xD0\xBE\xD1\x85");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(u"\u4E2D\u6587")) == "\xE4\xB8\xAD\xE6\x96\x87");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(u"")) == "");
    }

    SECTION("a surrogate pair becomes one four-byte character")
    {
        const char16_t emoji[] = {0xD83D, 0xDE00, 0};
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(emoji)) == "\xF0\x9F\x98\x80");
    }

    SECTION("an unpaired surrogate (NTFS allows it in a file name) becomes U+FFFD instead of an exception")
    {
        const char16_t highInMiddle[] = {u'a', 0xD83D, u'b', 0};
        const char16_t highAtEnd[] = {u'x', 0xD83D, 0};
        const char16_t lowAlone[] = {0xDE00, u'y', 0};
        const char16_t twoHighs[] = {0xD83D, 0xD83D, 0xDE00, 0};
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(highInMiddle)) == "a\xEF\xBF\xBD" "b");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(highAtEnd)) == "x\xEF\xBF\xBD");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(lowAlone)) == "\xEF\xBF\xBD" "y");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(twoHighs)) == "\xEF\xBF\xBD\xF0\x9F\x98\x80");
    }

    SECTION("32-bit code units (wchar_t on Linux) pass through as code points")
    {
        const char32_t text[] = {U'a', 0x1F600, 0x110000, 0};
        CHECK(Utf16ToUtf8Lossy(std::u32string_view(text)) == "a\xF0\x9F\x98\x80\xEF\xBF\xBD");
    }
}

TEST_CASE("v4.1.1 safe fs: extension tests work on the native text", "[rfab][v411][safefs]")
{
    CHECK(TextEndsWithAsciiNoCase(std::string_view("Save 1.ESS"), ".ess"));
    CHECK(TextEndsWithAsciiNoCase(std::u16string_view(u"\u0421\u043e\u0445.Ess"), ".ess"));
    CHECK(TextEndsWithAsciiNoCase(std::wstring_view(L"\u4E2D\u6587.ess.TMP"), ".ess.tmp"));
    CHECK(TextEndsWithAsciiNoCase(std::string_view("anything"), ""));
    CHECK_FALSE(TextEndsWithAsciiNoCase(std::string_view("a.es"), ".ess"));
    CHECK_FALSE(TextEndsWithAsciiNoCase(std::string_view("ess"), ".ess"));
    CHECK_FALSE(TextEndsWithAsciiNoCase(std::string_view("save.esp"), ".ess"));
    // Only ASCII letters fold: a code unit that merely shares its low byte with a letter is different.
    CHECK_FALSE(TextEndsWithAsciiNoCase(std::u16string_view(u"save.e\u0173s"), ".ess"));

    CHECK(EndsWithAsciiNoCase(fs::path("dir") / "Quick.ESS", ".ess"));
    CHECK(EndsWithAsciiNoCase(Utf8Path(kCjk) += ".skse", ".SKSE"));
    CHECK_FALSE(EndsWithAsciiNoCase(fs::path("save.ess.bak"), ".ess"));
    CHECK_FALSE(EndsWithAsciiNoCase(fs::path(), ".ess"));
}

TEST_CASE("v4.1.1 safe fs: a path of any name can be written to a log", "[rfab][v411][safefs]")
{
    for (const std::string_view name : {kCyrillic, kCjk, kEmoji, std::string_view("plain")})
    {
        const fs::path path = fs::path("Saves") / Utf8Path(name);
        CHECK(FileNameUtf8Lossy(path) == name);
        const std::string whole = ToUtf8Lossy(path);
        REQUIRE(whole.size() > name.size());
        CHECK(whole.substr(whole.size() - name.size()) == name);
    }
    CHECK(ToUtf8Lossy(fs::path()) == "");
}

TEST_CASE("v4.1.1 safe fs: queries answer false instead of throwing", "[rfab][v411][safefs]")
{
    TempDir tmp("rfab_safefs_queries");
    tmp.Write("file.bin", "x");

    CHECK(Exists(tmp.Path));
    CHECK(IsDirectory(tmp.Path));
    CHECK_FALSE(IsRegularFile(tmp.Path));
    CHECK(IsRegularFile(tmp.Path / "file.bin"));
    CHECK_FALSE(IsDirectory(tmp.Path / "file.bin"));
    CHECK_FALSE(Exists(tmp.Path / "missing"));
    CHECK_FALSE(IsDirectory(tmp.Path / "missing" / "deeper"));
    CHECK_FALSE(Exists(fs::path()));

    CHECK(CreateDirectories(tmp.Path / "a" / "b" / Utf8Path(kCyrillic)));
    CHECK(CreateDirectories(tmp.Path / "a" / "b"));                 // already there
    CHECK_FALSE(CreateDirectories(tmp.Path / "file.bin" / "below"));  // a file is in the way

    const fs::path fallback = "fallback-directory";
    CHECK_FALSE(CurrentPathOr(fallback).empty());
    CHECK(CurrentPathOr(fallback) != fallback);  // the test process has a working directory
}

TEST_CASE("v4.1.1 safe fs: walking a directory", "[rfab][v411][safefs]")
{
    TempDir tmp("rfab_safefs_walk");
    tmp.Write("a.txt", "1");
    tmp.Write(Utf8Path(kCyrillic) += ".ess", "2");
    tmp.Write(Utf8Path(kCjk) += ".ess", "3");
    tmp.Write(Utf8Path(kEmoji) += ".skse", "4");
    REQUIRE(CreateDirectories(tmp.Path / "sub"));

    SECTION("every entry is visited once, whatever its name")
    {
        std::vector<std::string> names;
        size_t directories = 0;
        const bool ok = ForEachEntry(tmp.Path, [&](const fs::directory_entry& acEntry) {
            names.push_back(FileNameUtf8Lossy(acEntry.path()));
            if (IsDirectory(acEntry))
                ++directories;
        });
        CHECK(ok);
        std::sort(names.begin(), names.end());
        std::vector<std::string> expected{"a.txt", "sub", std::string(kCyrillic) + ".ess", std::string(kCjk) + ".ess",
                                          std::string(kEmoji) + ".skse"};
        std::sort(expected.begin(), expected.end());
        CHECK(names == expected);
        CHECK(directories == 1);
    }

    SECTION("a visitor that returns false stops the walk, and that is still a successful walk")
    {
        size_t visited = 0;
        CHECK(ForEachEntry(tmp.Path, [&](const fs::directory_entry&) {
            ++visited;
            return false;
        }));
        CHECK(visited == 1);
    }

    SECTION("a visitor that returns true keeps going")
    {
        size_t visited = 0;
        CHECK(ForEachEntry(tmp.Path, [&](const fs::directory_entry&) {
            ++visited;
            return true;
        }));
        CHECK(visited == 5);
    }

    SECTION("a directory that is not there, or is a file: false, nothing visited")
    {
        size_t visited = 0;
        CHECK_FALSE(ForEachEntry(tmp.Path / "missing", [&](const fs::directory_entry&) { ++visited; }));
        CHECK_FALSE(ForEachEntry(tmp.Path / "a.txt", [&](const fs::directory_entry&) { ++visited; }));
        CHECK_FALSE(ForEachEntry(fs::path(), [&](const fs::directory_entry&) { ++visited; }));
        CHECK(visited == 0);
    }

    SECTION("an exception from the visitor ends the walk; it does not leave a noexcept function")
    {
        size_t visited = 0;
        const bool ok = ForEachEntry(tmp.Path, [&](const fs::directory_entry&) {
            ++visited;
            throw std::bad_alloc();
        });
        CHECK_FALSE(ok);
        CHECK(visited == 1);
    }

    SECTION("an empty directory is a successful walk of nothing")
    {
        size_t visited = 0;
        CHECK(ForEachEntry(tmp.Path / "sub", [&](const fs::directory_entry&) { ++visited; }));
        CHECK(visited == 0);
    }
}

