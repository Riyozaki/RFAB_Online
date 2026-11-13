#!/usr/bin/env python3
"""Hook discipline: engine threads only read snapshots and post (architecture step 3).

Skyrim calls our engine hooks on whatever thread it uses - Havok/AI job workers, Papyrus VM threads,
the CEF IPC thread, the save/load path. Client state (the entt registry, the services, the transport,
the event dispatcher) belongs to the main thread. Every crash class this project hit in hooks was the
same mistake: a hook reached into that state directly.

This checker reads the client sources and finds every entry point an engine thread can run:

  hook     a TP_HOOK / TP_HOOK_IMMEDIATE detour, or any TP_MAKE_THISCALL function (call-site patches)
  papyrus  a native bound into the Papyrus VM (Games/PapyrusFunctions.cpp)
  sink     a BSTEventSink<...>::OnEvent implementation
  cef      an OverlayClient CEF message handler (OnProcessMessageReceived, Process*)

and requires that, outside the work it posts to the main thread, an entry point does not

  * trigger the entt dispatcher (GetDispatcher, m_dispatcher, .trigger, .enqueue),
  * use the transport (GetTransport, m_transport, .Send),
  * touch the registry (World::Get()/m_world .view/.get/.try_get/.emplace/.ctx/...),
  * call a service method that is not declared RFAB_ANY_THREAD (Code/client/ThreadAffinity.h),
    directly or through a `auto& x = World::Get().GetFooService();` alias,
  * call a World member other than the thread-safe ones,
  * call a main-thread-only helper (OverlayClient::SetUIVisible).

Posted work is excluded: the arguments of GetRunner().Queue/Trigger and QueueMainThreadTask, the
body of `if (World::IsMainThread() && ...) {...}`, and the rest of a function after
`if (!World::IsMainThread()) { <post>; return; }`.

The bodies of RFAB_ANY_THREAD functions are held to the same rules, so annotating a method is a
claim this checker verifies (one level deep: the functions they call are trusted).

A few entry points are synchronous engine gates that must answer on the spot (may this save run?)
and cannot post. They are main-thread by engine contract; they are listed in MAIN_THREAD_ENTRIES
with the reason, and must call RFAB_EXPECT_MAIN_THREAD so a wrong assumption shows up in the log.

Usage:  check_hook_discipline.py [--root DIR] [--verbose]
        check_hook_discipline.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# ---------------------------------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------------------------------

# (file relative to Code/client, entry name) -> why it is known to run on the main thread.
MAIN_THREAD_ENTRIES: dict[tuple[str, str], str] = {
    ("Games/Skyrim/SaveLoad.cpp", "HookLoadEntry"):
        "BGSSaveLoadManager runs its load path from ProcessEvents in the main loop",
    ("Games/Skyrim/SaveLoad.cpp", "HookLoadCallSite"):
        "the engine's load call inside BGSSaveLoadManager::ProcessEvents (main loop)",
    ("Games/Skyrim/SaveLoad.cpp", "HookSaveImpl"):
        "BGSSaveLoadManager::ProcessEvents (main loop) and SkyrimSoulsRE's SaveHandler on the main thread",
    ("Games/Skyrim/SaveLoad.cpp", "HookSaveGame2"):
        "autosave path of BGSSaveLoadManager::ProcessEvents (main loop)",
    ("Games/Skyrim/SaveLoad.cpp", "HookLoadImpl"):
        "BGSSaveLoadManager::ProcessEvents (main loop)",
    ("Games/Skyrim/SaveLoad.cpp", "HookLoadMostRecent"):
        "menus and the death screen, main thread",
    ("Games/Skyrim/SaveLoad.cpp", "HookQueueSaveLoadTask"):
        "input handlers, menus and non-thread-safe Papyrus natives (Game.RequestSave), all main thread",
    ("Games/Skyrim/SaveLoad.cpp", "HookProcessEvents"):
        "BGSSaveLoadManager::ProcessEvents is called by the main loop",
    ("Games/References.cpp", "HookSetCurrentPickREFR"):
        "console reference picking, main thread (UI)",
    ("Services/Generic/DiscoveryService.cpp", "DiscoveryService::OnEvent"):
        "TESLoadGameEvent is sent by the load path at the end of BGSSaveLoadManager::ProcessEvents",
    ("Games/Skyrim/BSGraphics/BSGraphicsRenderer.cpp", "Hook_Renderer_Init"):
        "D3D11 device creation during startup, on the thread that then runs the main loop",
}

# Entry points that ARE the main thread: they drive the client and record its thread id.
THREAD_DEFINING_ENTRIES: dict[tuple[str, str], str] = {
    ("SkyrimVM64.cpp", "HookMainLoop"): "Main::Update (WinMain loop): drives World::Update and records the main thread",
}

# World members an engine thread may call (everything else on World::Get() is main-thread only).
WORLD_ANY_THREAD_MEMBERS = {
    "GetTick",               # falls back to the published tick off the main thread
    "QueueMainThreadTask",   # the always-lane post
    "GetMainThreadQueue",    # counters only (const)
}

# Getters whose result may be used off-thread with the methods listed (None: see the annotation).
WORLD_GETTER_POLICY: dict[str, set[str] | None] = {
    "Runner": {"Queue", "Trigger"},
    "Lifecycle": None,  # methods ending in AnyThread, see allowed_call()
}

# Helpers that must only run on the main thread even though they are members of an entry's class.
MAIN_THREAD_ONLY_CALLS = {
    "Services/Generic/OverlayClient.cpp": {"SetUIVisible"},
}

POST_CALLS = [
    r"\bGetRunner\s*\(\s*\)\s*\.\s*(?:Queue|Trigger)\s*(?:<[^<>()]*>)?\s*\(",
    r"\bm_runner\s*\.\s*(?:Queue|Trigger)\s*(?:<[^<>()]*>)?\s*\(",
    r"\bQueueMainThreadTask\s*\(",
]

REGISTRY_METHODS = (
    "view|group|try_get|get|get_or_emplace|emplace|emplace_or_replace|replace|patch|remove|erase|"
    "destroy|create|ctx|valid|any_of|all_of|clear|storage|each"
)

FORBIDDEN = [
    ("dispatcher", re.compile(r"\bGetDispatcher\s*\(|\bm_dispatcher\b|\.\s*trigger\s*[<(]|\.\s*enqueue\s*[<(]")),
    ("transport", re.compile(r"\bGetTransport\s*\(|\bm_transport\b|\.\s*Send\s*\(")),
    ("registry", re.compile(r"(?:World::Get\s*\(\s*\)|\bm_world)\s*\.\s*(?:" + REGISTRY_METHODS + r")\b\s*[<(]")),
]

SERVICE_CALL = re.compile(r"(?:World::Get\s*\(\s*\)|\bm_world)\s*\.\s*Get(\w+)\s*\(\s*\)\s*(?:\.|->)\s*(\w+)\s*(?:<[^<>()]*>)?\s*\(")
WORLD_MEMBER_CALL = re.compile(r"(?:World::Get\s*\(\s*\)|\bm_world)\s*\.\s*(\w+)\s*(?:<[^<>()]*>)?\s*\(")
ALIAS = re.compile(
    r"(?:const\s+)?(?:auto|[A-Z]\w*)\s*[&*]\s*(\w+)\s*=\s*&?\s*(?:World::Get\s*\(\s*\)|\bm_world)\s*\.\s*Get(\w+)\s*\(\s*\)\s*;"
)

# Parser rot guard: if the tree suddenly yields fewer entries, the parser broke, not the code.
MIN_ENTRIES = {"hook": 60, "papyrus": 2, "sink": 5, "cef": 8}

# ---------------------------------------------------------------------------------------------------
# C++ scanning helpers
# ---------------------------------------------------------------------------------------------------


def strip_code(text: str) -> str:
    """Blanks comments and the contents of string/char literals, keeping every newline and offset."""
    out = list(text)
    i, n = 0, len(text)

    def blank(a: int, b: int) -> None:
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            i = j
        elif c == "R" and i + 1 < n and text[i + 1] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            m = re.match(r'R"([^(\s]{0,16})\(', text[i:])
            if not m:
                i += 1
                continue
            end = text.find(")" + m.group(1) + '"', i + m.end())
            end = n if end < 0 else end + len(m.group(1)) + 2
            blank(i + 2, end - 1)
            i = end
        elif c in "\"'":
            # a digit separator (1'000) is not a char literal
            if c == "'" and i > 0 and text[i - 1].isalnum() and i + 1 < n and text[i + 1].isalnum():
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            blank(i + 1, min(j, n))
            i = j + 1
        else:
            i += 1
    return "".join(out)


def match_close(s: str, open_at: int, op: str, cl: str) -> int:
    depth = 0
    for k in range(open_at, len(s)):
        if s[k] == op:
            depth += 1
        elif s[k] == cl:
            depth -= 1
            if depth == 0:
                return k
    return -1


def line_of(s: str, offset: int) -> int:
    return s.count("\n", 0, offset) + 1


def strip_posted(body: str) -> str:
    """Replaces the argument list of every post call by POSTED: that code runs on the main thread."""
    out = body
    for pattern in POST_CALLS:
        rx = re.compile(pattern)
        while True:
            m = rx.search(out)
            if not m:
                break
            close = match_close(out, m.end() - 1, "(", ")")
            if close < 0:
                break
            out = out[: m.start()] + "POSTED" + out[close + 1 :]
    return out


MAIN_THREAD_TEST = re.compile(r"(?:\bWorld::|\b)IsMainThread\s*\(\s*\)")


def strip_main_thread_branches(body: str) -> str:
    """Removes code that provably runs on the main thread (see module docstring)."""
    out = body
    pos = 0
    while True:
        m = re.compile(r"\bif\s*\(").search(out, pos)
        if not m:
            return out
        cond_open = m.end() - 1
        cond_close = match_close(out, cond_open, "(", ")")
        if cond_close < 0:
            return out
        cond = out[cond_open + 1 : cond_close]
        k = cond_close + 1
        while k < len(out) and out[k].isspace():
            k += 1
        if k >= len(out) or out[k] != "{" or not MAIN_THREAD_TEST.search(cond):
            pos = m.end()
            continue
        block_close = match_close(out, k, "{", "}")
        if block_close < 0:
            return out
        block = out[k : block_close + 1]
        negated = re.fullmatch(r"\s*!\s*(?:World::)?IsMainThread\s*\(\s*\)\s*", cond) is not None
        if negated:
            if "POSTED" in block and re.search(r"\breturn\b", block):
                # self-marshalling: everything after the guard runs on the main thread
                end = out.rfind("}")
                if end > block_close:
                    out = out[: block_close + 1] + "\n" * out.count("\n", block_close + 1, end) + out[end:]
                return out
            pos = m.end()
            continue
        conjunctive = "||" not in cond and re.search(r"!\s*(?:World::)?IsMainThread", cond) is None
        if conjunctive:
            out = out[:k] + "{" + "\n" * block.count("\n") + "}" + out[block_close + 1 :]
        pos = m.end()


@dataclass
class Entry:
    file: str
    kind: str
    name: str
    line: int
    body: str


@dataclass
class AnyThreadMethod:
    cls: str
    name: str
    decl_file: str
    decl_line: int
    bodies: list[tuple[str, int, str]] = field(default_factory=list)


DEFINITION_TAIL = re.compile(r"\s*(?:const\s*)?(?:noexcept\s*(?:\([^)]*\)\s*)?)?(?:override\s*|final\s*)*(?:->\s*[\w:<>*&\s]+?)?\{")


def definition_body(s: str, open_paren: int) -> tuple[int, int] | None:
    """The body of `name(...) <qualifiers> {...}` given the offset of its '(' - None for a call."""
    close = match_close(s, open_paren, "(", ")")
    if close < 0:
        return None
    m = DEFINITION_TAIL.match(s, close + 1)
    if not m:
        return None
    brace = m.end() - 1
    end = match_close(s, brace, "{", "}")
    return (brace, end) if end > 0 else None


def enclosing_class(s: str, pos: int) -> str | None:
    """The innermost class/struct whose body contains `pos`."""
    best = None
    for c in re.finditer(r"\b(?:class|struct)\s+(\w+)(?:\s+final)?\s*(?::[^;{]*)?\{", s[:pos]):
        brace = c.end() - 1
        end = match_close(s, brace, "{", "}")
        if end < 0 or end > pos:
            best = c.group(1)
    return best


def find_body(s: str, after: int) -> tuple[int, int] | None:
    """The {...} of a function definition that starts at `after`, or None for a declaration."""
    brace = s.find("{", after)
    semi = s.find(";", after)
    if brace < 0 or (0 <= semi < brace):
        return None
    close = match_close(s, brace, "{", "}")
    return (brace, close) if close > 0 else None


def collect_entries(client: Path, sources: dict[str, str]) -> list[Entry]:
    entries: list[Entry] = []
    seen: set[tuple[str, str]] = set()

    def add(f: str, kind: str, name: str, s: str, start: int) -> None:
        span = find_body(s, start)
        if not span or (f, name) in seen:
            return
        seen.add((f, name))
        entries.append(Entry(f, kind, name, line_of(s, start), s[span[0] : span[1] + 1]))

    for f, s in sources.items():
        if not f.endswith(".cpp"):
            continue

        detours = {d for _, d in re.findall(r"\bTP_HOOK(?:_IMMEDIATE)?\s*\(\s*&?\s*(\w+)\s*,\s*&?\s*(\w+)\s*\)", s)}
        detours |= set(re.findall(r"\bTP_MAKE_THISCALL\s*\(\s*(\w+)\s*,", s))
        for name in sorted(detours):
            for m in re.finditer(r"\bTP_MAKE_THISCALL\s*\(\s*" + name + r"\s*,|\b" + name + r"\s*\(", s):
                line_start = s.rfind("\n", 0, m.start()) + 1
                prefix = s[line_start : m.start()]
                if "TP_HOOK" in prefix or re.search(r"[=.>,&(!]\s*$|\breturn\b", prefix):
                    continue
                if find_body(s, m.end()):
                    add(f, "hook", name, s, m.start())
                    break

        if f == "Games/PapyrusFunctions.cpp":
            for m in re.finditer(r"\n[\w:<>*& ]+?\b(\w+)\s*\([^;{)]*\)\s*(?:noexcept\s*)?\{", s):
                add(f, "papyrus", "PapyrusFunctions::" + m.group(1), s, m.start() + 1)

        for m in re.finditer(r"\bBSTEventResult\s+(\w+::OnEvent)\s*\(", s):
            # one class has several sinks: keep them apart by line
            name = f"{m.group(1)}@{line_of(s, m.start())}"
            add(f, "sink", name, s, m.start())

        if f == "Services/Generic/OverlayClient.cpp":
            for m in re.finditer(r"\bOverlayClient::(OnProcessMessageReceived|Process\w+)\s*\(", s):
                add(f, "cef", "OverlayClient::" + m.group(1), s, m.start())
    return entries


def collect_any_thread(sources: dict[str, str]) -> dict[tuple[str, str], AnyThreadMethod]:
    methods: dict[tuple[str, str], AnyThreadMethod] = {}
    for f, s in sources.items():
        if not f.endswith(".h") or f == "ThreadAffinity.h":
            continue
        for m in re.finditer(r"\bRFAB_ANY_THREAD\b", s):
            decl = re.match(r"\s*(?:\[\[\w+\]\]\s*)*(?:static\s+|virtual\s+|inline\s+)*[\w:<>,*&\s]+?\b(\w+)\s*\(", s[m.end() :])
            if not decl:
                continue
            cls = enclosing_class(s, m.start())
            if not cls:
                continue
            name = decl.group(1)
            method = methods.setdefault((cls, name), AnyThreadMethod(cls, name, f, line_of(s, m.start())))
            span = definition_body(s, m.end() + decl.end() - 1)
            if span:
                method.bodies.append((f, line_of(s, m.start()), s[span[0] : span[1] + 1]))

    for (cls, name), method in methods.items():
        for f, s in sources.items():
            if not f.endswith(".cpp"):
                continue
            for m in re.finditer(r"\b" + cls + r"::" + name + r"\s*\(", s):
                span = definition_body(s, m.end() - 1)
                if span:
                    method.bodies.append((f, line_of(s, m.start()), s[span[0] : span[1] + 1]))
    return methods


def check_body(body: str, any_thread: dict[tuple[str, str], AnyThreadMethod], extra_forbidden: set[str]) -> list[str]:
    code = strip_main_thread_branches(strip_posted(body))
    problems: list[str] = []

    def allowed_service(service: str, method: str) -> bool:
        if service in WORLD_GETTER_POLICY:
            policy = WORLD_GETTER_POLICY[service]
            return method.endswith("AnyThread") if policy is None else method in policy
        return (service, method) in any_thread

    for tag, rx in FORBIDDEN:
        for m in rx.finditer(code):
            problems.append(f"{tag}: {' '.join(m.group(0).split())}")

    for m in SERVICE_CALL.finditer(code):
        service, method = m.group(1), m.group(2)
        if not allowed_service(service, method):
            problems.append(f"service: {service}::{method} is not RFAB_ANY_THREAD")

    for m in WORLD_MEMBER_CALL.finditer(code):
        member = m.group(1)
        if member.startswith("Get") and code[m.end() :].lstrip().startswith(")"):
            after = code[m.end() :].lstrip()[1:].lstrip()
            if after.startswith(".") or after.startswith("->") or after.startswith(";"):
                continue  # a getter: its use is judged by SERVICE_CALL / ALIAS
        if member in WORLD_ANY_THREAD_MEMBERS:
            continue
        if re.match(r"(?:" + REGISTRY_METHODS + r")$", member):
            continue  # reported as registry
        problems.append(f"world: World::{member} is main-thread only")

    for alias in ALIAS.finditer(code):
        var, service = alias.group(1), alias.group(2)
        for use in re.finditer(r"\b" + var + r"\s*(?:\.|->)\s*(\w+)\s*(?:<[^<>()]*>)?\s*\(", code):
            if not allowed_service(service, use.group(1)):
                problems.append(f"service: {service}::{use.group(1)} (via {var}) is not RFAB_ANY_THREAD")

    for helper in sorted(extra_forbidden):
        if re.search(r"(?<![\w:])" + helper + r"\s*\(", code):
            problems.append(f"main-thread helper: {helper}() must be posted")
    return sorted(set(problems))


def load_sources(client: Path) -> dict[str, str]:
    sources: dict[str, str] = {}
    for path in sorted(client.rglob("*")):
        if path.suffix in (".cpp", ".h") and path.is_file():
            rel = path.relative_to(client).as_posix()
            sources[rel] = strip_code(path.read_text(encoding="utf-8", errors="replace"))
    return sources


def run(root: Path, verbose: bool, enforce_minimums: bool = True) -> list[str]:
    client = root / "Code" / "client"
    sources = load_sources(client)
    entries = collect_entries(client, sources)
    any_thread = collect_any_thread(sources)
    failures: list[str] = []

    counts = {kind: sum(1 for e in entries if e.kind == kind) for kind in MIN_ENTRIES}
    if enforce_minimums:
        for kind, minimum in MIN_ENTRIES.items():
            if counts[kind] < minimum:
                failures.append(f"parser: only {counts[kind]} {kind} entry points found (expected >= {minimum})")

    listed = set(MAIN_THREAD_ENTRIES) | set(THREAD_DEFINING_ENTRIES)
    for entry in entries:
        key = (entry.file, entry.name.split("@")[0])
        if key in THREAD_DEFINING_ENTRIES:
            listed.discard(key)
            continue
        if key in MAIN_THREAD_ENTRIES:
            listed.discard(key)
            if "RFAB_EXPECT_MAIN_THREAD" not in entry.body:
                failures.append(f"{entry.file}:{entry.line} [{entry.kind}] {entry.name}: main-thread gate without RFAB_EXPECT_MAIN_THREAD")
            continue
        for problem in check_body(entry.body, any_thread, MAIN_THREAD_ONLY_CALLS.get(entry.file, set())):
            failures.append(f"{entry.file}:{entry.line} [{entry.kind}] {entry.name}: {problem}")
    for f, name in sorted(listed):
        failures.append(f"{f}: the main-thread list names {name}, which is no longer an entry point")

    for (cls, name), method in sorted(any_thread.items()):
        if not method.bodies:
            failures.append(f"{method.decl_file}:{method.decl_line} RFAB_ANY_THREAD {cls}::{name}: no definition found")
        for f, line, body in method.bodies:
            for problem in check_body(body, any_thread, set()):
                failures.append(f"{f}:{line} RFAB_ANY_THREAD {cls}::{name}: {problem}")

    for f, s in sources.items():
        if re.search(r"\bTaskQueue\b", s):
            failures.append(f"{f}: TiltedPhoques::TaskQueue runs tasks under its lock; use World's MainThreadQueue")

    if verbose:
        print(f"entry points: {len(entries)} {counts}; RFAB_ANY_THREAD methods: {len(any_thread)}; "
              f"main-thread gates: {len(MAIN_THREAD_ENTRIES)}")
        for entry in entries:
            print(f"  {entry.kind:8} {entry.file}:{entry.line} {entry.name}")
    return failures


# ---------------------------------------------------------------------------------------------------
# Self-test: the rules must catch what they claim to catch
# ---------------------------------------------------------------------------------------------------

SELF_TEST_HEADER = """
#pragma once
#include <ThreadAffinity.h>
class FooService final : public Bar
{
public:
    struct ScopedThing
    {
        ScopedThing() noexcept;
    };

