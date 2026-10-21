#!/usr/bin/env python3
"""Independently verify the harvested id pairs (and the database parser).

Two independent sources of truth are combined:

  * the pre-Anniversary TiltedEvolution tree (November 2021), which contains the
    *raw* SkyrimSE 1.5.97 addresses it used before the AE migration, keyed by
    a human readable name (`RTTI_TESForm(0x1E10E30)`), and
  * CommonLibSSE-NG, which lists the (SE id, AE id) pair for the same names.

If the parser and the pairs are both correct, the 1.5.97 database must map the
SE id of a name onto exactly the address the old sources used. Any mismatch
means a broken parser (typically the pointer-scaling bit) or a bad pair.

Usage:
    python tools/port1597/verify_pairs.py \
        --database /path/to/version-1-5-97-0.bin \
        --legacy-tree /path/to/te-old/Code/client \
        --pairs docs/phase1/clibng_id_pairs.csv
"""

from __future__ import annotations

import argparse
import csv
import io
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RE_LEGACY = re.compile(r"\b(RTTI_\w+|NiRTTI_\w+|DynamicCast)\s*\(\s*0x([0-9A-Fa-f]+)\s*\)")


def load_database(path: Path) -> dict[int, int]:
    """Returns {id: offset}, formats 1/2/5 (mirrors Code/common/address_library)."""
    data = path.read_bytes()
    fmt = struct.unpack_from("<i", data, 0)[0]

    if fmt == 5:
        count = struct.unpack_from("<i", data, 92)[0]
        offsets = struct.unpack_from(f"<{count}I", data, 96)
        return {i: o for i, o in enumerate(offsets) if o}

    if fmt not in (1, 2):
        sys.exit(f"unsupported database format {fmt}")

    name_length = struct.unpack_from("<i", data, 20)[0]
    pointer_size = struct.unpack_from("<i", data, 24 + name_length)[0]
    count = struct.unpack_from("<i", data, 28 + name_length)[0]

    stream = io.BytesIO(data)
    stream.seek(32 + name_length)

    def read(fmt_code: str) -> int:
        size = struct.calcsize(fmt_code)
        return struct.unpack(fmt_code, stream.read(size))[0]

    entries: dict[int, int] = {}
    previous_id = 0
    previous_offset = 0

    for _ in range(count):
        record_type = read("<B")
        id_kind = record_type & 0xF
        offset_kind = record_type >> 4

        if id_kind == 0:
            new_id = read("<Q")
        elif id_kind == 1:
            new_id = previous_id + 1
        elif id_kind == 2:
            new_id = previous_id + read("<B")
        elif id_kind == 3:
            new_id = previous_id - read("<B")
        elif id_kind == 4:
            new_id = previous_id + read("<H")
        elif id_kind == 5:
            new_id = previous_id - read("<H")
        elif id_kind == 6:
            new_id = read("<H")
        elif id_kind == 7:
            new_id = read("<I")
        else:
            sys.exit(f"invalid id encoding {id_kind}")

        scaled = (offset_kind & 0x8) != 0
        base = previous_offset // pointer_size if scaled else previous_offset
        kind = offset_kind & 0x7

        if kind == 0:
            new_offset = read("<Q")
        elif kind == 1:
            new_offset = base + 1
        elif kind == 2:
            new_offset = base + read("<B")
        elif kind == 3:
            new_offset = base - read("<B")
        elif kind == 4:
            new_offset = base + read("<H")
        elif kind == 5:
            new_offset = base - read("<H")
        elif kind == 6:
            new_offset = read("<H")
        elif kind == 7:
            new_offset = read("<I")
        else:
            sys.exit(f"invalid offset encoding {kind}")

        if scaled:
            new_offset *= pointer_size

        entries[new_id] = new_offset
        previous_id = new_id
        previous_offset = new_offset

    return entries


def load_legacy_addresses(path: Path) -> dict[str, int]:
    addresses: dict[str, int] = {}
    # The 2021 tree carries both a Skyrim and a Fallout 4 port; the latter uses
    # its own address space and must not be mixed in.
    root = path / "Games" / "Skyrim"
    search_root = root if root.is_dir() else path
    for file in search_root.rglob("*.cpp"):
        text = file.read_text(encoding="utf-8", errors="replace")
        for match in RE_LEGACY.finditer(text):
            addresses.setdefault(match.group(1), int(match.group(2), 16))
    return addresses


def load_pairs(path: Path) -> dict[str, tuple[int, int]]:
    pairs: dict[str, tuple[int, int]] = {}
    for row in csv.DictReader(path.open(encoding="utf-8")):
        name = row["name"]
        if not name or row["source"] != "clibng:variant":
            continue
        pairs.setdefault(name, (int(row["se_1_5_97_id"]), int(row["ae_id"])))
    return pairs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--legacy-tree", type=Path, required=True)
    parser.add_argument("--pairs", type=Path, default=ROOT / "docs" / "phase1" / "clibng_id_pairs.csv")
    args = parser.parse_args()

    database = load_database(args.database)
    legacy = load_legacy_addresses(args.legacy_tree)
    pairs = load_pairs(args.pairs)

    print(f"database entries: {len(database)}")
    print(f"legacy named addresses: {len(legacy)}")
    print(f"named id pairs: {len(pairs)}")

    matched = 0
    mismatched: list[tuple[str, int, int]] = []
    missing = 0

    for name, (se, ae) in pairs.items():
        legacy_address = legacy.get(name)
        if legacy_address is None:
            missing += 1
            continue

        value = database.get(se)
        if value == legacy_address:
            matched += 1
        else:
            mismatched.append((name, se, value if value is not None else -1, legacy_address))

    total = matched + len(mismatched)
    print(f"checked: {total}  matched: {matched}  mismatched: {len(mismatched)}  not in legacy tree: {missing}")

    if mismatched:
        print("mismatches (first 20):")
        for name, se, actual, expected in mismatched[:20]:
            print(f"  {name}: se id {se} -> {actual:#x}, legacy sources used {expected:#x}")

    if total == 0:
        print("no overlapping names found; nothing verified", file=sys.stderr)
        return 2

    return 0 if not mismatched else 1


if __name__ == "__main__":
    raise SystemExit(main())
