#!/usr/bin/env python3
"""Actor lifetime: nothing outlives the frame it was read in except an id (work plan 4.1).

The engine frees actors, references, cells and their 3D whenever it likes: a cell unloads, a summon
expires, a save is loaded. A raw pointer to one of them is valid for the call it was obtained in and
no longer. The client therefore keeps *ids* (form id, server id) and looks the object up again when
it needs it. The review for stage 4.1 found no place that breaks this; these rules keep it that way.

  capture   work posted to the main thread (GetRunner().Queue / QueueForThisWorld / Trigger /
            TriggerInSession, m_runner.*, QueueMainThreadTask) must not capture an engine pointer,
            a reference to a local, or everything by default ([&], [=]). It runs in a later frame.
  member    a service, a system or World must not store a pointer to an engine object that the
            engine frees during play (Actor, TESObjectREFR, TESObjectCELL, Projectile, 3D nodes,
            AI processes, ActorExtension), neither directly nor inside a container.
  event     the same for the event structs in Code/client/Events: an event is queued.
  scope     an engine hook (Code/client/Games) posts through Trigger or QueueForThisWorld, never
            through the session-scoped Queue: what a hook sees is the world, and the world can be
            replaced by a load before the work runs (common/Session/MainThreadQueue.h).

Ref-counted holders (GamePtr, NiPointer, BSTSmartPointer, CefRefPtr) are allowed: they keep the
object alive. Code/client/Services/Debug is the developer overlay and is not checked.

Usage:  check_actor_lifetime.py [--root DIR] [--verbose]
        check_actor_lifetime.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Engine objects the engine frees while the game runs. Base forms defined by plugins (TESGlobal,
# BGSListForm, TESNPC, spells, quests) live as long as the process and may be cached.
VOLATILE_TYPES = (
    "Actor", "PlayerCharacter", "TESObjectREFR", "TESObjectCELL", "Projectile", "ActorExtension",
    "ExActor", "ExPlayerCharacter", "NiAVObject", "NiNode", "BSFadeNode", "AIProcess", "MiddleProcess",
    "ExtraDataList", "ActiveEffect", "MagicTarget", "ActorMagicCaster", "CombatController", "BShkbAnimationGraph",
)
VOLATILE = "|".join(VOLATILE_TYPES)

# A raw pointer to a volatile type, possibly inside a container. Smart holders are removed first.
SMART_HOLDER = re.compile(r"\b(?:GamePtr|NiPointer|BSTSmartPointer|CefRefPtr|std::shared_ptr|std::unique_ptr|UniquePtr|SharedPtr)\s*<[^<>]*>")
RAW_VOLATILE_POINTER = re.compile(r"\b(?:struct\s+|const\s+)*(?:" + VOLATILE + r")\b\s*(?:const\s*)?\*")

# Member declaration: `<type> m_name ...;` or `<type> s_name ...;` on one line.
MEMBER = re.compile(r"^\s*(?:mutable\s+|static\s+|inline\s+)*(?P<type>[^;=(){}]+?)\s+(?P<name>[ms]_\w+)\s*(?:\{[^;]*\}|=[^;]*)?;")
# Event field: `<type> Name ...;` inside a struct in Code/client/Events.
FIELD = re.compile(r"^\s*(?P<type>[A-Za-z_][^;=(){}]*?)\s+(?P<name>[A-Za-z_]\w*)\s*(?:\{[^;]*\}|=[^;]*)?;")

POST_CALLS = [
    r"\bGetRunner\s*\(\s*\)\s*\.\s*(?:Queue|QueueForThisWorld|Trigger|TriggerInSession)\s*(?:<[^<>()]*>)?\s*\(",
    r"\bm_runner\s*\.\s*(?:Queue|QueueForThisWorld|Trigger|TriggerInSession)\s*(?:<[^<>()]*>)?\s*\(",
    r"\bQueueMainThreadTask\s*\(",
]
SESSION_SCOPED_POST = re.compile(r"\bGetRunner\s*\(\s*\)\s*\.\s*Queue\s*\(")

# A capture that is a pointer by this code base's naming: pActor, apActor, cpActor.
POINTER_NAME = re.compile(r"^(?:c?a?p[A-Z]\w*)$")
# Captures that are fine although they look like pointers: the service itself.
ALLOWED_CAPTURES = {"this", "*this", "self"}

NOT_CHECKED = ("Services/Debug/",)


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


def match_close(s: str, open_at: int, op: str, cl: str) -> int:
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


def split_top_level(s: str) -> list[str]:
    """Splits a capture list on commas that are not nested in (), {}, <> or []."""
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in "({[<":
            depth += 1
        elif ch in ")}]>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        parts.append(tail)
    return parts


LAMBDA_INTRO = re.compile(r"\[([^\[\]]*)\]\s*(?:\(|mutable\b|noexcept\b|\{)")


def capture_problems(captures: str) -> list[str]:
    problems = []
    for cap in split_top_level(captures):
        if not cap:
            continue
        if cap in ALLOWED_CAPTURES:
            continue
        if cap in ("&", "="):
            problems.append(f"captures everything by default [{cap}]")
            continue
        if "=" in cap:
            name, init = (x.strip() for x in cap.split("=", 1))
            # `p = pActor` or `p = &actor`: the initializer is itself a pointer. `id = pActor->formID`
            # copies a value and is the correct pattern.
            bare = init.lstrip("&").strip()
            if POINTER_NAME.match(bare) or (init.startswith("&") and re.fullmatch(r"\w+", bare)):
                problems.append(f"captures the pointer {init} as {name}")
            continue
        if cap.startswith("&"):
            problems.append(f"captures {cap[1:].strip()} by reference")
            continue
        if POINTER_NAME.match(cap):
            problems.append(f"captures the pointer {cap}")
    return problems


def check_posted_captures(rel: str, s: str) -> list[str]:
    failures = []
    for pattern in POST_CALLS:
        for m in re.finditer(pattern, s):
            close = match_close(s, m.end() - 1, "(", ")")
            if close < 0:
                continue
            args = s[m.end() : close]
            for lam in LAMBDA_INTRO.finditer(args):
                for problem in capture_problems(lam.group(1)):
                    failures.append(f"{rel}:{line_of(s, m.start())} capture: posted work {problem}; capture an id and look it up when the work runs")
    return failures


def volatile_pointer_in(type_text: str) -> str | None:
    cleaned = SMART_HOLDER.sub("HOLDER", type_text)
    m = RAW_VOLATILE_POINTER.search(cleaned)
    return m.group(0).strip() if m else None


def check_members(rel: str, s: str) -> list[str]:
    failures = []
    for number, line in enumerate(s.split("\n"), 1):
        m = MEMBER.match(line)
        if not m:
            continue
        found = volatile_pointer_in(m.group("type"))
        if found:
            failures.append(f"{rel}:{number} member: {m.group('name')} stores {found}; store the form id (or a GamePtr if the object must stay alive)")
    return failures


def check_event_fields(rel: str, s: str) -> list[str]:
    failures = []
    for number, line in enumerate(s.split("\n"), 1):
        m = FIELD.match(line)
        if not m or m.group("type").strip().startswith(("return", "using", "typedef", "friend")):
            continue
        found = volatile_pointer_in(m.group("type"))
        if found:
            failures.append(f"{rel}:{number} event: field {m.group('name')} is {found}; an event is queued, carry the form id")
    return failures


def check_hook_scope(rel: str, s: str) -> list[str]:
    failures = []
    for m in SESSION_SCOPED_POST.finditer(s):
        failures.append(f"{rel}:{line_of(s, m.start())} scope: an engine hook posts with Queue(); use QueueForThisWorld() or Trigger() "
                        "so the work dies with the world it was read in")
    return failures


def run(root: Path, verbose: bool) -> list[str]:
    client = root / "Code" / "client"
    failures: list[str] = []
    counts = {"sources": 0, "posts": 0, "members": 0, "events": 0}
    for path in sorted(client.rglob("*")):
        if path.suffix not in (".cpp", ".h") or not path.is_file():
            continue
        rel = path.relative_to(client).as_posix()
        if any(rel.startswith(skip) for skip in NOT_CHECKED):
            continue
        s = strip_code(path.read_text(encoding="utf-8", errors="replace"))
        counts["sources"] += 1
        counts["posts"] += sum(len(re.findall(p, s)) for p in POST_CALLS)
        failures += check_posted_captures(rel, s)
        if path.suffix == ".h" and (rel.startswith(("Services/", "Systems/")) or rel == "World.h"):
            counts["members"] += sum(1 for line in s.split("\n") if MEMBER.match(line))
            failures += check_members(rel, s)
        if rel.startswith("Events/") and path.suffix == ".h":
            counts["events"] += 1
            failures += check_event_fields(rel, s)
        if rel.startswith("Games/"):
            failures += check_hook_scope(rel, s)
    if verbose:
        print(f"checked {counts['sources']} sources: {counts['posts']} posts, {counts['members']} service members, "
              f"{counts['events']} event headers")
    # Parser rot guard. The audited clean STR import had 47 posts (the removed v4 stack had
    # >60); require >=40 plus the member/event floors, not an obsolete architecture's size.
    if root == REPO_ROOT and (counts["posts"] < 40 or counts["members"] < 150 or counts["events"] < 30):
        failures.append(f"parser: too little found ({counts}); the patterns no longer match the sources")
    return failures


# ---------------------------------------------------------------------------------------------------
# Self-test: every rule must catch its mistake and leave the correct pattern alone
# ---------------------------------------------------------------------------------------------------

SELF_TEST_FILES = {
    "Services/Generic/GoodService.cpp": """
