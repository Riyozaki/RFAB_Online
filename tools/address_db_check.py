#!/usr/bin/env python3
"""Phase 0 audit tool: check TiltedEvolution Address Library IDs against a
version database file (e.g. RFAB's version-1-5-97-0.bin for Skyrim SE 1.5.97).

The decoder below is a faithful re-implementation of the nibble-delta parser in
TiltedEvolution's Code/client/VersionDb.h (VersionDb::Load). It is used here to
prove that the SE (format 1) database payload decodes with the exact same
algorithm the client uses for format 2 files.

Usage:
    python3 tools/address_db_check.py <version-*.bin> <TiltedEvolution-source-root>

Exit code 0 if every referenced ID is present in the database, 1 otherwise.
Presence of an ID proves the database contains a counterpart offset; it does
NOT prove identical function signatures or struct layouts (audited separately).
"""
import json
import re
import struct
import subprocess
import sys


def decode_db(path):
    data = open(path, "rb").read()
    off = 0

    def rd(fmt):
        nonlocal off
        size = struct.calcsize("<" + fmt)
        vals = struct.unpack_from("<" + fmt, data, off)
        off += size
        return vals[0]

    fmt = rd("i")
    ver = [rd("i") for _ in range(4)]
    tn_len = rd("i")
    module = data[off:off + tn_len].decode(errors="replace")
    off += tn_len
    ptr_size = rd("i")
    addr_count = rd("i")

    ids = {}
    pvid = 0
    poffset = 0
    for _ in range(addr_count):
        t = rd("B")
        low, high = t & 0xF, t >> 4
        if low == 0:
            q1 = rd("Q")
        elif low == 1:
            q1 = pvid + 1
        elif low == 2:
            q1 = pvid + rd("B")
        elif low == 3:
            q1 = pvid - rd("B")
        elif low == 4:
            q1 = pvid + rd("H")
        elif low == 5:
            q1 = pvid - rd("H")
        elif low == 6:
            q1 = rd("H")
        elif low == 7:
            q1 = rd("I")
        else:
            raise ValueError(f"bad id type nibble {low}")
        tp = (poffset // ptr_size) if (high & 8) else poffset
        h = high & 7
        if h == 0:
            q2 = rd("Q")
        elif h == 1:
            q2 = tp + 1
        elif h == 2:
            q2 = tp + rd("B")
        elif h == 3:
            q2 = tp - rd("B")
        elif h == 4:
            q2 = tp + rd("H")
        elif h == 5:
            q2 = tp - rd("H")
        elif h == 6:
            q2 = rd("H")
        else:
            q2 = rd("I")
        if high & 8:
            q2 *= ptr_size
        ids[q1] = q2
        poffset, pvid = q2, q1

    assert off == len(data), f"decoder desync: consumed {off} of {len(data)} bytes"
    return fmt, ver, module, ids


def collect_ids(root):
    out = subprocess.run(
        ["grep", "-rIn", "--include=*.cpp", "--include=*.h", "-E",
         r"POINTER_SKYRIMSE\(|VersionDbPtr", "Code/client"],
        cwd=root, capture_output=True, text=True).stdout
    pat = re.compile(r"(Code/[^:]+):\d+:.*?(?:POINTER_SKYRIMSE|VersionDbPtr).*?(\d{3,7})\)")
    found = {}
    for line in out.splitlines():
        m = pat.search(line)
        if m:
            found.setdefault(int(m.group(2)), set()).add(m.group(1))
    rtti_src = open(f"{root}/Code/client/Games/Skyrim/RTTI.cpp").read()
    rtti = [int(x) for x in re.findall(r"registerRtti_\w+\((\d+)\)", rtti_src)]
    return found, rtti


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    fmt, ver, module, ids = decode_db(sys.argv[1])
    print(f"database: format={fmt} version={'.'.join(map(str, ver))} "
          f"module={module} entries={len(ids)} max_offset={max(ids.values()):#x}")
    found, rtti = collect_ids(sys.argv[2])
    missing = {k: v for k, v in found.items() if k not in ids}
    rtti_missing = [i for i in rtti if i not in ids]
    print(f"function/global IDs referenced: {len(found)}, missing: {len(missing)}")
    for k in sorted(missing):
        print(f"  MISSING {k} in {sorted(missing[k])[:3]}")
    print(f"RTTI IDs referenced: {len(rtti)}, missing: {len(rtti_missing)}")
    if rtti_missing:
        print(f"  examples: {rtti_missing[:20]}")
    return 0 if not missing and not rtti_missing else 1


if __name__ == "__main__":
    sys.exit(main())
