#!/usr/bin/env python3
"""Turns the log of a failed CI step into GitHub annotations.

Why this exists
---------------
Development of this repository runs through a GitHub App and a sandbox that can reach
api.github.com but not the hosts that serve workflow logs and artifacts. A failed run is therefore
visible only as step statuses and annotations. Before this script a Linux job reported
"Process completed with exit code 1" and nothing else, and the Windows build reported the last
3500 characters of everything that looked like an error - on a cascade of compiler errors that is
the end of the cascade, never its cause.

What it does
------------
Reads a log, picks out what a person would look for first - compiler and linker errors, failed test
assertions with their expansion, sanitizer reports, Python tracebacks, failed source checks - keeps
the order in which they happened, drops repeats, shortens runner paths, and prints them as
`::error` annotations (DIAG_ERRORS_n_OF_m) in chunks GitHub accepts, between a `::warning` with a
one-line summary (DIAG_SUMMARY) and one with the tail of the log (DIAG_LOG_TAIL). With --ok it prints one `::notice` with the result lines, so a green run can be told apart
from a run that executed nothing.

It never fails the step it reports on: any problem inside this script becomes one more annotation.

Usage
-----
  python3 Tools/rfab/ci_diagnostics.py --log step.log --step "core tests" --exit-code 2
  python3 Tools/rfab/ci_diagnostics.py --log step.log --step "core tests" --ok
  python3 Tools/rfab/ci_diagnostics.py --self-test
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# GitHub shows at most 10 annotations of each level per step; one error slot is left for the caller.
MAX_ERROR_CHUNKS = 8
CHUNK_CHARS = 3300
LINE_CHARS = 360
TAIL_LINES = 40

ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")

MSVC = re.compile(
    r"^(?:\s*error:\s*)?(?P<file>(?:[A-Za-z]:)?[^:*?\"<>|\r\n]+?)\((?P<line>\d+)(?:,\s*\d+)?\)\s*:\s*"
    r"(?:fatal error|error)\s+(?P<code>[A-Z]{1,4}\d{3,5})\s*:\s*(?P<msg>.*)$"
)
MSVC_NOTE = re.compile(r"\)\s*:\s*note:|^\s+(?:with\b|and\b|\[|\]|see |while |could be|or  )")
LINK = re.compile(r"\b(?:fatal error|error)\s+LNK\d{4}\b|unresolved external symbol|\bLINK\s*:\s*fatal")
GCC = re.compile(r"^(?P<file>[^\s:][^:\r\n]*):(?P<line>\d+):(?:\d+:)?\s*(?:fatal error|error):\s*(?P<msg>.*)$")
GCC_CONTEXT = re.compile(r"^\s+(?:\d+\s*)?\|")
GCC_LINK = re.compile(r"undefined reference to|multiple definition of|collect2: error|ld returned \d+ exit status|cannot find -l")
CATCH_FAILED = re.compile(r"(?:\(\d+\)|:\d+): FAILED:")
CATCH_TOTALS = re.compile(r"^(?:test cases|assertions)\s*:")
RULE_LINE = re.compile(r"^[-=~.]{20,}\s*$")
DASH_RULE = re.compile(r"^-{20,}\s*$")
HARNESS_FAIL = re.compile(r"^\s*FAIL\b|^FAILED \(\d+ failure")
SANITIZER = re.compile(r"ERROR: \w*Sanitizer|runtime error:|^SUMMARY: \w*Sanitizer")
STACK_FRAME = re.compile(r"^\s+#\d+\s")
TRACEBACK = re.compile(r"^Traceback \(most recent call last\):")
PY_EXCEPTION = re.compile(r"^[A-Za-z_][\w.]*(?:Error|Exception|Exit|Interrupt)\b")
MAKE = re.compile(r"^make(?:\[\d+\])?: \*\*\*")
NODE = re.compile(r"\berror TS\d+\b|ERR_PNPM_|\bnpm ERR!|^Error: ")
CHECKER = re.compile(
    r"^\[[a-z0-9 -]+\]\s*(?:FAILED|NEW|STALE|NOREASON|MISMATCH|ERROR)\b|\b[1-9]\d* violation\(s\)|"
    r"self-test FAILED|SELF-TEST FAIL|\b[1-9]\d* failed\b|^!! "
)
CHECKER_ITEM = re.compile(r"^\s+- \S")
XMAKE = re.compile(r"^error:\s")
GENERIC = re.compile(
    r"(?i)\b(?:error|fatal|failed|failure|exception|assert(?:ion)?|denied|cannot|could not|unresolved|"
    r"not found|timed out|terminate called|segmentation fault|core dumped|aborted)\b"
)
RESULT_LINE = re.compile(
    r"Passed all|^All tests passed \(|^test cases\s*:|^assertions\s*:|^PASSED \(|\d+ violation\(s\)|^\[release-invariants\] OK\b|"
    r"^\[scan\] Total|self-test: |every rule catches|^==> all |^\[RFAB_RESULT\] "
)
# Any script may state a fact about a successful step by printing a line that starts with this
# marker; it ends up in the notice of the step, which is all of a CI run that can be read back
# from the development sandbox.
RESULT_MARKER = "[RFAB_RESULT] "


def read_log(path: str) -> list[str]:
    with open(path, "rb") as handle:
        raw = handle.read()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff") or raw[1:2] == b"\x00":
        text = raw.decode("utf-16", errors="replace")
    else:
        text = raw.decode("utf-8", errors="replace")
    text = ANSI.sub("", text).replace("\r\n", "\n").replace("\r", "\n")
    return text.split("\n")


def shorten_paths(line: str, workspace: str) -> str:
    if workspace:
        for prefix in {workspace.rstrip("\\/"), workspace.rstrip("\\/").replace("\\", "/")}:
            if prefix:
                line = re.sub(re.escape(prefix) + r"[\\/]", "", line, flags=re.IGNORECASE)
    # GitHub-hosted runner checkouts: D:\a\<repo>\<repo>\  and  /home/runner/work/<repo>/<repo>/
    # (no word boundary in front: `-ID:\a\repo\repo\Code` glues the drive letter to the flag)
    line = re.sub(r"(?i)[A-Z]:[\\/]a[\\/][^\\/\s]+[\\/][^\\/\s]+[\\/]", "", line)
    line = re.sub(r"/home/runner/work/[^/\s]+/[^/\s]+/", "", line)
    # xmake package cache: ...\.xmake\packages\t\tiltedcore\v0.2.9\<32 hex>\include\x.h
    line = re.sub(
        r"(?i)[A-Z]:[\\/]Users[\\/][^\\/]+[\\/]AppData[\\/]Local[\\/]\.xmake[\\/]packages[\\/]\w[\\/]([^\\/]+)[\\/]([^\\/]+)[\\/][0-9a-f]{32}[\\/]",
        r"<pkg \1 \2>/",
        line,
    )
    line = re.sub(r"(?i)[A-Z]:[\\/]Program Files[^:]*?[\\/]VC[\\/]Tools[\\/]MSVC[\\/][\d.]+[\\/]", "<msvc>/", line)
    line = re.sub(r"(?i)[A-Z]:[\\/]Program Files \(x86\)[\\/]Windows Kits[\\/]10[\\/]include[\\/][\d.]+[\\/]", "<winsdk>/", line)
    return line


def clip(line: str, limit: int = LINE_CHARS) -> str:
    line = line.rstrip()
    if len(line) <= limit:
        return line
    head = limit - 110
    return line[:head] + " ... " + line[-100:]


class Diagnostic:
    def __init__(self, kind: str, key: str, lines: list[str]) -> None:
        self.kind = kind
        self.key = key
        self.lines = lines


def extract(raw_lines: list[str], workspace: str) -> tuple[list[Diagnostic], list[Diagnostic], int]:
    """Returns (primary, secondary, duplicates). Primary: what broke. Secondary: anything error-like."""
    lines = [shorten_paths(line, workspace).replace("\\", "/") for line in raw_lines]
    primary: list[Diagnostic] = []
    secondary: list[Diagnostic] = []
    seen: set[str] = set()
    duplicates = 0
    consumed: set[int] = set()
    current_test = ""

    def add(target: list[Diagnostic], kind: str, key: str, block: list[str]) -> None:
        nonlocal duplicates
        if key in seen:
            duplicates += 1
            return
        seen.add(key)
        target.append(Diagnostic(kind, key, [clip(b) for b in block if b.strip()]))

    count = len(lines)
    for index, line in enumerate(lines):
        if index in consumed:
            continue
        stripped = line.strip()
        if not stripped:
            continue

        # Catch2 prints the test case name between two rules of dashes, before the failing
        # assertion (the file line that follows sits between a dashed and a dotted rule).
        if DASH_RULE.match(stripped) and index + 2 < count and DASH_RULE.match(lines[index + 2].strip()):
            candidate = lines[index + 1].strip()
            if candidate and not RULE_LINE.match(candidate):
                current_test = candidate
            continue

        match = MSVC.match(stripped)
        if match:
            block = [f"{match.group('file').strip()}({match.group('line')}): error {match.group('code')}: {match.group('msg')}"]
            look = index + 1
            while look < count and len(block) < 4 and MSVC_NOTE.search(lines[look]) and not MSVC.match(lines[look].strip()):
                block.append("    " + lines[look].strip())
                consumed.add(look)
                look += 1
            add(primary, "compile", block[0], block)
            continue

        if LINK.search(stripped) or GCC_LINK.search(stripped):
            add(primary, "link", stripped, [stripped])
            continue

        match = GCC.match(stripped)
        if match:
            block = [stripped]
            look = index + 1
            while look < count and len(block) < 3 and GCC_CONTEXT.match(lines[look]):
                block.append(lines[look].rstrip())
                consumed.add(look)
                look += 1
            add(primary, "compile", stripped, block)
            continue

        if CATCH_FAILED.search(stripped):
            block = ([f"[test] {current_test}"] if current_test else []) + [stripped]
            look = index + 1
            blanks = 0
            while look < count and len(block) < 14:
                text = lines[look]
                if RULE_LINE.match(text.strip() or "x"):
                    break
                if not text.strip():
                    blanks += 1
                    if blanks > 1:
                        break
                else:
                    block.append(text.rstrip())
                consumed.add(look)
                look += 1
            add(primary, "test", f"{current_test}|{stripped}", block)
            continue

        if CATCH_TOTALS.match(stripped) or HARNESS_FAIL.match(line):
            add(primary, "test", stripped, [stripped])
            continue

        if SANITIZER.search(stripped):
            block = [stripped]
            look = index + 1
            while look < count and len(block) < 9 and (STACK_FRAME.match(lines[look]) or lines[look].startswith("    ")):
                block.append(lines[look].rstrip())
                consumed.add(look)
                look += 1
            add(primary, "sanitizer", stripped, block)
            continue

        if TRACEBACK.match(stripped):
            frames: list[str] = []
            look = index + 1
            while look < count and (lines[look].startswith((" ", "\t")) or not lines[look].strip()):
                if lines[look].strip():
                    frames.append(lines[look].rstrip())
                consumed.add(look)
                look += 1
            final = lines[look].strip() if look < count else ""
            if final:
                consumed.add(look)
            block = [stripped] + frames[-4:] + ([final] if final else [])
            add(primary, "python", final or stripped + "|" + "|".join(frames[-2:]), block)
            continue

        if PY_EXCEPTION.match(stripped) or MAKE.match(stripped) or NODE.search(stripped):
            add(primary, "tool", stripped, [stripped])
            continue

        if CHECKER.search(stripped):
            block = [stripped]
            look = index + 1
            while look < count and len(block) < 12 and CHECKER_ITEM.match(lines[look]):
                block.append(lines[look].rstrip())
                consumed.add(look)
                look += 1
            add(primary, "check", stripped, block)
            continue

        if XMAKE.match(stripped) or GENERIC.search(stripped):
            add(secondary, "other", clip(stripped, 200), [clip(stripped, 240)])

    return primary, secondary, duplicates


def escape_data(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def escape_property(text: str) -> str:
    return escape_data(text).replace(":", "%3A").replace(",", "%2C")


def annotation(level: str, title: str, message: str) -> str:
    return f"::{level} title={escape_property(title)}::{escape_data(message)}"


def chunk_blocks(blocks: list[list[str]], limit: int, max_chunks: int) -> tuple[list[str], int]:
    """Packs whole diagnostics into chunks; returns (chunks, number of diagnostics that did not fit)."""
    chunks: list[str] = []
    current: list[str] = []
    size = 0
    placed = 0
    for block in blocks:
        text = "\n".join(block)
        if len(text) > limit:
            text = text[: limit - 20] + "\n    ... (cut)"
        if size + len(text) + 1 > limit and current:
            chunks.append("\n".join(current))
            current, size = [], 0
            if len(chunks) == max_chunks:
                break
        current.append(text)
        size += len(text) + 1
        placed += 1
    else:
        if current:
            chunks.append("\n".join(current))
    if len(chunks) > max_chunks:
        chunks = chunks[:max_chunks]
    return chunks, len(blocks) - placed


def step_title(step: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"[^\w .\-\[\]/()+]", " ", step)).strip()[:60] or "step"


def report_failure(raw_lines: list[str], step: str, exit_code: str, workspace: str, max_chunks: int = MAX_ERROR_CHUNKS) -> list[str]:
    primary, secondary, duplicates = extract(raw_lines, workspace)
    name = step_title(step)
    out: list[str] = []

    by_kind: dict[str, int] = {}
    for diagnostic in primary:
        by_kind[diagnostic.kind] = by_kind.get(diagnostic.kind, 0) + 1
    files: dict[str, int] = {}
    for diagnostic in primary:
        if diagnostic.kind == "compile":
            path = re.split(r"[(:]", diagnostic.lines[0], maxsplit=1)[0]
            files[path] = files.get(path, 0) + 1
    summary = [f"{name}: exit code {exit_code}" if exit_code else f"{name}: failed"]
    if by_kind:
        summary.append("found: " + ", ".join(f"{count} {kind}" for kind, count in sorted(by_kind.items())))
    if duplicates:
        summary.append(f"{duplicates} repeated diagnostic(s) dropped")
    if files:
        top = sorted(files.items(), key=lambda item: (-item[1], item[0]))[:6]
        summary.append("files: " + ", ".join(f"{path} x{count}" for path, count in top))
    if not primary:
        summary.append("no compiler, linker, test or check diagnostics recognised; see DIAG_LOG_TAIL")
    # Titles stay plain words: the step name goes into the message, where nothing is parsed.
    out.append(annotation("warning", "DIAG_SUMMARY", " | ".join(summary)))

    blocks = [d.lines for d in primary]
    if len(blocks) < 6:
        # Little that is specific: add the generic error-looking lines, they may be all there is.
        blocks += [d.lines for d in secondary[:40]]
    chunks, left_out = chunk_blocks(blocks, CHUNK_CHARS, max(1, min(max_chunks, MAX_ERROR_CHUNKS)))
    for number, chunk in enumerate(chunks, 1):
        if number == len(chunks) and left_out:
            chunk += f"\n... +{left_out} more diagnostic(s) not shown"
        out.append(annotation("error", f"DIAG_ERRORS_{number}_OF_{len(chunks)}", f"{name}\n{chunk}"))

    tail = [clip(shorten_paths(line, workspace).replace("\\", "/"), 240) for line in raw_lines if line.strip()][-TAIL_LINES:]
    tail_text = "\n".join(tail)
    if len(tail_text) > CHUNK_CHARS:
        tail_text = "... " + tail_text[-(CHUNK_CHARS - 4):]
    if tail_text:
        out.append(annotation("warning", "DIAG_LOG_TAIL", f"{name}\n{tail_text}"))
    return out


def result_lines(raw_lines: list[str]) -> list[str]:
    results = []
    for line in raw_lines:
        text = line.strip()
        if RESULT_LINE.search(text):
            if text.startswith(RESULT_MARKER):
                text = text[len(RESULT_MARKER):]
            results.append(clip(text, 300))
    return results[-8:]


def report_success(raw_lines: list[str], step: str) -> list[str]:
    results = result_lines(raw_lines)
    message = f"OK: {step_title(step)}"
    if results:
        message += " | " + " | ".join(results)
    return [annotation("notice", "RFAB_CI", message)]


# --------------------------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------------------------
MSVC_CASCADE = r"""
[ 41%]: compiling.releasedbg Code\client\Services\RfabAdapterService.cpp
error: D:\a\RFABTogether\RFABTogether\Code\client\Services\RfabAdapterService.cpp(548,13): error C2039: 'ForceActorValue': is not a member of 'PlayerCharacter'
D:\a\RFABTogether\RFABTogether\Code\client\Games\Skyrim\PlayerCharacter.h(12): note: see declaration of 'PlayerCharacter'
D:\a\RFABTogether\RFABTogether\Code\encoding\Structs/ActionEvent.h(37): error C2039: 'Reader': is not a member of 'TiltedPhoques'
C:\Users\runneradmin\AppData\Local\.xmake\packages\t\tiltedcore\v0.2.9\a984d563f9e448bfa7c6d9b92f93e32d\include\TiltedCore/Stl.hpp(15): note: see declaration of 'TiltedPhoques'
D:\a\RFABTogether\RFABTogether\Code\encoding\Structs/ActionEvent.h(37): error C2061: syntax error: identifier 'Reader'
D:\a\RFABTogether\RFABTogether\Code\encoding\Structs/ActionEvent.h(37): error C2039: 'Reader': is not a member of 'TiltedPhoques'
D:\a\RFABTogether\RFABTogether\Code\encoding\Structs/ActionEvent.h(37): error C2061: syntax error: identifier 'Reader'
LINK : fatal error LNK1120: 1 unresolved externals
error: execv(C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC\14.44.35207\bin\HostX64\x64\cl.exe -nologo -c -MT /EHsc -ID:\a\RFABTogether\RFABTogether\Code\client) failed(2)
"""

CATCH_WINDOWS = r"""
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
TPTests.exe is a Catch v2.13.9 host application.
Run with -? for options

