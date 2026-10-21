#!/usr/bin/env python3
"""Build the worklist for the Address Library ids that still need reversing.

For every id the client references but that has no 1.5.97 counterpart yet the
report collects everything a reverse engineer needs:

  * the symbol name(s) used at the call site and where they are used;
  * the neighbouring AE ids that *are* mapped, with their SE counterparts, so
    the candidate range is obvious (Address Library ids grow with the address
    of the function, so an unmapped id sits between its mapped neighbours);
  * flags for suspicious cases (one AE id used for two different symbols, an
    id that contradicts the neighbour ordering).

The report is deliberately a *worklist*: candidate ids are not applied
automatically. Anything derived from neighbours must be confirmed against the
1.5.97 executable, because functions are added and removed between versions.

Usage:
    python tools/port1597/id_worklist.py
"""

from __future__ import annotations

import argparse
import collections
import csv
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
CLIENT = ROOT / "Code" / "client"
INVENTORY = ROOT / "docs" / "phase1" / "address_id_inventory.csv"
OUT = ROOT / "docs" / "phase1" / "id_worklist.csv"

PATTERNS = (
    re.compile(r"POINTER_SKYRIMSE\s*\(\s*[\w:<>,\[\]*&\s]+?,\s*(\w+)\s*,\s*(\d+)\s*\)\s*;", re.S),
    re.compile(r"VersionDbPtr\s*<[^;>]*>\s*(\w+)\s*\(\s*(\d+)\s*\)\s*;"),
    # internal::RttiLocator<TESForm> registerRtti_TESForm(392216);
    re.compile(r"registerRtti_(\w+)\s*\(\s*(\d+)\s*\)"),
)


def scan_symbols() -> tuple[dict[int, set[str]], dict[int, list[str]]]:
    symbols: dict[int, set[str]] = collections.defaultdict(set)
    references: dict[int, list[str]] = collections.defaultdict(list)

    for path in sorted(CLIENT.rglob("*")):
        if path.suffix not in {".cpp", ".h"}:
            continue
        relative = path.relative_to(ROOT).as_posix()
        for line_number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for pattern in PATTERNS:
                for match in pattern.finditer(line):
                    ae_id = int(match.group(2))
                    symbols[ae_id].add(match.group(1))
                    references[ae_id].append(f"{relative}:{line_number}")

    return symbols, references


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=pathlib.Path, default=OUT)
    args = parser.parse_args()

    rows = list(csv.DictReader(INVENTORY.open(encoding="utf-8")))
    mapped = sorted(int(row["ae_id"]) for row in rows if row["se_1_5_97_id"])
    unmapped = sorted(int(row["ae_id"]) for row in rows if not row["se_1_5_97_id"])

    symbols, references = scan_symbols()

    mapped_set = set(mapped)
    report = []

    for ae_id in unmapped:
        previous = next((value for value in reversed(mapped) if value < ae_id), None)
        following = next((value for value in mapped if value > ae_id), None)

        previous_row = next((row for row in rows if int(row["ae_id"]) == previous), None)
        following_row = next((row for row in rows if int(row["ae_id"]) == following), None)

        notes = []
        names = sorted(symbols.get(ae_id, []))
        if len(names) > 1:
            notes.append("несколько локальных имён у одного id (проверить, одна ли это функция): " + ", ".join(names))
        if not names:
            notes.append("символ не найден сканером (проверить вручную)")

        report.append(
            {
                "ae_id": ae_id,
                "kind": next(row["kind"] for row in rows if int(row["ae_id"]) == ae_id),
                "symbols": ", ".join(names),
                "references": "; ".join(references.get(ae_id, [])[:3]),
                "prev_mapped_ae": previous or "",
                "prev_mapped_se": (previous_row or {}).get("se_1_5_97_id", ""),
                "next_mapped_ae": following or "",
                "next_mapped_se": (following_row or {}).get("se_1_5_97_id", ""),
                "notes": "; ".join(notes),
            }
        )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(report[0].keys()) if report else [], lineterminator="\n")
        writer.writeheader()
        writer.writerows(report)

    duplicates = [row for row in report if "несколько локальных имён" in row["notes"]]
    no_symbol = [row for row in report if "не найден" in row["notes"]]

    print(f"unmapped ids: {len(unmapped)}")
    print(f"  with several symbols (suspicious): {len(duplicates)} -> {[row['ae_id'] for row in duplicates]}")
    print(f"  without a scanned symbol: {len(no_symbol)} -> {[row['ae_id'] for row in no_symbol]}")
    print(f"wrote {args.out.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