    RFAB_ANY_THREAD [[nodiscard]] bool IsSafe() const noexcept { return m_flag.load(); }
    RFAB_ANY_THREAD [[nodiscard]] GamePtr<Actor> Find(Actor* apActor) const noexcept;
    RFAB_ANY_THREAD void SelfMarshals(int a) noexcept;
    RFAB_ANY_THREAD void Lies() noexcept;
    void MainOnly() noexcept;
};
"""

SELF_TEST_SERVICE = """
void FooService::SelfMarshals(int a) noexcept
{
    if (!World::IsMainThread())
    {
        m_world.GetRunner().Queue([this, a]() { SelfMarshals(a); });
        return;
    }
    m_transport.Send(a);
    auto view = m_world.view<FormIdComponent>();
}

void FooService::Lies() noexcept
{
    m_dispatcher.trigger(Event{});
}

GamePtr<Actor> FooService::Find(Actor* apActor) const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return Select(m_snapshot, apActor);
}

void FooService::MainOnly() noexcept
{
    if (FooService::Find(nullptr)) { m_transport.Send(1); }
}
"""

# name -> (hook body, expected problem substrings; empty = clean)
SELF_TEST_CASES: dict[str, tuple[str, list[str]]] = {
    "CleanPost": ("World::Get().GetRunner().Queue([]() { World::Get().GetTransport().Send(x); });", []),
    "CleanTrigger": ("World::Get().GetRunner().Trigger(HitEvent(a, b));", []),
    "CleanAlwaysLane": ("World::Get().QueueMainThreadTask([]() { World::Get().GetPartyService().CreateParty(); });", []),
    "CleanAnyThread": ("if (World::Get().GetFooService().IsSafe()) return;", []),
    "CleanAlias": ("auto& foo = World::Get().GetFooService(); foo.SelfMarshals(1);", []),
    "CleanGuardedBranch": ("if (World::IsCreated() && World::IsMainThread()) { World::Get().GetFooService().MainOnly(); }", []),
    "CleanSelfMarshal": ("if (!World::IsMainThread()) { World::Get().GetRunner().Queue([]{}); return; } "
                         "World::Get().GetFooService().MainOnly();", []),
    "CleanLifecycle": ("auto t = World::Get().GetLifecycle().GetTokenAnyThread();", []),
    "CleanStringLiteral": ('const char* s = "m_transport.Send( World::Get().view<X>() {"; (void)s;', []),
    "CleanComment": ("// World::Get().GetTransport().Send(x);\n/* m_dispatcher.trigger(e); */", []),
    "BadTransport": ("World::Get().GetTransport().Send(x);", ["transport"]),
    "BadDispatcher": ("World::Get().GetDispatcher().trigger(e);", ["dispatcher"]),
    "BadRegistry": ("auto v = World::Get().view<FormIdComponent>();", ["registry"]),
    "BadService": ("World::Get().GetFooService().MainOnly();", ["FooService::MainOnly"]),
    "BadAlias": ("auto& foo = World::Get().GetFooService(); foo.MainOnly();", ["FooService::MainOnly (via foo)"]),
    "BadRunnerDrain": ("World::Get().GetRunner().OnUpdate(e);", ["Runner::OnUpdate"]),
    "BadLifecycle": ("World::Get().GetLifecycle().IsStillCurrentSession(t);", ["Lifecycle::IsStillCurrentSession"]),
    "BadWorldMember": ("World::Get().Update();", ["World::Update"]),
    "BadOrBranch": ("if (World::IsMainThread() || a) { World::Get().GetTransport().Send(x); }", ["transport"]),
    "BadNegatedNoReturn": ("if (!World::IsMainThread()) { World::Get().GetRunner().Queue([]{}); } "
                           "World::Get().GetTransport().Send(x);", ["transport"]),
    "BadOutsidePost": ("World::Get().GetRunner().Queue([]{}); m_transport.Send(x);", ["transport"]),
    "Gate": ('RFAB_EXPECT_MAIN_THREAD("HookGate"); World::Get().GetTransport().Send(x);', []),
    "GateNoExpect": ("World::Get().GetTransport().Send(x);", ["without RFAB_EXPECT_MAIN_THREAD"]),
}


def self_test() -> list[str]:
    failures: list[str] = []

    # strip_code keeps offsets and newlines
    sample = 'a("x{\\"}");\n// c\nb(\'{\'); R"(})" /* {\n} */ d'
    stripped = strip_code(sample)
    if len(stripped) != len(sample) or stripped.count("\n") != sample.count("\n") or "{" in stripped or "}" in stripped:
        failures.append(f"strip_code: {stripped!r}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        client = root / "Code" / "client"
        (client / "Services" / "Generic").mkdir(parents=True)
        (client / "Games").mkdir(parents=True)
        (client / "Services" / "FooService.h").write_text(SELF_TEST_HEADER)
        (client / "Services" / "Generic" / "FooService.cpp").write_text(SELF_TEST_SERVICE)
        hooks = []
        for name, (body, _) in SELF_TEST_CASES.items():
            hooks.append(f"static void* Real{name} = nullptr;\nvoid TP_MAKE_THISCALL(Hook{name}, Actor, int a)\n{{\n    {body}\n}}\n")
        hooks.append("static TiltedPhoques::Initializer s_init([]() {\n" +
                     "".join(f"    TP_HOOK(&Real{n}, Hook{n});\n" for n in SELF_TEST_CASES) + "});\n")
        (client / "Games" / "Hooks.cpp").write_text("\n".join(hooks))
        (client / "Services" / "Generic" / "OverlayClient.cpp").write_text(
            "void OverlayClient::ProcessReveal()\n{\n    SetUIVisible(false);\n}\n"
            "void OverlayClient::ProcessHide()\n{\n    CefRefPtr<OverlayClient> self(this);\n"
            "    World::Get().QueueMainThreadTask([self]() { self->SetUIVisible(false); });\n}\n"
            "void OverlayClient::SetUIVisible(bool a) noexcept\n{\n    World::Get().GetOverlayService().SetActive(a);\n}\n")

        global MAIN_THREAD_ENTRIES
        global THREAD_DEFINING_ENTRIES
        saved = (MAIN_THREAD_ENTRIES, THREAD_DEFINING_ENTRIES)
        MAIN_THREAD_ENTRIES = {("Games/Hooks.cpp", "HookGate"): "self-test", ("Games/Hooks.cpp", "HookGateNoExpect"): "self-test",
                               ("Games/Hooks.cpp", "HookGone"): "self-test"}
        THREAD_DEFINING_ENTRIES = {}
        try:
            results = run(root, verbose=False, enforce_minimums=False)
        finally:
            MAIN_THREAD_ENTRIES, THREAD_DEFINING_ENTRIES = saved

    for name, (_, expected) in SELF_TEST_CASES.items():
        mine = [r for r in results if f"Hook{name}:" in r]
        if not expected and mine:
            failures.append(f"{name}: expected clean, got {mine}")
        for needle in expected:
            if not any(needle in r for r in mine):
                failures.append(f"{name}: expected a '{needle}' violation, got {mine}")

    def expect(needle: str, present: bool) -> None:
        if any(needle in r for r in results) != present:
            failures.append(f"{'missing' if present else 'unexpected'}: {needle} in {results}")

    expect("OverlayClient::ProcessReveal: main-thread helper: SetUIVisible", True)
    expect("OverlayClient::ProcessHide:", False)
    expect("OverlayClient::SetUIVisible", False)  # not an entry point: only reachable through a post
    expect("RFAB_ANY_THREAD FooService::Lies: dispatcher", True)
    expect("RFAB_ANY_THREAD FooService::SelfMarshals", False)
    expect("RFAB_ANY_THREAD FooService::IsSafe", False)
    expect("RFAB_ANY_THREAD FooService::Find", False)  # template return type, call inside if() is not a definition
    expect("ScopedThing", False)  # a nested class does not capture the outer class's annotations
    expect("names HookGone, which is no longer an entry point", True)
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
        print(f"hook discipline self-test: {len(SELF_TEST_CASES) + 11 - len(failures)} checks, {len(failures)} failed")
        return 1 if failures else 0

    failures = run(args.root, args.verbose)
    for failure in failures:
        print(f"FAIL: {failure}")
    print(f"hook discipline: {len(failures)} violation(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
