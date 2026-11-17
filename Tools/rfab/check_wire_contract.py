#!/usr/bin/env python3
"""Wire contract: every message and every event kind has a sender and a receiver in shipped code.

Why this exists
---------------
The unit suites test a rule in isolation and the server harness plays the client by hand, so both
stay green when one end of a feature was never written. That is how "shared Word Walls" shipped
with a server handler, a checkpoint chunk, a HUD message and tests - and no client code that ever
sends the event (DEFECT_REGISTRY.md, M4).

What is checked (sources only, comments stripped; tests and the harness do not count as shipped code)
---------------------------------------------------------------------------------------------------
  W1  A message type is constructed by its sending side:
        ClientMessage -> somewhere under Code/client, ServerMessage -> somewhere under Code/server
        (a local, a temporary or a container of T; Code/common and the non-message part of
        Code/encoding are compiled into both binaries and count for either side).
  W2  A message type is handled by its receiving side:
        ClientMessage -> `PacketEvent<T>` under Code/server, ServerMessage -> `sink<T>` under
        Code/client, or a raw `m_messageHandlers[T::Opcode]` / `CastUnique<T>` on either side.
  W3  Every enumerator of an enum declared inside a message struct (EventKind and friends) is
      referenced by the sending side and by the receiving side. The enumerator a member defaults
      to in the message header counts as sent only if the sender builds the message somewhere
      without assigning that member; when every construction assigns it, nothing ever sends the
      default (this is how SyncRfabProgressionEvent::LedgerXpGrant went unnoticed).
      An enumerator handled by a final `else` or `default:` is invisible to this rule and has to
      be listed in the baseline with that reason.
      A RESERVED enumerator (the "reserved" list of the baseline) keeps its number on the wire and
      must be referenced by neither side: the moment one side starts using it again, the check
      fails until it is taken off the list and both ends exist.

What is NOT checked: field values. A harness that sends a real message type with a field value no
client ever produces (MountRequest{MountId = 0}, M3) passes this check; that needs a behavioural test.

Known violations live in wire_contract_baseline.json next to this file, each with a reason. The
check fails on a violation that is not listed, and on a listed one that no longer occurs - the list
can only shrink, and never silently.

Usage
-----
  python3 Tools/rfab/check_wire_contract.py              # check the repository
  python3 Tools/rfab/check_wire_contract.py --list       # print every violation, baseline ignored
  python3 Tools/rfab/check_wire_contract.py --self-test  # prove each rule catches its mistake
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = Path(__file__).resolve().with_name("wire_contract_baseline.json")

SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".inl"}

MESSAGE_DECL = re.compile(
    r"\bstruct\s+(?P<name>[A-Za-z_]\w*)\s*(?:final\s*)?:\s*(?:public\s+)?(?P<base>ClientMessage|ServerMessage)\b"
)
ENUM_DECL = re.compile(
    r"\benum\s+(?P<scoped>class\s+|struct\s+)?(?P<name>[A-Za-z_]\w*)\s*(?::\s*[\w:\s]+?)?\s*\{(?P<body>[^{}]*)\}"
)


def strip_comments(text: str) -> str:
    """Drops // and /* */ comments, keeps string and character literals intact."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            end = text.find("\n", i)
            i = n if end == -1 else end
        elif ch == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            # Keep the line count stable so reported positions stay meaningful.
            block = text[i : n if end == -1 else end + 2]
            out.append("\n" * block.count("\n"))
            i = n if end == -1 else end + 2
        elif ch in "\"'":
            quote = ch
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
                if j < n and text[j - 1] == "\n" and quote == "'":
                    break  # a digit separator (1'000), not a character literal
            out.append(text[i : min(j + 1, n)])
            i = min(j + 1, n)
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def read_sources(directory: Path) -> dict[Path, str]:
    sources: dict[Path, str] = {}
    if not directory.is_dir():
        return sources
    for path in sorted(directory.rglob("*")):
        if path.suffix in SOURCE_SUFFIXES and path.is_file():
            sources[path] = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    return sources