void GoodService::F(Actor* apActor) noexcept
{
    const uint32_t formId = apActor->formID;
    m_world.GetRunner().QueueForThisWorld([this, formId, id = apActor->formID, hours]() noexcept {
        if (Actor* pActor = Cast<Actor>(TESForm::GetById(formId)))
            Use(pActor);   // a pointer looked up inside the work is fine
    });
    World::Get().GetRunner().Trigger(HitEvent(apActor->formID, 0x14));
    World::Get().QueueMainThreadTask([self]() { self->Show(); });
    GamePtr<Actor> spActor(apActor);
    m_world.GetRunner().Queue([spActor]() { spActor->Touch(); });   // ref-counted: stays alive
    std::sort(v.begin(), v.end(), [&](int a, int b) { return a < b; });   // not posted
}
""",
    "Services/Generic/BadService.cpp": """
void BadService::RawPointer(Actor* apActor) noexcept
{
    m_world.GetRunner().Queue([this, apActor]() { apActor->Kill(); });
}
void BadService::LocalPointer() noexcept
{
    Actor* pActor = Find();
    World::Get().GetRunner().QueueForThisWorld([pActor]() noexcept { pActor->Kill(); });
}
void BadService::Renamed(Actor* apActor) noexcept
{
    m_runner.Queue([victim = apActor]() { victim->Kill(); });
}
void BadService::AddressOf() noexcept
{
    Thing local;
    World::Get().QueueMainThreadTask([p = &local]() { p->Go(); });
}
void BadService::ByReference() noexcept
{
    int counter = 0;
    m_world.GetRunner().Queue([&counter]() { ++counter; });
}
void BadService::Everything() noexcept
{
    m_world.GetRunner().Queue([&]() { Go(); });
    m_world.GetRunner().Queue([=]() mutable { Go(); });
}
""",
    "Services/GoodService.h": """
