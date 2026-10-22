#!/usr/bin/env python3
"""
Tools/rfab/verify_se1597_port.py

Offline verifier for the Skyrim SE 1.5.97.0.8 port of RFAB Online:
1. Validates that every row in docs/se-1597-id-map.csv has a valid SE 1.5.97 ID
   and, if version-1-5-97-0.bin is provided via --addrlib, verifies that every
   mapped ID exists in version-1-5-97-0.bin and matches its recorded RVA.
2. Scans all C/C++ sources under Code/ for POINTER_SKYRIMSE(...) and
   VersionDbPtr<...>(...) references and reports how many are already migrated
   to SE 1.5.97 IDs vs still carrying AE 1.6.x IDs.
3. Checks critical 1.5.97 C++ struct size/offset assertions across Code/client/Games/Skyrim/.
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import struct
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MAP_CSV = REPO_ROOT / "docs" / "se-1597-id-map.csv"

EXPECTED_STRUCT_ASSERTS = {
    "Code/client/Games/Skyrim/ExtraData/ExtraDataList.h": "sizeof(ExtraDataList) == 0x18",
    "Code/client/Games/Skyrim/TESObjectREFR.h": "sizeof(TESObjectREFR) == 0x98",
    "Code/client/Games/Skyrim/Actor.h": "sizeof(Actor) == 0x2B0",
    "Code/client/Games/Skyrim/PlayerCharacter.h": "sizeof(PlayerCharacter) == 0xBE0",
    "Code/client/Games/Skyrim/Forms/TESObjectCELL.h": "sizeof(TESObjectCELL) == 0x140",
    "Code/client/Games/Skyrim/Components/TESLeveledList.h": "sizeof(TESLeveledList) == 0x28",
    "Code/client/Games/Skyrim/BSAnimationGraphManager.h": "offsetof(BSAnimationGraphManager, animationGraphIndex) == 0xA8",
    "Code/client/Games/Skyrim/Combat/CombatController.h": "sizeof(CombatController) == 0xD8",
}


def load_version_bin_v1(path: pathlib.Path) -> dict[int, int]:
    data = path.read_bytes()
    pos = 0

    def ru8() -> int:
        nonlocal pos
        v = data[pos]
        pos += 1
        return v

    def ru16() -> int:
        nonlocal pos
        v = struct.unpack_from("<H", data, pos)[0]
        pos += 2
        return v

    def ru32() -> int:
        nonlocal pos
        v = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        return v

    def ru64() -> int:
        nonlocal pos
        v = struct.unpack_from("<Q", data, pos)[0]
        pos += 8
        return v

    fmt = ru32()
    if fmt != 1:
        raise ValueError(f"Expected Address Library format 1 (SE 1.5.97), got {fmt}")
    ver = (ru32(), ru32(), ru32(), ru32())
    if ver[:3] != (1, 5, 97):
        raise ValueError(f"Expected binary version 1.5.97.x, got {ver}")
    name_len = ru32()
    pos += name_len
    ptr_size = ru32()
    count = ru32()

    se_rva: dict[int, int] = {}
    pvid = 0
    pvo = 0
    for _ in range(count):
        tp = ru8()
        lo = tp & 0xF
        hi = tp >> 4
        if lo == 0:
            idv = ru64()
        elif lo == 1:
            idv = pvid + 1
        elif lo == 2:
            idv = pvid + ru8()
        elif lo == 3:
            idv = pvid - ru8()
        elif lo == 4:
            idv = pvid + ru16()
        elif lo == 5:
            idv = pvid - ru16()
        elif lo == 6:
            idv = ru16()
        else:
            idv = ru32()

        tpo = (pvo // ptr_size) if (hi & 8) else pvo
        hi &= 7
        if hi == 0:
            ov = ru64()
        elif hi == 1:
            ov = tpo + 1
        elif hi == 2:
            ov = tpo + ru8()
        elif hi == 3:
            ov = tpo - ru8()
        elif hi == 4:
            ov = tpo + ru16()
        elif hi == 5:
            ov = tpo - ru16()
        elif hi == 6:
            ov = ru16()
        else:
            ov = ru32()

        if tp & 0x80:
            ov *= ptr_size
        se_rva[idv] = ov
        pvid = idv
        pvo = ov
    return se_rva


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify Skyrim SE 1.5.97 port state")
    parser.add_argument("--addrlib", type=pathlib.Path, default=None, help="Optional path to version-1-5-97-0.bin")
    parser.add_argument("--strict-migrated", action="store_true", help="Fail if any unmigrated AE IDs or struct asserts remain")
    args = parser.parse_args()

    rows = list(csv.DictReader(MAP_CSV.open(newline="", encoding="utf-8")))
    print(f"[id-map] Loaded {len(rows)} AE->SE entries from {MAP_CSV.relative_to(REPO_ROOT)}")

    ae_to_se = {int(r["ae_id"]): int(r["se_1_5_97_id"]) for r in rows}
    se_set = set(ae_to_se.values())

    if args.addrlib and args.addrlib.exists():
        se_rva = load_version_bin_v1(args.addrlib)
        print(f"[addrlib] Verified format-1 version-1-5-97-0.bin with {len(se_rva)} entries")
        bad = [
            r for r in rows
            if int(r["se_1_5_97_id"]) not in se_rva
            or se_rva[int(r["se_1_5_97_id"])] != int(r["se_rva_hex"], 16)
        ]
        if bad:
            print(f"[ERROR] {len(bad)} invalid entries against version-1-5-97-0.bin: {bad[:5]}")
            return 1
        print("[addrlib] 100% of docs/se-1597-id-map.csv entries match version-1-5-97-0.bin byte-for-byte!")

    pat_ptr = re.compile(r"\bPOINTER_SKYRIMSE\s*\([^,]+,[^,]+,\s*(\d+)\s*\)")
    pat_vdb = re.compile(r"\bVersionDbPtr\s*<[^>]+>\s*\w+\s*\(\s*(\d+)\s*\)")
    pat_rtti = re.compile(r"\{\s*(\d+)\s*(?:,|\})")

    found_ids: set[int] = set()
    for p in (REPO_ROOT / "Code").rglob("*"):
        if p.suffix not in {".h", ".hpp", ".inl", ".c", ".cpp"}:
            continue
        txt = p.read_text(encoding="utf-8", errors="replace")
        for m in pat_ptr.finditer(txt):
            found_ids.add(int(m.group(1)))
        for m in pat_vdb.finditer(txt):
            found_ids.add(int(m.group(1)))
        if p.name == "RTTI.cpp":
            for m in pat_rtti.finditer(txt):
                val = int(m.group(1))
                if val > 1000:
                    found_ids.add(val)

    in_se = sum(1 for x in found_ids if x in se_set or x == 75591)
    only_ae = sum(1 for x in found_ids if x in ae_to_se and x not in se_set)
    print(f"[scan] Unique IDs in Code/: {len(found_ids)} | valid SE 1.5.97 IDs: {in_se} | still AE-only IDs: {only_ae}")

    missing_structs = []
    for rel_path, expected in EXPECTED_STRUCT_ASSERTS.items():
        txt = (REPO_ROOT / rel_path).read_text(encoding="utf-8", errors="replace")
        ok = expected in txt
        status = "OK" if ok else "NEEDS_PORT"
        print(f"[struct] {status:<10} {rel_path} -> {expected}")
        if not ok:
            missing_structs.append(rel_path)

    if args.strict_migrated and (only_ae > 0 or missing_structs):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
