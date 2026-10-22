#!/usr/bin/env python3
"""Dump raw bytes around RFAB Online's mid-function patch sites from a
Skyrim SE 1.5.97 executable, so the correct 1.5.97 offsets can be derived
by disassembly instead of guessed.

Usage (on the machine that has the game):
    python3 tools/dump_patch_sites.py "C:\\...\\SkyrimSE.exe" > patch-sites.txt

Then send patch-sites.txt back for analysis. The tool only reads the file.

Notes:
- Function RVAs below come from the Address Library database shipped in RFAB
  (version-1-5-97-0.bin), cross-checked with tools/address_db_check.py.
- Steam CEG-protected executables store encrypted code on disk; the tool
  detects a high-entropy text section and tells you so. In that case run it
  on an unpacked copy of your own exe or dump the same RVAs from the running
  process memory with a debugger (see docs/phase-1-midfunction-audit.md).
"""
import math
import struct
import sys

# site: (label, address-library id, function RVA in 1.5.97, [(patch offset, length, kind)])
SITES = [
    ("BSGraphics::InitWindows dwStyle", 77226, 0xDEE380, [(0x174, 8, "Put WS_OVERLAPPEDWINDOW at +0x174+1")]),
    ("BSInput window/DirectInput cooperative level", 68781, 0xC50FA0, [(0x55, 8, "Put 3 at +0x55+2")]),
    ("BSGraphics timer StopTimer call", 77246, 0xDEFB40, [(9, 6, "SwapCall")]),
    ("BSTaskletManager ctor call", 69554, 0xC71D40, [(0x63, 6, "SwapCall")]),
    ("UI process AddToActiveQueue call", 82082, 0xF35EC0, [(0x682, 6, "SwapCall")]),
    ("Main init startup-movie jump", 36548, 0x5EA350, [(0xFE, 4, "Put 0xEB")]),
    ("FavoritesMenu CanProcess", 51538, 0x8BA1C0, [(0x15, 4, "Put 0x9090")]),
    ("StatsMenu ProcessMessage", 52510, 0x8F95C0, [(0x84E, 6, "Nop 6"), (0xA10, 4, "Nop 4"), (0x1040, 2, "Nop 2")]),
    ("StatsMenu controls", 52518, 0x8F9720, [(0x46, 4, "Nop 4"), (0x4A, 2, "Nop 2")]),
    ("Projectile guard jump", 34452, 0x56EF30, [(0x374, 16, "Jump out"), (0x379, 16, "Jump back target")]),
]
CONTEXT = 48  # bytes before/after each patch offset


def parse_pe(data):
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise SystemExit("not a PE file (no MZ header)")
    e_lfanew, = struct.unpack_from("<I", data, 0x3C)
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise SystemExit("not a PE file (no PE signature)")
    num_sections, = struct.unpack_from("<H", data, e_lfanew + 6)
    opt_size, = struct.unpack_from("<H", data, e_lfanew + 20)
    sec_off = e_lfanew + 24 + opt_size
    sections = []
    for i in range(num_sections):
        name = data[sec_off + i * 40: sec_off + i * 40 + 8].rstrip(b"\0").decode(errors="replace")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, sec_off + i * 40 + 8)
        sections.append((name, vaddr, vsize, raw_ptr, raw_size))
    return sections


def entropy(blob):
    if not blob:
        return 0.0
    counts = [0] * 256
    for b in blob:
        counts[b] += 1
    e = 0.0
    for c in counts:
        if c:
            p = c / len(blob)
            e -= p * math.log2(p)
    return e


def rva_to_off(sections, rva):
    for _, vaddr, vsize, raw_ptr, raw_size in sections:
        if vaddr <= rva < vaddr + max(vsize, raw_size):
            return raw_ptr + (rva - vaddr)
    return None


def hexdump(data, start_rva, blob):
    lines = []
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {start_rva + i:08X}  {hexs:<47}  {asc}")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()
    sections = parse_pe(data)
    print(f"file: {sys.argv[1]} ({len(data)} bytes)")
    print("sections:")
    for name, vaddr, vsize, raw_ptr, raw_size in sections:
        ent = entropy(data[raw_ptr:raw_ptr + min(raw_size, 1 << 20)])
        print(f"  {name:<10} RVA {vaddr:#010x} raw {raw_ptr:#x}+{raw_size:#x} entropy(1MiB) {ent:.2f}")
    text = next((s for s in sections if s[0].startswith(".text")), None)
    if text:
        sample = data[text[3]:text[3] + min(text[4], 1 << 20)]
        if entropy(sample) > 7.5:
            print("\nWARNING: .text looks encrypted/packed (Steam CEG?). On-disk bytes below")
            print("will be garbage. Re-run on an unpacked copy of your exe, or dump these")
            print("RVAs from the running process memory (see phase-1-midfunction-audit.md).")
    for label, aid, func_rva, patches in SITES:
        print(f"\n=== {label} (Address Library ID {aid}, function RVA {func_rva:#x}) ===")
        for off, length, kind in patches:
            rva = func_rva + off
            foff = rva_to_off(sections, max(func_rva, rva - CONTEXT))
            print(f"-- patch {kind} at function+{off:#x} (RVA {rva:#x}), context -/+{CONTEXT} bytes")
            if foff is None:
                print("   !! RVA not inside any section")
                continue
            start_rva = max(func_rva, rva - CONTEXT)
            blob = data[foff:foff + (rva - start_rva) + length + CONTEXT]
            print(hexdump(data, start_rva, blob))
    return 0


if __name__ == "__main__":
    sys.exit(main())
