#!/usr/bin/env python3
"""Filesystem calls that can throw (work plan 4.3, defect M14).

std::filesystem reports errors by exception unless every call gets an std::error_code. In this code
base that became crashes at start-up, before any log could say why:

  narrow     path::string() / generic_string() convert through the ANSI code page of the machine
             and throw when the name has a character that page cannot hold (a Cyrillic folder on a
             non-Cyrillic Windows, an emoji in a save name). Use Compat::SafeFs::ToUtf8Lossy, or
             keep the path a path.
  iterate    `for (x : directory_iterator(dir, ec))` protects only the construction; the loop
             advances with operator++, which throws when the walk fails midway. A directory_iterator
             built without an error_code throws on construction as well.
             Use Compat::SafeFs::ForEachEntry.
  query      exists(p), is_directory(p), file_size(p), rename(a, b), current_path() ... without an
             error_code, and entry.is_regular_file() / entry.file_size() without one.
             Pass an error_code or use the Compat::SafeFs wrappers.

The rules apply to all shipped code under Code/ (tests are free to throw: a test that throws fails).
A line may opt out with a trailing comment `// fs-throws-ok: <why>` when the throwing form is what
the code needs and the exception is handled right there.

Usage:  check_filesystem_calls.py [--root DIR] [--verbose]
        check_filesystem_calls.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Shipped code. Code/tests is exercised by the test runner, which reports a throw as a failure.
NOT_CHECKED = ("tests/",)
# The header that implements the non-throwing forms uses the std calls it wraps.
IMPLEMENTATION = ("common/Compat/SafeFs.h",)

FREE_FUNCTIONS = (
    "exists|is_directory|is_regular_file|is_symlink|is_empty|is_other|file_size|remove|remove_all|rename|"
    "create_directories|create_directory|create_symlink|create_hard_link|copy_file|copy|last_write_time|canonical|"
    "weakly_canonical|absolute|current_path|temp_directory_path|equivalent|status|symlink_status|resize_file|space|"
    "read_symlink|hard_link_count|permissions|relative|proximate"
)
NAMESPACE = r"(?:std::filesystem|fs|filesystem)"
FREE_CALL = re.compile(r"\b" + NAMESPACE + r"::(" + FREE_FUNCTIONS + r")\s*\(")
# The same functions called without a namespace (argument-dependent lookup finds them for a path).
# Only the names that cannot be anything else: exists(), remove() or rename() alone are too common.
UNQUALIFIED_FUNCTIONS = (
    "exists|is_directory|is_regular_file|file_size|create_directories|create_directory|remove_all|current_path|"
    "last_write_time|copy_file|temp_directory_path|weakly_canonical"
)
UNQUALIFIED_CALL = re.compile(r"(?<![\w:.>~])(" + UNQUALIFIED_FUNCTIONS + r")\s*\(")
# `directory_iterator(dir)`, `directory_iterator it(dir)` and `directory_iterator it{dir}`.
ITERATOR = re.compile(r"\b" + NAMESPACE + r"::(recursive_directory_iterator|directory_iterator)\s*(?:\w+\s*)?([({])")
ENTRY_QUERY = re.compile(
    r"(?:\.|->)\s*(is_regular_file|is_directory|is_symlink|is_other|file_size|last_write_time|exists|status|symlink_status|hard_link_count)\s*\(\s*\)"
)
NARROW = re.compile(r"(?:\.|->)\s*(string|generic_string)\s*\(\s*\)")
# An argument that is an error_code by this code base's naming: ec, existsEc, sizeEc2, error, aError.
ERROR_CODE_ARG = re.compile(r"\b\w*(?:[eE]c\d*|[eE]rror\w*)\b")
RANGE_FOR_HEAD = re.compile(r"\bfor\s*\([^;()]*:\s*$")
ALLOW = re.compile(r"//\s*fs-throws-ok:\s*\S")


def strip_code(text: str) -> str:
    """Blanks comments and string/char literals, keeping every offset and newline."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "R" and nxt == '"':
            m = re.match(r'R"([^()\\ ]{0,16})\(', text[i:])
            if not m:
                i += 1
                continue
            end = text.find(")" + m.group(1) + '"', i + m.end())
            end = n if end < 0 else end + len(m.group(1)) + 2
            for k in range(i, end):
                if out[k] != "\n":
                    out[k] = " "
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def match_close(s: str, open_at: int) -> int:
    op = s[open_at]
    cl = ")" if op == "(" else "}"
    depth = 0
    for i in range(open_at, len(s)):
        if s[i] == op:
            depth += 1
        elif s[i] == cl:
            depth -= 1
            if depth == 0:
                return i
    return -1