struct GoodService
{
    uint32_t m_targetFormId{0};
    Map<uint32_t, uint64_t> m_lastSeenMs;
    GamePtr<Actor> m_spHeld;
    Vector<GamePtr<TESObjectREFR>> m_held;
    TESGlobal* m_pXpGlobal{nullptr};          // plugin form: lives as long as the process
    struct TESForm* m_pLocation = nullptr;    // compared, never dereferenced
    RfabAdapterService* m_pAdapter{nullptr};
};
""",
    "Services/BadService.h": """
struct BadService
{
    Actor* m_pTarget{nullptr};
    const TESObjectREFR* m_pDoor = nullptr;
    Map<uint32_t, Actor*> m_actorsById;
    std::vector<TESObjectCELL*> m_cells{};
    static NiAVObject* s_pNode;
};
""",
    "Events/GoodEvent.h": """
struct GoodEvent
{
    GoodEvent(Actor* apActor) : FormId(apActor->formID) {}
    uint32_t FormId;
    GameId Target{};
};
""",
    "Events/BadEvent.h": """
struct BadEvent
{
    Actor* pHitter;
    TESObjectREFR* Target{nullptr};
};
""",
    "Games/GoodHook.cpp": """
void TP_MAKE_THISCALL(HookGood, Actor)
{
    World::Get().GetRunner().Trigger(HitEvent(apThis->formID, 0));
    World::Get().GetRunner().QueueForThisWorld([id = apThis->formID]() noexcept { Check(id); });
    World::Get().GetRunner().TriggerInSession(ErrorEvent());
}
""",
    "Games/BadHook.cpp": """
