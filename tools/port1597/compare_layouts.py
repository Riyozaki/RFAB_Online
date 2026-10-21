#!/usr/bin/env python3
"""Compare the compile time layout assertions of the 2021 (1.5.97) tree with the fork.

Every game structure in the client is pinned by
`static_assert(offsetof(Class, member) == 0x..)` / `static_assert(sizeof(Class) == 0x..)`.
The 2021 tree was written against SkyrimSE 1.5.97, the current one against 1.6.x,
so the difference between the two sets *is* the layout porting work.

Usage:
    python tools/port1597/compare_layouts.py --legacy-tree /path/to/te-old/Code/client
"""

from __future__ import annotations

import argparse
import collections
import csv
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CURRENT = ROOT / "Code" / "client"
OUT_PATH = ROOT / "docs" / "phase1" / "layout_diff.csv"

RE_OFFSET = re.compile(r"static_assert\s*\(\s*offsetof\s*\(\s*([\w:]+)\s*,\s*(\w+)\s*\)\s*==\s*(0x[0-9A-Fa-f]+|\d+)\s*\)")
RE_SIZE = re.compile(r"static_assert\s*\(\s*sizeof\s*\(\s*([\w:]+)\s*\)\s*==\s*(0x[0-9A-Fa-f]+|\d+)\s*\)")


def parse_tree(root: Path) -> tuple[dict[tuple[str, str], int], dict[str, int], dict[tuple[str, str], str]]:
    offsets: dict[tuple[str, str], int] = {}
    sizes: dict[str, int] = {}
    origin: dict[tuple[str, str], str] = {}

    for path in root.rglob("*.h"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in RE_OFFSET.finditer(text):
            key = (match.group(1), match.group(2))
            offsets.setdefault(key, int(match.group(3), 0))
            origin.setdefault(key, path.name)
        for match in RE_SIZE.finditer(text):
            sizes.setdefault(match.group(1), int(match.group(2), 0))

    return offsets, sizes, origin


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--legacy-tree", type=Path, required=True)
    args = parser.parse_args()

    current_offsets, current_sizes, origin = parse_tree(CURRENT)
    legacy_offsets, legacy_sizes, _ = parse_tree(args.legacy_tree)

    common = sorted(set(current_offsets) & set(legacy_offsets))
    only_current = sorted(set(current_offsets) - set(legacy_offsets))
    only_legacy = sorted(set(legacy_offsets) - set(current_offsets))

    rows = []
    deltas = collections.Counter()
    for key in common:
        delta = current_offsets[key] - legacy_offsets[key]
        deltas[delta] += 1
        rows.append(
            {
                "kind": "offset",
                "class": key[0],
                "member": key[1],
                "se_1_5_97": legacy_offsets[key],
                "ae_1_6_x": current_offsets[key],
                "delta": delta,
                "file": origin.get(key, ""),
            }
        )

    size_rows = []
    size_deltas = collections.Counter()
    for name in sorted(set(current_sizes) & set(legacy_sizes)):
        delta = current_sizes[name] - legacy_sizes[name]
        size_deltas[delta] += 1
        size_rows.append(
            {
                "kind": "sizeof",
                "class": name,
                "member": "",
                "se_1_5_97": legacy_sizes[name],
                "ae_1_6_x": current_sizes[name],
                "delta": delta,
                "file": "",
            }
        )

    rows.extend(size_rows)

    with OUT_PATH.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=["kind", "class", "member", "se_1_5_97", "ae_1_6_x", "delta", "file"])
        writer.writeheader()
        writer.writerows(rows)

    print(f"offset assertions: current {len(current_offsets)}, legacy {len(legacy_offsets)}, common {len(common)}")
    print(f"sizeof assertions: current {len(current_sizes)}, legacy {len(legacy_sizes)}, common {len(size_rows)}")
    print(f"assertions only in the current tree: {len(only_current)}")
    print(f"assertions only in the legacy tree: {len(only_legacy)}")
    print()
    print("offset delta distribution (ae - se):")
    for delta, count in sorted(deltas.items(), key=lambda item: -item[1]):
        print(f"  {delta:+#x}: {count}")
    print("sizeof delta distribution:")
    for delta, count in sorted(size_deltas.items(), key=lambda item: -item[1]):
        print(f"  {delta:+#x}: {count}")
    print()
    print("non-zero offset deltas:")
    for row in rows:
        if row["kind"] == "offset" and row["delta"]:
            print(f"  {row['class']}::{row['member']}: {row['se_1_5_97']:#x} -> {row['ae_1_6_x']:#x} ({row['delta']:+#x}) [{row['file']}]")
    print()
    print(f"wrote {OUT_PATH.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