def line_of(s: str, offset: int) -> int:
    return s.count("\n", 0, offset) + 1


def check_source(rel: str, raw: str) -> list[str]:
    s = strip_code(raw)
    raw_lines = raw.split("\n")
    found: list[tuple[int, str]] = []

    for m in FREE_CALL.finditer(s):
        close = match_close(s, m.end() - 1)
        args = s[m.end() : close] if close >= 0 else ""
        if not ERROR_CODE_ARG.search(args):
            found.append((line_of(s, m.start()), f"query: {m.group(1)}() without an error_code"))

    for m in UNQUALIFIED_CALL.finditer(s):
        close = match_close(s, m.end() - 1)
        args = s[m.end() : close] if close >= 0 else ""
        # A declaration of a function with such a name has a type in front of it; a call does not.
        before = s[max(0, m.start() - 40) : m.start()].rstrip()
        if re.search(r"(?:\bbool|\bauto|\bvoid|\buintmax_t|\bpath|[>&*])$", before):
            continue
        if not ERROR_CODE_ARG.search(args):
            found.append((line_of(s, m.start()), f"query: {m.group(1)}() without an error_code"))

    for m in ITERATOR.finditer(s):
        close = match_close(s, m.end() - 1)
        args = s[m.end() : close] if close >= 0 else ""
        head = s[max(0, m.start() - 160) : m.start()]
        if not ERROR_CODE_ARG.search(args):
            found.append((line_of(s, m.start()), f"iterate: {m.group(1)} built without an error_code"))
        elif RANGE_FOR_HEAD.search(head):
            found.append((line_of(s, m.start()), f"iterate: range-for over {m.group(1)} advances with a throwing operator++"))

    for m in ENTRY_QUERY.finditer(s):
        found.append((line_of(s, m.start()), f"query: .{m.group(1)}() without an error_code"))

    for m in NARROW.finditer(s):
        found.append((line_of(s, m.start()), f"narrow: .{m.group(1)}() throws on a name outside the ANSI code page"))

    failures = []
    for line, problem in sorted(set(found)):
        if 0 < line <= len(raw_lines) and ALLOW.search(raw_lines[line - 1]):
            continue
        failures.append(f"{rel}:{line} {problem}")
    return failures


