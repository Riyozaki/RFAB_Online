#!/usr/bin/env python3
"""Narrow down the Address Library ids that still have no 1.5.97 counterpart.

Address Library ids are allocated in address order inside a database, so the
mapping AE id -> SE id is monotonically increasing (functions get inserted or
removed between versions, but their relative order holds).  Every known pair is
therefore an anchor: an unmapped AE id must map to an SE id that sits between
the SE ids of its two closest anchors.

The script prints that interval and, when the interval is small, the exact SE
ids a reverse engineer has to choose from (with their 1.5.97 file offsets).
Short intervals can be resolved by opening the function in IDA/Ghidra; wide
intervals need the executable.  Candidates are *never* applied automatically.

Usage:
    python tools/port1597/id_candidates.py --database ~/.ref/rfab/al-1-5-97.bin
"""

from __future__ import annotations

import argparse
import bisect
import csv
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[2]
INVENTORY = ROOT / "docs" / "phase1" / "address_id_inventory.csv"
PAIRS = ROOT / "docs" / "phase1" / "clibng_id_pairs.csv"
WORKLIST = ROOT / "docs" / "phase1" / "id_worklist.csv"
OUT = ROOT / "docs" / "phase1" / "id_candidates.csv"

MAX_LISTED = 8


def load_offsets(path: pathlib.Path) -> dict[int, int]:
    """id -> 1.5.97 offset (format 1/2 sparse database)."""
    data = path.read_bytes()
    position = 0

    def read(fmt: str) -> tuple:
        nonlocal position
        value = struct.unpack_from(fmt, data, position)
        position += struct.calcsize(fmt)
        return value

    (format_version,) = read("<i")
    if format_version not in (1, 2):
        raise SystemExit(f"unsupported database format {format_version}")

    read("<4i")
    (name_length,) = read("<i")
    position += name_length
    (pointer_size,) = read("<i")
    (count,) = read("<i")

    offsets: dict[int, int] = {}
    previous_id = previous_offset = 0

    for _ in range(count):
        (record_type,) = read("<B")
        id_kind, offset_kind = record_type & 0x0F, record_type >> 4

        if id_kind == 0:
            (address_id,) = read("<Q")
        elif id_kind == 1:
            address_id = previous_id + 1
        elif id_kind in (2, 3):
            (value,) = read("<B")
            address_id = previous_id + value if id_kind == 2 else previous_id - value
        elif id_kind in (4, 5):
            (value,) = read("<H")
            address_id = previous_id + value if id_kind == 4 else previous_id - value
        elif id_kind == 6:
            (address_id,) = read("<H")
        elif id_kind == 7:
            (address_id,) = read("<I")
        else:
            raise SystemExit(f"invalid address id encoding {id_kind}")

        scaled = bool(offset_kind & 0x8)
        base = previous_offset // pointer_size if scaled else previous_offset
        kind = offset_kind & 0x7

        if kind == 0:
            (offset,) = read("<Q")
        elif kind == 1:
            offset = base + 1
        elif kind in (2, 3):
            (value,) = read("<B")
            offset = base + value if kind == 2 else base - value
        elif kind in (4, 5):
            (value,) = read("<H")
            offset = base + value if kind == 4 else base - value
        elif kind == 6:
            (offset,) = read("<H")
        elif kind == 7:
            (offset,) = read("<I")
        else:
            raise SystemExit(f"invalid address offset encoding {kind}")

        if scaled:
            offset *= pointer_size

        previous_id, previous_offset = address_id, offset
        offsets[address_id] = offset

    return offsets


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--database", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, default=OUT)
    args = parser.parse_args()

    offsets = load_offsets(args.database)

    inventory = list(csv.DictReader(INVENTORY.open(encoding="utf-8")))
    worklist = {int(row["ae_id"]): row for row in csv.DictReader(WORKLIST.open(encoding="utf-8"))}

    # Only pairs that came from CommonLibSSE-NG are used as anchors: the phase 0
    # heuristics were guessed from symbol names and two of them demonstrably
    # collide (several AE ids mapped onto one SE id).
    anchors = {
        int(row["ae_id"]): int(row["se_1_5_97_id"])
        for row in csv.DictReader(PAIRS.open(encoding="utf-8"))
        if row["se_1_5_97_id"]
    }
    anchor_ids = sorted(anchors)

    used_se = {se for _, se in anchors.items()}
    unmapped = [int(row["ae_id"]) for row in inventory if not row["se_1_5_97_id"]]

    report = []
    for ae_id in sorted(unmapped):
        position = bisect.bisect_left(anchor_ids, ae_id)
        previous = anchors[anchor_ids[position - 1]] if position > 0 else None
        following = anchors[anchor_ids[position]] if position < len(anchor_ids) else None

        if previous is None or following is None:
            width = -1
            candidates: list[int] = []
            verdict = "вне диапазона известных пар"
        else:
            candidates = [
                se for se in range(previous + 1, following) if se in offsets and se not in used_se
            ]
            width = following - previous - 1
            if width <= 0:
                verdict = "интервал пуст: соседние пары указывают на ошибку в парах"
            elif len(candidates) == 1:
                verdict = "единственный кандидат — проверить тело функции и принять"
            elif len(candidates) <= MAX_LISTED:
                verdict = "узкий интервал — проверить кандидатов по смещению"
            else:
                verdict = "широкий интервал — нужен exe (IDA/Ghidra)"

        row = worklist.get(ae_id, {})
        report.append(
            {
                "ae_id": ae_id,
                "symbols": row.get("symbols", ""),
                "references": row.get("references", ""),
                "prev_ae": anchor_ids[position - 1] if position > 0 else "",
                "prev_se": previous if previous is not None else "",
                "next_ae": anchor_ids[position] if position < len(anchor_ids) else "",
                "next_se": following if following is not None else "",
                "interval_width": width,
                "used_candidates": len(candidates),
                "candidates": " ".join(
                    f"{se}@0x{offsets[se]:X}" for se in candidates[:MAX_LISTED]
                ),
                "verdict": verdict,
            }
        )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(report[0].keys()))
        writer.writeheader()
        writer.writerows(report)

    buckets: dict[str, int] = {}
    for row in report:
        buckets[row["verdict"]] = buckets.get(row["verdict"], 0) + 1

    print(f"unmapped ids analysed: {len(report)}")
    for verdict, count in sorted(buckets.items(), key=lambda item: -item[1]):
        print(f"  {count:>3}  {verdict}")
    print(f"wrote {args.out.relative_to(ROOT)}")

    for row in report:
        if row["used_candidates"] == 1:
            print(f"  single candidate: {row['ae_id']} ({row['symbols']}) -> {row['candidates']}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