class Message:
    def __init__(self, name: str, base: str, header: Path) -> None:
        self.name = name
        self.base = base  # "ClientMessage" | "ServerMessage"
        self.header = header
        self.enums: dict[str, list[str]] = {}
        self.scoped: dict[str, bool] = {}
        self.defaults: set[tuple[str, str]] = set()
        # enum name -> names of the members of that enum type (`EventKind Kind{...}` -> "Kind")
        self.default_members: dict[str, set[str]] = {}

    @property
    def sender(self) -> str:
        return "client" if self.base == "ClientMessage" else "server"

    @property
    def receiver(self) -> str:
        return "server" if self.base == "ClientMessage" else "client"


def parse_messages(messages_dir: Path) -> list[Message]:
    found: list[Message] = []
    for header in sorted(messages_dir.glob("*.h")):
        text = strip_comments(header.read_text(encoding="utf-8", errors="replace"))
        for decl in MESSAGE_DECL.finditer(text):
            message = Message(decl.group("name"), decl.group("base"), header)
            # The struct body runs to the end of the file in every message header; an enum that is
            # declared before the struct belongs to the file, not to the message, and is skipped.
            for enum in ENUM_DECL.finditer(text, decl.end()):
                enumerators = []
                for item in enum.group("body").split(","):
                    ident = item.split("=")[0].strip()
                    if re.fullmatch(r"[A-Za-z_]\w*", ident):
                        enumerators.append(ident)
                if enumerators:
                    name = enum.group("name")
                    message.enums[name] = enumerators
                    message.scoped[name] = bool(enum.group("scoped"))
                    # `EventKind Kind{EventKind::LedgerXpGrant};`  `StatusCode Status = Started;`
                    for value in enumerators:
                        default = re.compile(
                            r"\b" + re.escape(name) + r"\s+([A-Za-z_]\w*)\s*(?:\{|=)\s*(?:" + re.escape(name) + r"\s*::\s*)?"
                            + re.escape(value) + r"\s*[\};]"
                        )
                        declared = default.search(text, decl.end())
                        if declared:
                            message.defaults.add((name, value))
                            message.default_members.setdefault(name, set()).add(declared.group(1))
            found.append(message)
    return found


def constructed(name: str, sources: dict[Path, str]) -> bool:
    patterns = (
        # `T request;`  `T request{};`  `T request = {};`  `T request(args);`
        re.compile(r"(?<![\w:<&*])" + re.escape(name) + r"\s+[A-Za-z_]\w*\s*(?:;|\{|=|\()"),
        # `T{...}`  `T(...)` temporaries, but not `PacketEvent<T>(` or `sink<T>(`
        re.compile(r"(?<![\w:<&*])" + re.escape(name) + r"\s*[\{\(]"),
        re.compile(r"\b(?:make_unique|make_shared|MakeUnique|MakeShared|New)\s*<\s*" + re.escape(name) + r"\s*>"),
        # `Map<Player*, T> messages;` - one message per recipient, default-constructed by operator[]
        re.compile(r"\b(?:Map|Vector|Array|List|map|unordered_map|vector|array|deque|optional)\s*<[^<>;(){}]*\b" + re.escape(name) + r"\s*>"),
    )
    for text in sources.values():
        if name not in text:
            continue
        for pattern in patterns:
            for hit in pattern.finditer(text):
                # `struct T {` / `struct T final : ...` and `friend struct T;` are declarations.
                before = text[max(0, hit.start() - 16) : hit.start()]
                if re.search(r"\b(?:struct|class|enum|friend|using|typedef)\s+$", before):
                    continue
                return True
    return False


def handled(message: Message, sources: dict[Path, str]) -> bool:
    name = re.escape(message.name)
    if message.base == "ClientMessage":
        typed = r"\bPacketEvent\s*<\s*" + name + r"\s*>"
    else:
        typed = r"\bsink\s*<\s*" + name + r"\s*>"
    # The handshake is dispatched by hand on both sides, before any typed event exists.
    raw = r"\bm_messageHandlers\s*\[\s*" + name + r"\s*::\s*Opcode\s*\]|\bCastUnique\s*<\s*" + name + r"\s*>"
    pattern = re.compile(typed + "|" + raw)
    return any(message.name in text and pattern.search(text) for text in sources.values())


