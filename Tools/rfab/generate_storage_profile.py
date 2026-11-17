#!/usr/bin/env python3
"""Resolve RFAB secure access -> hidden backing from the user's *installed* plugins.

Offline, read-only input: use the virtual Data folder of the selected MO2 profile and an
ordered active plugins.txt (or --plugin in load order). No downloads, guessed FormIDs or
plugin edits. Output the same TSV to the client Data/SKSE/Plugins and server config folders.
The JSON is an audit manifest, not a claim that an RFAB/Bizarre native ABI is supported.

VMAD object formats follow TES5Edit 43652b9932a9e115105fc018a87045bf1fbce083,
Core/wbDefinitionsTES5.pas, wbScriptPropertyObject (v1/v2, including alias/padding).
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from typing import Iterator
import zlib

SCRIPT = "rfab_securechestscript"
PROPERTY = "securechest"
MAX_PLUGIN = 512 * 1024 * 1024
MAX_RECORD = 32 * 1024 * 1024
MAX_RECORDS = 2_000_000
MAX_RULES = 1024


class ProfileError(ValueError):
    pass


def plugin_name(name: str) -> str:
    if not 5 <= len(name) <= 120 or not name.isascii() or name != name.strip():
        raise ProfileError(f"Invalid plugin filename: {name!r}")
    if any(c in name for c in "\\/:|\t\r\n\x00") or name in {".", ".."}:
        raise ProfileError(f"Not a plugin basename: {name!r}")
    if not re.fullmatch(r"[A-Za-z0-9 _.'()+\-]+\.(?:esm|esp|esl)", name, re.IGNORECASE):
        raise ProfileError(f"Unsupported plugin filename: {name!r}")
    return name.lower()


class Reader:
    def __init__(self, data: bytes):
        self.data, self.pos = data, 0

    @property
    def remaining(self) -> int:
        return len(self.data) - self.pos

    def take(self, count: int) -> bytes:
        if count < 0 or count > self.remaining:
            raise ProfileError("Truncated plugin/VMAD field")
        start = self.pos
        self.pos += count
        return self.data[start:self.pos]

    def number(self, fmt: str) -> int:
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))[0]

    def text(self) -> str:
        count = self.number("<H")
        if count > 4096:
            raise ProfileError("Oversized VMAD string")
        return self.take(count).decode("utf-8", errors="strict")


def records(data: bytes, start: int = 0, end: int | None = None, depth: int = 0) -> Iterator[tuple[bytes, int, int, bytes]]:
    end = len(data) if end is None else end
    if depth > 32:
        raise ProfileError("Excessive GRUP nesting")
    pos = start
    while pos < end:
        if end - pos < 24:
            raise ProfileError("Truncated record header")
        kind = data[pos:pos + 4]
        size, flags, form = struct.unpack_from("<III", data, pos + 4)
        if kind == b"GRUP":
            if size < 24 or size > end - pos:
                raise ProfileError("Invalid GRUP bounds")
            yield from records(data, pos + 24, pos + size, depth + 1)
            pos += size
            continue
        if size > end - pos - 24 or size > MAX_RECORD:
            raise ProfileError("Invalid/oversized record bounds")
        payload = data[pos + 24:pos + 24 + size]
        if flags & 0x00040000:
            if len(payload) < 4:
                raise ProfileError("Truncated compressed record")
            expected = struct.unpack_from("<I", payload)[0]
            if expected > MAX_RECORD:
                raise ProfileError("Decompression limit exceeded")
            decoder = zlib.decompressobj()
            payload = decoder.decompress(payload[4:], expected + 1)
            if len(payload) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                raise ProfileError("Invalid compressed size/stream")
        yield kind, flags, form, payload
        pos += 24 + size


def subrecords(data: bytes) -> Iterator[tuple[bytes, bytes]]:
    r = Reader(data)
    extended: int | None = None
    while r.remaining:
        kind, count = r.take(4), r.number("<H")
        if kind == b"XXXX":
            if count != 4 or extended is not None:
                raise ProfileError("Invalid XXXX field")
            extended = r.number("<I")
            if extended > MAX_RECORD:
                raise ProfileError("Oversized extended field")
            continue
        yield kind, r.take(extended if extended is not None else count)
        extended = None
    if extended is not None:
        raise ProfileError("Dangling XXXX field")


@dataclass(frozen=True, order=True)
class Form:
    plugin: str
    local: int
    light: bool

    def tsv(self) -> str:
        return f"{self.plugin}\t{self.local:06x}\t{int(self.light)}"

    def audit(self) -> dict:
        return {"plugin": self.plugin, "local": f"{self.local:06x}", "light": self.light}


@dataclass
class Plugin:
    name: str
    path: Path
    light: bool
    masters: list[str]
    digest: str

    def resolve(self, raw: int, plugins: dict[str, "Plugin"]) -> Form:
        index, local = raw >> 24, raw & 0xFFFFFF
        if index > len(self.masters) or not raw or raw >> 24 == 0xFF:
            raise ProfileError(f"{self.name}: unresolved/runtime FormID {raw:08x}")
        name = self.masters[index] if index < len(self.masters) else self.name
        target = plugins[name]
        if local == 0 or local > (0xFFF if target.light else 0xFFFFFF):
            raise ProfileError(f"{self.name}: invalid {'ESL' if target.light else 'standard'} local FormID {raw:08x}")
        return Form(name, local, target.light)


# Values are (property type, value). Object values retain alias; aliases must NOT silently
# become a stable reference. Non-semantic fields are parsed to preserve stream alignment.
def vmad(data: bytes, plugin: Plugin, plugins: dict[str, Plugin]) -> dict[str, tuple[int, dict[str, tuple[int, object]]]]:
    r = Reader(data)
    version, fmt, count = r.number("<H"), r.number("<H"), r.number("<H")
    if version not in {3, 4, 5} or fmt not in {1, 2} or count > 256:
        raise ProfileError("Unsupported VMAD version/object format/count")

    def value(kind: int) -> object:
        if 11 <= kind <= 15:
            n = r.number("<I")
            if n > 4096:
                raise ProfileError("Oversized VMAD property array")
            return [value(kind - 10) for _ in range(n)]
        if kind == 0:
            return None
        if kind == 1:
            if fmt == 2:
                unused, alias, raw = r.number("<H"), r.number("<h"), r.number("<I")
            else:
                raw, alias, unused = r.number("<I"), r.number("<h"), r.number("<H")
            return (plugin.resolve(raw, plugins) if raw else None, alias, unused)
        if kind == 2:
            return r.text()
        if kind in {3, 4}:
            return r.take(4)
        if kind == 5:
            v = r.number("<B")
            if v > 1:
                raise ProfileError("Invalid VMAD boolean")
            return bool(v)
        raise ProfileError(f"Unsupported VMAD property type {kind}")

    scripts = {}
    for _ in range(count):
        name, flags, n = r.text().lower(), r.number("<B"), r.number("<H")
        if name in scripts or flags > 3 or n > 1024:
            raise ProfileError("Duplicate/invalid VMAD script")
        props = {}
        for _ in range(n):
            prop, kind, status = r.text().lower(), r.number("<B"), r.number("<B")
            if prop in props or status > 3:
                raise ProfileError("Duplicate/invalid VMAD property")
            v = value(kind)
            props[prop] = (kind, v) if status != 3 else (0, None)
        scripts[name] = (flags, props)
    # These are only REFR/ACTI/CONT VMADs: no quest/scene fragment suffix is legal here.
    if r.remaining:
        raise ProfileError("Unexpected fragment/bytes in secure chest VMAD")
    return scripts


@dataclass
class Record:
    kind: bytes
    deleted: bool
    base: Form | None
    scripts: dict


def load_plugins(paths: list[Path]) -> tuple[list[Plugin], dict[str, Plugin]]:
    plugins: dict[str, Plugin] = {}
    ordered = []
    for path in paths:
        name = plugin_name(path.name)
        if name in plugins:
            raise ProfileError(f"Duplicate plugin (case insensitive): {name}")
        if path.stat().st_size > MAX_PLUGIN:
            raise ProfileError(f"Plugin too large: {name}")
        data = path.read_bytes()
        first = next(records(data), None)
        if first is None or first[0] != b"TES4" or first[2] != 0:
            raise ProfileError(f"{name}: no leading TES4 header")
        masters = []
        for kind, payload in subrecords(first[3]):
            if kind == b"MAST":
                if not payload.endswith(b"\0") or b"\0" in payload[:-1]:
                    raise ProfileError(f"{name}: malformed MAST filename")
                master = plugin_name(payload[:-1].decode("utf-8", errors="strict"))
                if master not in plugins or master in masters:
                    raise ProfileError(f"{name}: master absent, duplicated or after dependent: {master}")
                masters.append(master)
        p = Plugin(name, path, bool(first[1] & 0x200), masters, hashlib.sha256(data).hexdigest())
        ordered.append(p)
        plugins[name] = p
    if not ordered or len(ordered) > 4096:
        raise ProfileError("Empty/oversized active plugin list")
    return ordered, plugins


def generate(paths: list[Path]) -> tuple[str, dict]:
    ordered, plugins = load_plugins(paths)
    winning: dict[Form, Record] = {}
    count = 0
    for plugin in ordered:
        data = plugin.path.read_bytes()
        if hashlib.sha256(data).hexdigest() != plugin.digest:
            raise ProfileError(f"{plugin.name}: input changed while scanning")
        for kind, flags, raw, payload in records(data):
            count += 1
            if count > MAX_RECORDS:
                raise ProfileError("Profile record limit exceeded")
            if kind not in {b"REFR", b"CONT", b"ACTI"}:
                continue
            form = plugin.resolve(raw, plugins)
            base, scripts, seen = None, {}, set()
            for field, value in subrecords(payload):
                if field not in {b"NAME", b"VMAD"}:
                    continue
                if field in seen:
                    raise ProfileError(f"{plugin.name}: duplicate NAME/VMAD")
                seen.add(field)
                if field == b"NAME" and kind == b"REFR":
                    if len(value) != 4:
                        raise ProfileError("Invalid REFR NAME length")
                    base = plugin.resolve(struct.unpack("<I", value)[0], plugins)
                elif field == b"VMAD" and SCRIPT.encode() in value.lower():
                    scripts = vmad(value, plugin, plugins)
            winning[form] = Record(kind, bool(flags & 0x20), base, scripts)
    rules: dict[Form, Form] = {}
    for access, record in winning.items():
        if record.kind != b"REFR" or record.deleted:
            continue
        base_record = winning.get(record.base) if record.base else None
        base_script = base_record.scripts.get(SCRIPT) if base_record and not base_record.deleted else None
        own_script = record.scripts.get(SCRIPT)
        if not base_script and not own_script:
            continue
        flags, props = own_script or base_script
        if flags & 2:
            continue
        # A reference script may inherit a property from its object base.
        prop = props.get(PROPERTY) or (base_script[1].get(PROPERTY) if base_script and not base_script[0] & 2 else None)
        if not prop or prop[0] != 1:
            raise ProfileError(f"{access}: secure script has no unambiguous object SecureChest property")
        backing, alias, unused = prop[1]
        if backing is None or alias != -1 or unused != 0 or backing == access:
            raise ProfileError(f"{access}: null/alias/unsupported secure backing; not guessing a reference")
        target = winning.get(backing)
        container = winning.get(target.base) if target and target.base else None
        if not target or target.deleted or target.kind != b"REFR" or not container or container.deleted or container.kind != b"CONT":
            raise ProfileError(f"{access}: SecureChest does not resolve to a live container reference")
        rules[access] = backing
    if not rules or len(rules) > MAX_RULES:
        raise ProfileError("No verified secure access points, or more than 1024; no allowlist was created")
    rows = [access.tsv() + "\t" + backing.tsv() for access, backing in sorted(rules.items())]
    canonical = "\n".join(rows) + "\n"
    digest = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    manifest = {"schema": 1, "storage_sha256": digest, "runtime": "1.5.97", "game_tested": False,
                "plugins": [{"name": p.name, "light": p.light, "sha256": p.digest} for p in ordered],
                "storage": [{"access": a.audit(), "backing": b.audit()} for a, b in sorted(rules.items())],
                "note": "Installed VMAD data only; not proof of reward/native/arena capability. Keep old personal contents until explicit import."}
    return "# RFAB_STORAGE_V1 sha256=" + digest + "\n# access-plugin\tlocal-hex\tlight\tbacking-plugin\tlocal-hex\tlight\n" + canonical, manifest


def active_paths(data: Path, plugins_file: Path) -> list[Path]:
    available = {}
    for path in data.iterdir():
        if path.is_file() and path.suffix.lower() in {".esm", ".esp", ".esl"}:
            name = plugin_name(path.name)
            if name in available:
                raise ProfileError("Case-colliding filenames in the selected Data folder")
            available[name] = path
    lines = plugins_file.read_text(encoding="utf-8-sig").splitlines()
    starred = any(line.strip().startswith("*") for line in lines if not line.lstrip().startswith("#"))
    names = []
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#") or (starred and not line.startswith("*")):
            continue
        name = plugin_name(line.lstrip("*"))
        if name not in available:
            raise ProfileError(f"Active plugin not present in virtual Data: {name}")
        names.append(available[name])
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--plugin", action="append", type=Path, help="Installed plugin, repeated in exact load order (include masters)")
    group.add_argument("--plugins", type=Path, help="Active plugins.txt in exact load order (include masters)")
    parser.add_argument("--data", type=Path, help="Merged virtual Data directory of this profile (for --plugins)")
    parser.add_argument("--output", required=True, type=Path, help="New output directory; never overwrite a previous profile")
    args = parser.parse_args()
    try:
        if args.plugins and not args.data:
            raise ProfileError("--plugins requires the installed merged --data folder")
        paths = args.plugin or active_paths(args.data, args.plugins)
        tsv, manifest = generate(paths)
        # Do not overwrite a deployed profile or a previous report, even on refusal.
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "RFAB_Storage.tsv").write_text(tsv, encoding="utf-8", newline="\n")
        (args.output / "RFAB_Profile.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"Verified {len(manifest['storage'])} access points; {len(set(row['backing']['plugin'] + ':' + row['backing']['local'] for row in manifest['storage']))} backing containers. No game files changed.")
        return 0
    except (OSError, ProfileError, UnicodeError, zlib.error, StopIteration) as error:
        print(f"REFUSED: {error}. Existing inputs/deployed profile are untouched.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
