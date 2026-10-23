#!/usr/bin/env python3
"""
Tools/rfab/parse_rfab_esp.py

Standalone TES4/Skyrim plugin (.esp/.esm/.esl) record extractor used to regenerate
docs/rfab/rfab_records.tsv from a local RFAB.esp installation without committing
binary game plugins into Git.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import zlib


def parse_esp(esp_path: pathlib.Path, out_tsv: pathlib.Path) -> int:
    data = esp_path.read_bytes()
    pos = 0
    rows: list[tuple[str, str, str, str]] = []

    def walk(buf: bytes, start: int, end: int) -> None:
        p = start
        while p + 24 <= end:
            rec_type = buf[p : p + 4].decode("latin1")
            size, flags, form_id = struct.unpack_from("<III", buf, p + 4)
            if rec_type == "GRUP":
                walk(buf, p + 24, p + size)
                p += size
                continue
            payload = buf[p + 24 : p + 24 + size]
            if flags & 0x00040000 and len(payload) >= 4:
                payload = zlib.decompress(payload[4:])
            edid = ""
            full = ""
            sp = 0
            xxxx_size = 0
            while sp + 6 <= len(payload):
                sub_type = payload[sp : sp + 4].decode("latin1")
                sub_len = struct.unpack_from("<H", payload, sp + 4)[0]
                sp += 6
                if sub_type == "XXXX":
                    xxxx_size = struct.unpack_from("<I", payload, sp)[0]
                    sp += sub_len
                    continue
                if xxxx_size:
                    sub_len = xxxx_size
                    xxxx_size = 0
                raw_val = payload[sp : sp + sub_len]
                sp += sub_len
                if sub_type == "EDID":
                    edid = raw_val.rstrip(b"\x00").decode("utf-8", errors="replace")
                elif sub_type == "FULL" and len(raw_val) > 4:
                    full = raw_val.rstrip(b"\x00").decode("cp1251", errors="replace")
            if edid or rec_type in {"GLOB", "QUST", "PERK", "SPEL", "MGEF", "NPC_", "LVLN", "KYWD", "FLST"}:
                rows.append((rec_type, f"0x{form_id:08X}", edid, full))
            p += 24 + size

    walk(data, pos, len(data))
    out_tsv.parent.mkdir(parents=True, exist_ok=True)
    with out_tsv.open("w", encoding="utf-8") as f:
        f.write("type\tform_id\teditor_id\tfull_name\n")
        for rec_type, fid, edid, full in rows:
            clean_full = full.replace("\t", " ").replace("\n", " ").replace("\r", "")
            f.write(f"{rec_type}\t{fid}\t{edid}\t{clean_full}\n")
    return len(rows)


def main() -> int:
    ap = argparse.ArgumentParser(description="Extract TES4 records from RFAB.esp to TSV")
    ap.add_argument("esp", type=pathlib.Path, help="Path to RFAB.esp")
    ap.add_argument(
        "-o",
        "--output",
        type=pathlib.Path,
        default=pathlib.Path("docs/rfab/rfab_records.tsv"),
        help="Output TSV path",
    )
    args = ap.parse_args()
    count = parse_esp(args.esp, args.output)
    print(f"Wrote {count} records to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