def default_is_sent(message: Message, enum: str, sources: dict[Path, str]) -> bool:
    """True if the sender builds the message at least once without assigning the enum member.

    A construction is `T name{...};`, `T name;` or `T name = ...;`. The member counts as assigned
    when `name.Member =` follows within the next 60 lines (one function). A message that is built
    in a way this cannot follow (a temporary, a container element) keeps the benefit of the doubt.
    """
    members = message.default_members.get(enum, set())
    if not members:
        return True
    construction = re.compile(r"\b" + re.escape(message.name) + r"\s+([A-Za-z_]\w*)\s*(?:\{[^;]*\}|=[^;]*)?\s*;")
    seen_any = False
    for text in sources.values():
        if message.name not in text:
            continue
        for built in construction.finditer(text):
            seen_any = True
            variable = built.group(1)
            window = "\n".join(text[built.end():].split("\n", 61)[:60])
            assigned = any(
                re.search(r"\b" + re.escape(variable) + r"\s*\.\s*" + re.escape(member) + r"\s*=[^=]", window) for member in members
            )
            if not assigned:
                return True
    return not seen_any


def enumerator_referenced(message: Message, enum: str, value: str, sources: dict[Path, str]) -> bool:
    msg, enm, val = re.escape(message.name), re.escape(enum), re.escape(value)
    qualified = re.compile(r"\b" + msg + r"\s*::\s*" + enm + r"\s*::\s*" + val + r"\b")
    # An unscoped enum is reachable as `T::Value`.
    through_type = re.compile(r"\b" + msg + r"\s*::\s*" + val + r"\b")
    # `EventKind::X` with no type in front of it, in a file that names the message (a member
    # function of the message). `OtherMessage::EventKind::X` is deliberately not accepted.
    unqualified = re.compile(r"(?<![\w:])" + enm + r"\s*::\s*" + val + r"\b")
    # `using RT = T::Enum;` ... `RT::X`
    alias_decl = re.compile(r"\busing\s+([A-Za-z_]\w*)\s*=\s*" + msg + r"\s*::\s*" + enm + r"\s*;")
    for text in sources.values():
        if value not in text:
            continue
        if qualified.search(text):
            return True
        if message.name not in text:
            continue
        if not message.scoped.get(enum, True) and through_type.search(text):
            return True
        if unqualified.search(text):
            return True
        for alias in alias_decl.findall(text):
            if re.search(r"(?<![\w:])" + re.escape(alias) + r"\s*::\s*" + val + r"\b", text):
                return True
    return False


def find_violations(root: Path) -> list[dict[str, str]]:
    messages = parse_messages(root / "Code" / "encoding" / "Messages")
    side_sources = {
        "client": read_sources(root / "Code" / "client"),
        "server": read_sources(root / "Code" / "server"),
    }
    # Shared code compiled into both binaries can construct or name a message on behalf of either
    # side: Code/common, and Code/encoding outside the message definitions themselves
    # (StringCache::Serialize builds the StringCacheUpdate the server sends).
    shared = read_sources(root / "Code" / "common")
    encoding = root / "Code" / "encoding"
    for path, text in read_sources(encoding).items():
        if path.parent == encoding:
            shared[path] = text
    for side in side_sources:
        side_sources[side] = {**shared, **side_sources[side]}

    violations: list[dict[str, str]] = []

    def report(rule: str, subject: str, detail: str) -> None:
        violations.append({"rule": rule, "subject": subject, "detail": detail})

    for message in messages:
        has_sender = constructed(message.name, side_sources[message.sender])
        has_receiver = handled(message, side_sources[message.receiver])
        if not has_sender:
            report("W1", message.name, f"{message.base} is never constructed under Code/{message.sender}")
        if not has_receiver:
            report("W2", message.name, f"{message.base} has no handler under Code/{message.receiver}")
        for enum, values in message.enums.items():
            for value in values:
                for role, side in (("sender", message.sender), ("receiver", message.receiver)):
                    # A side that does not exist at all is one finding (W1/W2), not one per enumerator.
                    if not (has_sender if role == "sender" else has_receiver):
                        continue
                    if role == "sender" and (enum, value) in message.defaults and default_is_sent(message, enum, side_sources[side]):
                        continue
                    if not enumerator_referenced(message, enum, value, side_sources[side]):
                        report(
                            "W3",
                            f"{message.name}::{enum}::{value}",
                            f"never referenced by the {role} (Code/{side})",
                        )
    return violations


def key_of(violation: dict[str, str]) -> str:
    return f"{violation['rule']} {violation['subject']} | {violation['detail']}"