def run(root: Path, verbose: bool) -> list[str]:
    code = root / "Code"
    failures: list[str] = []
    checked = 0
    users = 0
    for path in sorted(code.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp", ".inl") or not path.is_file():
            continue
        rel = path.relative_to(code).as_posix()
        if any(rel.startswith(skip) for skip in NOT_CHECKED) or rel in IMPLEMENTATION:
            continue
        raw = path.read_text(encoding="utf-8", errors="replace")
        checked += 1
        if "filesystem" in raw or "fs::" in raw:
            users += 1
        failures += check_source(f"Code/{rel}", raw)
    if verbose:
        print(f"checked {checked} sources, {users} of them use the filesystem")
    # Parser rot guard: the real tree has dozens of filesystem users.
    if root == REPO_ROOT and users < 20:
        failures.append(f"parser: only {users} filesystem users found; the scan no longer reads the sources")
    return failures


# ---------------------------------------------------------------------------------------------------
# Self-test: every rule must catch its mistake and leave the correct pattern alone
# ---------------------------------------------------------------------------------------------------

SELF_TEST_FILES = {
    "client/Good.cpp": """
#include <filesystem>
void Good(const std::filesystem::path& acDir)
{
    std::error_code ec;
    if (std::filesystem::exists(acDir, ec) && std::filesystem::is_directory(acDir, ec)) {}
    std::error_code existsEc1;
    if (std::filesystem::exists(acDir, existsEc1)) {}
    const auto size = std::filesystem::file_size(acDir / "a", ec);
    std::filesystem::rename(acDir / "a", acDir / "b", ec);
    Compat::SafeFs::ForEachEntry(acDir, [](const std::filesystem::directory_entry& acEntry) {
        std::error_code entryEc;
        if (acEntry.is_regular_file(entryEc)) {}
        spdlog::info("{}", Compat::SafeFs::ToUtf8Lossy(acEntry.path()));
    });
    for (std::filesystem::directory_iterator it(acDir, ec), end; !ec && it != end; it.increment(ec)) {}
    const std::string text = stream.str();          // not a path
    const auto wide = acDir.wstring();              // never converts through the code page
    const auto u8 = acDir.u8string();
    spdlog::info("exists() and .string() in a string literal are text");  // and in a comment: p.string()
    const bool there = Exists(acDir);               // the wrapper
    if (exists(acDir, ec) && !is_directory(acDir, ec)) {}   // unqualified, with an error_code
    std::filesystem::directory_iterator held(acDir, ec);
    const std::string narrow = acDir.string(); // fs-throws-ok: Lua needs the ANSI name, caught below
}
""",
    "server/Bad.cpp": """
#include <filesystem>
namespace fs = std::filesystem;
void Bad(const fs::path& acDir)
{
    if (fs::exists(acDir)) {}
    const auto work = std::filesystem::current_path();
    std::filesystem::create_directories(acDir / "x");
    for (const auto& entry : fs::directory_iterator(acDir)) {}
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(acDir, ec))
    {
        if (entry.is_regular_file()) {}
        const auto size = entry.file_size();
        const std::string name = entry.path().filename().string();
    }
    fs::recursive_directory_iterator walk(acDir);
    const std::string generic = acDir.generic_string();
    const std::string excused = acDir.string(); // fs-throws-ok:
    if (exists(acDir) || !is_directory(acDir, ec)) {}
    fs::directory_iterator braced{acDir};
}
""",
    "tests/Throwing.cpp": """
void Test(const std::filesystem::path& p) { if (std::filesystem::exists(p)) { p.string(); } }
""",
    "common/Compat/SafeFs.h": """
inline bool Probe(const std::filesystem::path& p) { return std::filesystem::exists(p); }
""",
}

SELF_TEST_EXPECT = [
    ("Code/server/Bad.cpp:6 query: exists() without an error_code", True),
    ("Code/server/Bad.cpp:7 query: current_path() without an error_code", True),
    ("Code/server/Bad.cpp:8 query: create_directories() without an error_code", True),
    ("Code/server/Bad.cpp:9 iterate: directory_iterator built without an error_code", True),
    ("Code/server/Bad.cpp:11 iterate: range-for over directory_iterator advances with a throwing operator++", True),
    ("Code/server/Bad.cpp:13 query: .is_regular_file() without an error_code", True),
    ("Code/server/Bad.cpp:14 query: .file_size() without an error_code", True),
    ("Code/server/Bad.cpp:15 narrow: .string() throws on a name outside the ANSI code page", True),
    ("Code/server/Bad.cpp:17 iterate: recursive_directory_iterator built without an error_code", True),
    ("Code/server/Bad.cpp:18 narrow: .generic_string() throws", True),
    ("Code/server/Bad.cpp:19 narrow: .string() throws", True),  # an allow marker needs a reason
    ("Code/server/Bad.cpp:20 query: exists() without an error_code", True),  # unqualified, found through ADL
    ("Code/server/Bad.cpp:21 iterate: directory_iterator built without an error_code", True),
    ("Good.cpp", False),
    ("tests/", False),
    ("SafeFs.h", False),
]


def self_test() -> list[str]:
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in SELF_TEST_FILES.items():
            path = root / "Code" / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        results = run(root, verbose=False)

    for needle, present in SELF_TEST_EXPECT:
        if any(needle in r for r in results) != present:
            failures.append(f"{'missing' if present else 'unexpected'}: {needle!r} in {results}")
    expected_total = sum(1 for _, present in SELF_TEST_EXPECT if present)
    if len(results) != expected_total:
        failures.append(f"expected exactly {expected_total} findings, got {len(results)}: {results}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        failures = self_test()
        for failure in failures:
            print(f"SELF-TEST FAIL: {failure}")
        print(f"filesystem calls self-test: {len(SELF_TEST_EXPECT) + 1 - len(failures)} checks, {len(failures)} failed")
        return 1 if failures else 0

    failures = run(args.root, args.verbose)
    for failure in failures:
        print(f"FAIL: {failure}")
    print(f"filesystem calls: {len(failures)} violation(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