void TP_MAKE_THISCALL(HookBad, Actor)
{
    World::Get().GetRunner().Queue([id = apThis->formID]() noexcept { Check(id); });
}
""",
    "Services/Debug/DebugView.cpp": """
void DebugView::Draw(ActiveEffect* pEffect)
{
    m_world.GetRunner().Queue([pEffect]() { pEffect->fElapsedSeconds = 0.f; });
}
""",
}

SELF_TEST_EXPECT = [
    ("Services/Generic/BadService.cpp:4 capture: posted work captures the pointer apActor", True),
    ("Services/Generic/BadService.cpp:9 capture: posted work captures the pointer pActor", True),
    ("Services/Generic/BadService.cpp:13 capture: posted work captures the pointer apActor as victim", True),
    ("Services/Generic/BadService.cpp:18 capture: posted work captures the pointer &local as p", True),
    ("Services/Generic/BadService.cpp:23 capture: posted work captures counter by reference", True),
    ("Services/Generic/BadService.cpp:27 capture: posted work captures everything by default [&]", True),
    ("Services/Generic/BadService.cpp:28 capture: posted work captures everything by default [=]", True),
    ("Services/BadService.h:4 member: m_pTarget stores Actor*", True),
    ("Services/BadService.h:5 member: m_pDoor stores const TESObjectREFR*", True),
    ("Services/BadService.h:6 member: m_actorsById stores Actor*", True),
    ("Services/BadService.h:7 member: m_cells stores TESObjectCELL*", True),
    ("Services/BadService.h:8 member: s_pNode stores NiAVObject*", True),
    ("Events/BadEvent.h:4 event: field pHitter is Actor*", True),
    ("Events/BadEvent.h:5 event: field Target is TESObjectREFR*", True),
    ("Games/BadHook.cpp:4 scope: an engine hook posts with Queue()", True),
    ("GoodService", False),
    ("GoodEvent", False),
    ("GoodHook", False),
    ("Debug", False),
]


def self_test() -> list[str]:
    failures: list[str] = []
    sample = 'a("x[&]"); // [=]\nb'
    if "[" in strip_code(sample) or len(strip_code(sample)) != len(sample):
        failures.append(f"strip_code: {strip_code(sample)!r}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in SELF_TEST_FILES.items():
            path = root / "Code" / "client" / rel
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
        print(f"actor lifetime self-test: {len(SELF_TEST_EXPECT) + 2 - len(failures)} checks, {len(failures)} failed")
        return 1 if failures else 0

    failures = run(args.root, args.verbose)
    for failure in failures:
        print(f"FAIL: {failure}")
    print(f"actor lifetime: {len(failures)} violation(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