def load_baseline(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return {f"{e['rule']} {e['subject']} | {e['detail']}": e.get("reason", "") for e in data.get("known", [])}


def load_reserved(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return {e["subject"]: e.get("reason", "") for e in data.get("reserved", [])}


def check(root: Path, baseline_path: Path, list_only: bool, quiet: bool = False) -> int:
    def say(line: str) -> None:
        if not quiet:
            print(line)

    violations = find_violations(root)
    if list_only:
        for violation in violations:
            print(key_of(violation))
        print(f"wire contract: {len(violations)} violation(s), baseline ignored")
        return 0

    baseline = load_baseline(baseline_path)
    reserved = load_reserved(baseline_path)

    # A reserved enumerator is expected to be unreferenced by BOTH sides: exactly two W3 findings.
    reserved_hits: dict[str, int] = {subject: 0 for subject in reserved}
    kept = []
    for violation in violations:
        if violation["rule"] == "W3" and violation["subject"] in reserved:
            reserved_hits[violation["subject"]] += 1
            continue
        kept.append(violation)
    reserved_in_use = sorted(subject for subject, hits in reserved_hits.items() if hits != 2)
    violations = kept

    current = {key_of(v) for v in violations}
    unexpected = sorted(current - baseline.keys())
    stale = sorted(baseline.keys() - current)
    missing_reason = sorted(k for k, reason in baseline.items() if not reason.strip())
    missing_reason += sorted(f"reserved {subject}" for subject, reason in reserved.items() if not reason.strip())

    for subject in reserved_in_use:
        say(f"[wire-contract] RESERVED {subject} is referenced by at least one side (or does not exist): "
            f"a reserved enumerator must be unused by both; take it off the reserved list of {baseline_path.name}")
    for key in unexpected:
        say(f"[wire-contract] NEW      {key}")
    for key in stale:
        say(f"[wire-contract] STALE    {key}  (fixed or renamed: remove it from {baseline_path.name})")
    for key in missing_reason:
        say(f"[wire-contract] NOREASON {key}")

    known = len(current & baseline.keys())
    failed = bool(unexpected or stale or missing_reason or reserved_in_use)
    say(
        f"wire contract: {len(unexpected)} new violation(s), {len(stale)} stale baseline "
        f"entr{'y' if len(stale) == 1 else 'ies'}, {known} known and listed in {baseline_path.name}, "
        f"{len(reserved) - len(reserved_in_use)} reserved enumerator(s) unused by both sides"
    )
    return 1 if failed else 0


# --------------------------------------------------------------------------------------------
# Self-test: a miniature repository per mistake the rules exist for.
# --------------------------------------------------------------------------------------------
def _write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def self_test() -> int:
    failures: list[str] = []

    def expect(name: str, files: dict[str, str], wanted: set[str]) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel, text in files.items():
                _write(root, rel, text)
            got = {f"{v['rule']} {v['subject']}" for v in find_violations(root)}
        if got != wanted:
            failures.append(f"{name}: expected {sorted(wanted)}, got {sorted(got)}")

    ping_h = "struct Ping final : ClientMessage { uint32_t Id{}; };\n"
    pong_h = "struct Pong final : ServerMessage { uint32_t Id{}; };\n"
    kinds_h = (
        "struct Event final : ClientMessage {\n"
        "    enum class Kind : uint8_t { A = 1, B = 2 };\n"
        "    Kind Kind{};\n};\n"
    )
    notify_kinds_h = (
        "struct NotifyEvent final : ServerMessage {\n"
        "    enum class Kind : uint8_t { A = 1, B = 2 };\n"
        "    Kind Kind{};\n};\n"
    )
    client_ok = "void F() { Ping ping; ping.Id = 1; m_transport.Send(ping); }\nvoid G() { m_dispatcher.sink<Pong>().connect<&S::OnPong>(this); }\n"
    server_ok = "void H() { aDispatcher.sink<PacketEvent<Ping>>().connect<&S::OnPing>(this); }\nvoid I() { Pong pong{}; pPlayer->Send(pong); }\n"

    expect(
        "both ends present",
        {
            "Code/encoding/Messages/Ping.h": ping_h,
            "Code/encoding/Messages/Pong.h": pong_h,
            "Code/client/A.cpp": client_ok,
            "Code/server/B.cpp": server_ok,
        },
        set(),
    )
    expect(
        "W1: the client never sends, only a test does",
        {
            "Code/encoding/Messages/Ping.h": ping_h,
            "Code/client/A.cpp": "void G() {}\n",
            "Code/server/B.cpp": "void H() { aDispatcher.sink<PacketEvent<Ping>>().connect<&S::OnPing>(this); }\n",
            "Code/tests/t.cpp": "void T() { Ping ping; }\n",
            "Tools/rfab/linux-server/harness/main.cpp": "void T() { Ping ping; }\n",
        },
        {"W1 Ping"},
    )
    expect(
        "W1: a commented-out send, a handler signature and a declaration are not a sender",
        {
            "Code/encoding/Messages/Ping.h": ping_h,
            "Code/client/A.cpp": "// Ping ping; m_transport.Send(ping);\n/* Ping other{}; */\nvoid OnPing(const Ping& acMessage);\nstruct Ping;\n",
            "Code/server/B.cpp": "void H() { aDispatcher.sink<PacketEvent<Ping>>().connect<&S::OnPing>(this); }\n",
        },
        {"W1 Ping"},
    )
    expect(
        "W2: nobody listens",
        {
            "Code/encoding/Messages/Ping.h": ping_h,
            "Code/encoding/Messages/Pong.h": pong_h,
            "Code/client/A.cpp": "void F() { Ping ping; m_transport.Send(ping); }\n",
            "Code/server/B.cpp": "void I() { Pong pong; pPlayer->Send(pong); }\n",
        },
        {"W2 Ping", "W2 Pong"},
    )
    expect(
        "W3: an event kind the client never produces (the Word Wall case)",
        {
            "Code/encoding/Messages/Event.h": kinds_h,
            "Code/encoding/Messages/NotifyEvent.h": notify_kinds_h,
            "Code/client/A.cpp": (
                "void F() { Event e; e.Kind = Event::Kind::A; m_transport.Send(e); }\n"
                "void G() { m_dispatcher.sink<NotifyEvent>().connect<&S::On>(this); }\n"
                "void On(const NotifyEvent& m) { switch (m.Kind) { case NotifyEvent::Kind::A: case NotifyEvent::Kind::B: break; } }\n"
            ),
            "Code/server/B.cpp": (
                "void H() { aDispatcher.sink<PacketEvent<Event>>().connect<&S::On>(this); }\n"
                "void On(const PacketEvent<Event>& m) { switch (m.Packet.Kind) { case Event::Kind::A: case Event::Kind::B: break; } }\n"
                "void I() { NotifyEvent n; n.Kind = NotifyEvent::Kind::A; n.Kind = NotifyEvent::Kind::B; pPlayer->Send(n); }\n"
            ),
        },
        {"W3 Event::Kind::B"},
    )
    defaulted_h = (
        "struct Event final : ClientMessage {\n"
        "    enum class Kind : uint8_t { A = 0, B = 1 };\n"
        "    Kind Kind{Kind::A};\n};\n"
    )
    defaulted_server = (
        "void H() { aDispatcher.sink<PacketEvent<Event>>().connect<&S::On>(this); }\n"
        "void On(const PacketEvent<Event>& m) { switch (m.Packet.Kind) { case Event::Kind::A: case Event::Kind::B: break; } }\n"
    )
    expect(
        "W3: a default kind is sent when some construction leaves the member alone",
        {
            "Code/encoding/Messages/Event.h": defaulted_h,
            "Code/client/A.cpp": "void F() { Event e; m_transport.Send(e); }\nvoid G() { Event other{}; other.Kind = Event::Kind::B; m_transport.Send(other); }\n",
            "Code/server/B.cpp": defaulted_server,
        },
        set(),
    )
    expect(
        "W3: a default kind that every construction overwrites is never sent (the LedgerXpGrant case)",
        {
            "Code/encoding/Messages/Event.h": defaulted_h,
            "Code/client/A.cpp": "void F() { Event e{}; e.Id = 1; e.Kind = Event::Kind::B; m_transport.Send(e); }\n",
            "Code/server/B.cpp": defaulted_server,
        },
        {"W3 Event::Kind::A"},
    )
    expect(
        "shared code may construct a message for either side",
        {
            "Code/encoding/Messages/Ping.h": ping_h,
            "Code/common/Build.h": "inline Ping MakePing() { Ping ping{}; return ping; }\n",
            "Code/client/A.cpp": "void F() { m_transport.Send(MakePing()); }\n",
            "Code/server/B.cpp": "void H() { aDispatcher.sink<PacketEvent<Ping>>().connect<&S::OnPing>(this); }\n",
        },
        set(),
    )

    # The baseline must fail in both directions: an unlisted violation and a stale entry.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _write(root, "Code/encoding/Messages/Ping.h", ping_h)
        _write(root, "Code/client/A.cpp", "void G() {}\n")
        _write(root, "Code/server/B.cpp", "void H() { aDispatcher.sink<PacketEvent<Ping>>().connect<&S::OnPing>(this); }\n")
        baseline = root / "baseline.json"
        baseline.write_text('{"known": []}', encoding="utf-8")
        if check(root, baseline, list_only=False, quiet=True) != 1:
            failures.append("baseline: an unlisted violation must fail the check")
        baseline.write_text(
            json.dumps({"known": [
                {"rule": "W1", "subject": "Ping", "detail": "ClientMessage is never constructed under Code/client", "reason": "test"},
                {"rule": "W2", "subject": "Gone", "detail": "ClientMessage has no handler under Code/server", "reason": "test"},
            ]}),
            encoding="utf-8",
        )
        if check(root, baseline, list_only=False, quiet=True) != 1:
            failures.append("baseline: a stale entry must fail the check")
        baseline.write_text(
            json.dumps({"known": [
                {"rule": "W1", "subject": "Ping", "detail": "ClientMessage is never constructed under Code/client", "reason": "test"},
            ]}),
            encoding="utf-8",
        )
        if check(root, baseline, list_only=False, quiet=True) != 0:
            failures.append("baseline: a listed violation with a reason must pass")
        baseline.write_text(
            json.dumps({"known": [
                {"rule": "W1", "subject": "Ping", "detail": "ClientMessage is never constructed under Code/client", "reason": " "},
            ]}),
            encoding="utf-8",
        )
        if check(root, baseline, list_only=False, quiet=True) != 1:
            failures.append("baseline: an entry without a reason must fail the check")

    reserved_h = (
        "struct Event final : ClientMessage {\n"
        "    enum class Kind : uint8_t { A = 1, Gone = 2 };\n"
        "    Kind Kind{};\n};\n"
    )
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _write(root, "Code/encoding/Messages/Event.h", reserved_h)
        _write(root, "Code/client/A.cpp", "void F() { Event e; e.Kind = Event::Kind::A; m_transport.Send(e); }\n")
        server = "void H() { aDispatcher.sink<PacketEvent<Event>>().connect<&S::On>(this); }\nvoid On(const PacketEvent<Event>& m) { if (m.Packet.Kind == Event::Kind::A) {} }\n"
        _write(root, "Code/server/B.cpp", server)
        baseline = root / "baseline.json"
        baseline.write_text(json.dumps({"known": [], "reserved": [{"subject": "Event::Kind::Gone", "reason": "kept for the wire"}]}), encoding="utf-8")
        if check(root, baseline, list_only=False, quiet=True) != 0:
            failures.append("reserved: an enumerator unused by both sides and listed as reserved must pass")
        _write(root, "Code/server/B.cpp", server + "void J(const PacketEvent<Event>& m) { if (m.Packet.Kind == Event::Kind::Gone) {} }\n")
        if check(root, baseline, list_only=False, quiet=True) != 1:
            failures.append("reserved: an enumerator one side references again must fail")
        _write(root, "Code/server/B.cpp", server)
        baseline.write_text(json.dumps({"known": [], "reserved": [{"subject": "Event::Kind::Gone", "reason": ""}]}), encoding="utf-8")
        if check(root, baseline, list_only=False, quiet=True) != 1:
            failures.append("reserved: an entry without a reason must fail")

    if failures:
        print("wire contract self-test FAILED:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("wire contract self-test: every rule catches the mistake it exists for")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true", help="print every violation and ignore the baseline")
    parser.add_argument("--self-test", action="store_true", help="run the rules against synthetic trees")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    return check(ROOT, BASELINE, args.list)


if __name__ == "__main__":
    sys.exit(main())
