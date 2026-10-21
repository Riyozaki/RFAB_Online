#!/usr/bin/env python3
"""Harvest SE(1.5.97) <-> AE(1.6.x) Address Library id pairs from CommonLibSSE-NG.

CommonLibSSE-NG carries the exact id pair for every symbol it exposes:

    RELOCATION_ID(se_id, ae_id)          functions / globals
    REL::VariantID RTTI_Name(se, ae, vr) RTTI descriptors
    REL::VariantID NiRTTI_Name(se, ae,vr)
    REL::VariantID VTABLE_Name(se, ae, vr) vtables (inside std::array initialisers)

Reverse engineering an id by hand is error prone; this dump is a far better
source. The result is committed as docs/phase1/clibng_id_pairs.csv so the port
does not depend on having a CommonLibSSE-NG checkout at build time.

Usage:
    python tools/port1597/extract_clibng_pairs.py /path/to/CommonLibSSE-NG
    python tools/port1597/extract_clibng_pairs.py /path/to/clibng --check
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_PATH = ROOT / "docs" / "phase1" / "clibng_id_pairs.csv"

# REL::VariantID RTTI_X(684588, 392214, 0x1ed6cf8);   (also inside std::array{...})
RE_VARIANT_NAMED = re.compile(r"REL::VariantID\s+(\w+)\s*\(\s*(\d+)\s*,\s*(\d+)\s*[,)]")
RE_VARIANT_ANON = re.compile(r"REL::VariantID\(\s*(\d+)\s*,\s*(\d+)\s*[,)]")
RE_VARIANT_ARRAY = re.compile(r"\b(VTABLE_\w+|\w+)\s*\{\s*REL::VariantID\(")
RE_RELOCATION = re.compile(r"RELOCATION_ID\(\s*(\d+)\s*,\s*(\d+)\s*[,)]")

SKIP_DIRS = {".git", "build", "extern", "ThirdParty"}


def scan_files(root: Path):
    for path in root.rglob("*"):
        if path.suffix not in {".h", ".hpp", ".cpp", ".inl"}:
            continue
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        yield path


def collect(root: Path) -> tuple[dict[int, tuple[int, str, str]], list[str]]:
    """Returns {ae_id: (se_id, source, name)} plus a list of conflict messages."""
    pairs: dict[int, tuple[int, str, str]] = {}
    conflicts: list[str] = []

    def add(se: int, ae: int, source: str, name: str) -> None:
        if se <= 0 or ae <= 0 or se == ae:
            return
        previous = pairs.get(ae)
        if previous is not None and previous[0] != se:
            conflicts.append(f"ae {ae}: se {previous[0]} ({previous[2]}, {previous[1]}) vs se {se} ({name}, {source})")
            return
        if previous is None:
            pairs[ae] = (se, source, name)

    for path in scan_files(root):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        for line in text.splitlines():
            for match in RE_VARIANT_NAMED.finditer(line):
                add(int(match.group(2)), int(match.group(3)), "clibng:variant", match.group(1))

            # Array declarations: derive the name from the declaration, then take
            # every VariantID on the line.
            array_name = ""
            array_match = RE_VARIANT_ARRAY.search(line)
            if array_match:
                array_name = array_match.group(1)
            if "REL::VariantID(" in line and "REL::VariantID " not in line:
                for match in RE_VARIANT_ANON.finditer(line):
                    add(int(match.group(1)), int(match.group(2)), "clibng:vtable", array_name)

            for match in RE_RELOCATION.finditer(line):
                add(int(match.group(1)), int(match.group(2)), "clibng:relocation", f"{path.name}")

    return pairs, conflicts


def render(pairs: dict[int, tuple[int, str, str]]) -> str:
    import io

    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(["ae_id", "se_1_5_97_id", "source", "name"])
    for ae in sorted(pairs):
        se, source, name = pairs[ae]
        writer.writerow([ae, se, source, name])
    return buffer.getvalue()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("clibng", type=Path, help="path to a CommonLibSSE-NG checkout")
    parser.add_argument("--check", action="store_true", help="fail if the output file is stale")
    args = parser.parse_args()

    if not args.clibng.is_dir():
        print(f"{args.clibng} is not a directory", file=sys.stderr)
        return 2

    pairs, conflicts = collect(args.clibng)
    text = render(pairs)

    print(f"pairs: {len(pairs)}  conflicts: {len(conflicts)}")
    for conflict in conflicts[:20]:
        print(f"  conflict: {conflict}")

    if args.check:
        current = OUT_PATH.read_text(encoding="utf-8") if OUT_PATH.exists() else ""
        if current != text:
            print(f"{OUT_PATH} is out of date", file=sys.stderr)
            return 1
        return 0

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(text, encoding="utf-8")
    print(f"wrote {OUT_PATH.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
