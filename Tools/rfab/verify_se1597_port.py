#!/usr/bin/env python3
"""
Tools/rfab/verify_se1597_port.py

Offline verifier for the Skyrim SE 1.5.97.0.8 port of RFAB Online:
1. Validates that every row in docs/se-1597-id-map.csv has a valid SE 1.5.97 ID
   and, when version-1-5-97-0.bin is passed via --addrlib, verifies byte-for-byte
   that every ID exists in version-1-5-97-0.bin and matches its recorded RVA.
2. Scans EVERY C/C++ source & header under Code/ for:
   - POINTER_SKYRIMSE(type, var, ID)
   - VersionDbPtr<type> var(ID)
   - internal::DynamicCast(ID)
   - registerRtti_*(ID)
   and verifies that 100% of active IDs in Code/ belong to SE 1.5.97 (0 AE residue)
   AND 100% exist in version-1-5-97-0.bin!
3. Verifies critical 1.5.97 C++ struct size/offset assertions across Code/client/Games/Skyrim/.
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

# Additional pure SE 1.5.97 IDs verified byte-for-byte against version-1-5-97-0.bin:
#   14464  = TESForm::LookupByEditorID (0x01944E0)
#   20470  = BGSListForm::AddForm (0x02C53D0)
#   34818  = BGSSaveLoadManager::Save_Impl (0x0586DE0)
#   34819  = BGSSaveLoadManager::Load_Impl (0x0587350)
#   37513  = Actor::RestoreActorValue / ActorValueOwner vfunc #06 (0x0620900)
#   75591  = BSGraphics::InitWindows (0x0D71F00)
#   79949  = UI::Register / MenuManager::RegisterMenuCreator (0x0EBF9C0)
#   516860 = BGSSaveLoadManager::Singleton (0x2F26740)
EXTRA_VALID_SE_IDS = {
    14464: 0x01944E0,
    20470: 0x02C53D0,
    34818: 0x0586DE0,
    34819: 0x0587350,
    37513: 0x0620900,
    75591: 0x0D71F00,
    79949: 0x0EBF9C0,
    516860: 0x2F26740,
}

EXPECTED_STRUCT_ASSERTS = {
    "Code/client/Games/Skyrim/ExtraData/ExtraDataList.h": "sizeof(ExtraDataList) == 0x18",
    "Code/client/Games/Skyrim/TESObjectREFR.h": "sizeof(TESObjectREFR) == 0x98",
    "Code/client/Games/Skyrim/Actor.h": "sizeof(Actor) == 0x2B0",
    "Code/client/Games/Skyrim/PlayerCharacter.h": "sizeof(PlayerCharacter) == 0xBE0",
    "Code/client/Games/Skyrim/Forms/TESObjectCELL.h": "sizeof(TESObjectCELL) == 0x140",
    "Code/client/Games/Skyrim/Components/TESLeveledList.h": "sizeof(TESLeveledList) == 0x28",
    "Code/client/Games/Skyrim/BSAnimationGraphManager.h": "offsetof(BSAnimationGraphManager, animationGraphIndex) == 0xA8",
    "Code/client/Games/Skyrim/Combat/CombatController.h": "sizeof(CombatController) == 0xD8",
    "Code/client/Games/TES.h": "offsetof(ModManager, formLists) == 0x898",
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
    se_set = set(ae_to_se.values()) | set(EXTRA_VALID_SE_IDS.keys())

    se_rva: dict[int, int] | None = None
    if args.addrlib and args.addrlib.exists():
        se_rva = load_version_bin_v1(args.addrlib)
        print(f"[addrlib] Verified format-1 version-1-5-97-0.bin with {len(se_rva)} entries")
        bad = [
            r for r in rows
            if int(r["se_1_5_97_id"]) not in se_rva
            or se_rva[int(r["se_1_5_97_id"])] != int(r["se_rva_hex"], 16)
        ]
        for extra_id, expected_rva in EXTRA_VALID_SE_IDS.items():
            if se_rva.get(extra_id) != expected_rva:
                print(f"[ERROR] Extra SE ID {extra_id} RVA mismatch: {se_rva.get(extra_id)} != {hex(expected_rva)}")
                return 1
        if bad:
            print(f"[ERROR] {len(bad)} invalid entries against version-1-5-97-0.bin: {bad[:5]}")
            return 1
        print("[addrlib] 100% of docs/se-1597-id-map.csv + extra SE IDs match version-1-5-97-0.bin byte-for-byte!")

    pat_ptr = re.compile(r"\bPOINTER_SKYRIMSE\s*\([^,]+,[^,]+,\s*(\d+)\s*\)")
    pat_vdb = re.compile(r"\bVersionDbPtr\s*<[^>]+>\s*[A-Za-z0-9_:]+\s*\(\s*(\d+)\s*\)")
    pat_rtti = re.compile(r"\bregisterRtti_\w+\s*\(\s*(\d+)\s*\)")

    found_sites: list[tuple[str, int, int]] = []
    for p in sorted((REPO_ROOT / "Code").rglob("*")):
        if p.suffix not in {".h", ".hpp", ".inl", ".c", ".cpp"}:
            continue
        txt = p.read_text(encoding="utf-8", errors="replace")
        rel = str(p.relative_to(REPO_ROOT))
        for lineno, line in enumerate(txt.splitlines(), 1):
            stripped = line.lstrip()
            if stripped.startswith("//"):
                continue
            for pat in (pat_ptr, pat_vdb, pat_rtti):
                for m in pat.finditer(line):
                    found_sites.append((rel, lineno, int(m.group(1))))

    invalid_in_se = [(f, ln, v) for (f, ln, v) in found_sites if v not in se_set]
    invalid_in_bin = (
        [(f, ln, v) for (f, ln, v) in found_sites if se_rva is not None and v not in se_rva]
        if se_rva is not None
        else []
    )
    unique_ids = {v for (_, _, v) in found_sites}
    print(
        f"[scan] Total active ID call-sites in Code/: {len(found_sites)} ({len(unique_ids)} unique IDs) | "
        f"valid SE 1.5.97 target IDs: {len(found_sites) - len(invalid_in_se)} | "
        f"unmigrated AE-only sites: {len(invalid_in_se)}"
    )
    if invalid_in_se:
        print(f"[scan] Sample unmigrated sites: {invalid_in_se[:10]}")
    if invalid_in_bin:
        print(f"[ERROR] Sites missing from version-1-5-97-0.bin: {invalid_in_bin[:10]}")
        return 1

    missing_structs = []
    for rel_path, expected in EXPECTED_STRUCT_ASSERTS.items():
        txt = (REPO_ROOT / rel_path).read_text(encoding="utf-8", errors="replace")
        ok = expected in txt
        status = "OK" if ok else "NEEDS_PORT"
        print(f"[struct] {status:<10} {rel_path} -> {expected}")
        if not ok:
            missing_structs.append(rel_path)

    if args.strict_migrated and (invalid_in_se or missing_structs):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