-------------------------------------------------------------------------------
v4.1.0 lifecycle: every participant class implements Session::IParticipant
-------------------------------------------------------------------------------
Code\tests\rfab_v410_session_lifecycle.cpp(672)
...............................................................................

Code\tests\rfab_v410_session_lifecycle.cpp(690): FAILED:
  REQUIRE( missing.empty() )
with expansion:
  false

===============================================================================
test cases:     248 |     247 passed | 1 failed
assertions: 4482879 | 4482878 passed | 1 failed
"""

GCC_AND_HARNESS = """
==> building core tests (-j2)
/home/runner/work/RFABTogether/RFABTogether/Code/tests/a.cpp:12:5: error: 'foo' was not declared in this scope
   12 |     foo();
      |     ^~~
/usr/bin/ld: a.o: undefined reference to `Bar::Baz()'
collect2: error: ld returned 1 exit status
make: *** [Makefile:88: build/tests] Error 1
  ok   Player 1 joins
  FAIL Mount lock is released on dismount
FAILED (1 failures)
"""

PYTHON_AND_CHECKS = """
Traceback (most recent call last):
  File "Tools/rfab/verify_release_bundle.py", line 40, in <module>
    main()
  File "Tools/rfab/verify_release_bundle.py", line 31, in main
    assert bom, "FATAL: Build-CloudRelease.ps1 is missing UTF-8 BOM"
AssertionError: FATAL: Build-CloudRelease.ps1 is missing UTF-8 BOM
[release-invariants] FAILED (2):
  - VERSION must be semver, got 'x'
  - RfabModpackCompat.h kRfabOnlineVersion ('4.1.0') disagrees with VERSION ('4.1.1')
[wire-contract] NEW      W1 Ping | ClientMessage is never constructed under Code/client
hook discipline: 0 violation(s)
==9==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000011
    #0 0x55d1 in Decode Code/encoding/Structs/AnimationVariables.cpp:88
    #1 0x55d2 in main Code/tests/x.cpp:5
SUMMARY: AddressSanitizer: heap-buffer-overflow Code/encoding/Structs/AnimationVariables.cpp:88 in Decode
"""


def self_test() -> int:
    failures: list[str] = []

    def expect(condition: bool, what: str) -> None:
        if not condition:
            failures.append(what)

    def messages(annotations: list[str], level: str) -> list[str]:
        """Decoded messages of one level, without the leading step-name line of error chunks."""
        prefix = f"::{level} "
        decoded = [a.split("::", 2)[2].replace("%0A", "\n").replace("%25", "%") for a in annotations if a.startswith(prefix)]
        if level == "error":
            decoded = [m.split("\n", 1)[1] if "\n" in m else "" for m in decoded]
        return decoded

    # 1. A compiler cascade: the cause comes first, repeats are dropped, runner paths are gone.
    out = report_failure(MSVC_CASCADE.split("\n"), "[4/8] xmake build (Client + Launcher + Server + Tests)", "-1", "")
    errors = messages(out, "error")
    expect(len(errors) == 1, f"cascade: expected one error chunk, got {len(errors)}")
    body = errors[0] if errors else ""
    first = body.split("\n")[0] if body else ""
    expect(first == "Code/client/Services/RfabAdapterService.cpp(548): error C2039: 'ForceActorValue': is not a member of 'PlayerCharacter'", f"cascade: first line is {first!r}")
    expect("note: see declaration of 'PlayerCharacter'" in body, "cascade: the note of the first error is missing")
    expect(body.count("error C2061") == 1, "cascade: a repeated error was not dropped")
    expect("D:/a/" not in body and "D:\\a\\" not in body, "cascade: runner checkout path survived")
    expect("<pkg tiltedcore v0.2.9>/include/TiltedCore/Stl.hpp(15)" in body, "cascade: xmake package path was not shortened")
    expect("LNK1120" in body, "cascade: linker error missing")
    summary = messages(out, "warning")[0] if messages(out, "warning") else ""
    expect("exit code -1" in summary and "2 repeated" in summary and "Code/encoding/Structs/ActionEvent.h x2" in summary, f"cascade: summary is {summary!r}")
    expect(all(len(a) < 4200 for a in out), "cascade: an annotation exceeds the size GitHub keeps")

    # 2. A failed assertion keeps its test name and its expansion.
    out = report_failure(CATCH_WINDOWS.split("\n"), "[5/8] Native Windows x64 Unit Tests (TPTests.exe)", "1", "")
    body = "\n".join(messages(out, "error"))
    expect("[test] v4.1.0 lifecycle: every participant class implements Session::IParticipant" in body, "catch: test name missing")
    expect("REQUIRE( missing.empty() )" in body and "with expansion:" in body and "false" in body, "catch: assertion or expansion missing")
    expect("assertions: 4482879 | 4482878 passed | 1 failed" in body, "catch: totals missing")

    # 3. gcc, the linker, make and the server harness.
    out = report_failure(GCC_AND_HARNESS.split("\n"), "core tests", "2", "")
    body = "\n".join(messages(out, "error"))
    expect(body.startswith("Code/tests/a.cpp:12:5: error: 'foo' was not declared in this scope"), f"gcc: starts with {body[:80]!r}")
    expect("12 |     foo();" in body, "gcc: source excerpt missing")
    expect("undefined reference to `Bar::Baz()'" in body and "make: *** [Makefile:88: build/tests] Error 1" in body, "gcc: linker or make line missing")
    expect("FAIL Mount lock is released on dismount" in body and "FAILED (1 failures)" in body, "harness: failure lines missing")
    expect("ok   Player 1 joins" not in body, "harness: a passing check was reported as an error")

    # 4. Tracebacks, source checks, sanitizers.
    out = report_failure(PYTHON_AND_CHECKS.split("\n"), "static checks", "1", "")
    body = "\n".join(messages(out, "error"))
    expect("AssertionError: FATAL: Build-CloudRelease.ps1 is missing UTF-8 BOM" in body and "line 31, in main" in body, "python: traceback tail missing")
    expect("[release-invariants] FAILED (2):" in body and "disagrees with VERSION ('4.1.1')" in body, "checks: release invariants items missing")
    expect("[wire-contract] NEW      W1 Ping" in body, "checks: wire contract line missing")
    expect("0 violation(s)" not in body, "checks: a clean check was reported as an error")
    expect("ERROR: AddressSanitizer: heap-buffer-overflow" in body and "#0 0x55d1 in Decode" in body, "sanitizer: report or stack missing")

    # 5. A flood stays bounded and says how much it left out.
    flood = [f"D:\\a\\r\\r\\Code\\client\\File{n}.cpp({n}): error C2065: 'x{n}': undeclared identifier" for n in range(5000)]
    out = report_failure(flood, "build", "1", "")
    errors = messages(out, "error")
    expect(len(errors) == MAX_ERROR_CHUNKS, f"flood: {len(errors)} error chunks")
    expect(errors and errors[0].startswith("Code/client/File0.cpp(0)"), "flood: the first error is not first")
    expect(errors and "more diagnostic(s) not shown" in errors[-1], "flood: no marker for what was left out")
    expect(all(len(a) < 4200 for a in out), "flood: an annotation exceeds the size GitHub keeps")
    # Several failing commands inside one job step share its ten error annotations.
    shared = messages(report_failure(flood, "build", "1", "", 2), "error")
    expect(len(shared) == 2 and "more diagnostic(s) not shown" in shared[-1], f"flood: --max-chunks 2 gave {len(shared)} chunks")

    # 6. Nothing recognisable: the tail is still reported, and workflow-command characters are escaped.
    out = report_failure(["step one", "100% done\rprogress", "something went sideways"], "odd: step, name", "3", "")
    expect(any(a.startswith("::warning title=DIAG_LOG_TAIL::odd step name%0A") for a in out), f"plain: tail annotation missing in {out}")
    expect(all(re.match(r"^::(?:error|warning|notice) title=[A-Z0-9_]+::", a) for a in out), f"plain: a title is not a plain word in {out}")
    expect(all("\n" not in a and "\r" not in a for a in out), "plain: raw newline in a workflow command")
    expect(any("100%25 done" in a for a in out), "plain: percent sign not escaped")

    # 7. Success mode reports the result lines.
    marked = result_lines(["noise", "[RFAB_RESULT] symbols SkyrimSE: 212345 functions", "All tests passed (4494898 assertions in 273 test cases)", "more noise"])
    expect(marked == ["symbols SkyrimSE: 212345 functions", "All tests passed (4494898 assertions in 273 test cases)"], f"success: marked result lines: {marked}")
    out = report_success(["==> running core tests", "Passed all 248 test cases with 4488389 assertions.", ""], "core tests")
    expect(out == ["::notice title=RFAB_CI::OK: core tests | Passed all 248 test cases with 4488389 assertions."], f"ok: {out}")

    if failures:
        print("ci diagnostics self-test FAILED:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("ci diagnostics self-test: OK (compiler cascade, test failure, gcc/harness, checks, flood, escaping, success)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--log")
    parser.add_argument("--step", default="step")
    parser.add_argument("--exit-code", default="")
    parser.add_argument("--ok", action="store_true", help="the step succeeded: print one notice with its result lines")
    parser.add_argument("--results-only", action="store_true",
                        help="print the result lines of a successful step as plain text joined by ' | ' (for a caller that writes its own notice)")
    parser.add_argument("--workspace", default=os.environ.get("GITHUB_WORKSPACE", ""))
    parser.add_argument("--max-chunks", type=int, default=MAX_ERROR_CHUNKS,
                        help="error annotations this step may use (a job step shows at most ten per level, "
                             "so several failing commands inside one step have to share them)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    try:
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        if not args.log:
            raise ValueError("--log is required")
        raw_lines = read_log(args.log)
        if args.results_only:
            print(" | ".join(result_lines(raw_lines)), flush=True)
            return 0
        lines = report_success(raw_lines, args.step) if args.ok else report_failure(raw_lines, args.step, args.exit_code, args.workspace, args.max_chunks)
        for line in lines:
            print(line, flush=True)
    except Exception as error:  # noqa: BLE001 - this script must never fail the step it reports on
        print(annotation("warning", "DIAG_INTERNAL", f"ci_diagnostics.py could not read {args.log!r}: {error!r}"), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
