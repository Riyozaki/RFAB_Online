#!/usr/bin/env python3
"""Compare struct layout asserts of the current tree against the last pre-AE
Skyrim SE 1.5.97 commit of the upstream history.

The imported history contains the commit where TiltedEvolution still targeted
Skyrim SE 1.5.97 (the parent of the "AE sigma grindset" migration). Every
`static_assert(offsetof(...))` / `static_assert(sizeof(...))` in that commit is
an upstream-verified statement about the 1.5.97 layouts, written by people who
had the real binary. Comparing it with our port is therefore a *free* accuracy
check that needs neither the game nor a disassembler.

Usage:
    python3 tools/struct_layout_diff.py [--ref <commit>] [--root <repo>]

Exit code 0 always (report only); use --fail-on-regression to make it non-zero
when a layout present in both trees disagrees.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import defaultdict

ASSERT_RE = re.compile(
    r"static_assert\s*\(\s*(offsetof|sizeof)\s*\(\s*([A-Za-z_][\w:]*)"
    r"(?:\s*,\s*([A-Za-z_]\w*))?\s*\)\s*==\s*(0x[0-9A-Fa-f]+|\d+)"
)


def git_show(root: str, ref: str, path: str) -> str | None:
    try:
        out = subprocess.run(
            ["git", "-C", root, "show", f"{ref}:{path}"],
            check=True, capture_output=True, text=True,
        )
        return out.stdout
    except subprocess.CalledProcessError:
        return None


def read_current(root: str, path: str) -> str | None:
    """Read the working tree version of a file (so uncommitted fixes count)."""
    try:
        with open(f"{root}/{path}", "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return None


def git_ls(root: str, ref: str, prefix: str) -> list[str]:
    out = subprocess.run(
        ["git", "-C", root, "ls-tree", "-r", "--name-only", ref, prefix],
        check=True, capture_output=True, text=True,
    )
    return [p for p in out.stdout.splitlines() if p.endswith((".h", ".hpp"))]


def parse_asserts(text: str) -> dict[tuple[str, str, str], int]:
    found: dict[tuple[str, str, str], int] = {}
    for kind, struct, member, value in ASSERT_RE.findall(text):
        found[(kind, struct, member or "")] = int(value, 0)
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--ref", default="5c5837f6",
                    help="last pre-AE (Skyrim SE 1.5.97) upstream commit")
    ap.add_argument("--prefix", default="Code/client/Games/Skyrim")
    ap.add_argument("--fail-on-regression", action="store_true")
    args = ap.parse_args()

    try:
        ref_files = set(git_ls(args.root, args.ref, args.prefix))
    except subprocess.CalledProcessError:
        print(
            f"reference commit {args.ref} is not present in this clone.\n"
            f"Fetch the upstream history first, e.g.:\n"
            f"  git fetch origin --depth=300\n"
            f"or pass another --ref. Without a reference the layout check cannot run.",
            file=sys.stderr,
        )
        return 2
    cur_files = set(git_ls(args.root, "HEAD", args.prefix))
    import os
    cur_files = {p for p in cur_files if os.path.exists(f"{args.root}/{p}")}

    same = 0
    differing: list[str] = []
    missing_in_port: list[str] = []
    renames: list[str] = []
    no_history = 0

    for path in sorted(ref_files):
        ref_text = git_show(args.root, args.ref, path)
        if ref_text is None:
            continue
        ref_asserts = parse_asserts(ref_text)
        if not ref_asserts:
            continue
        if path not in cur_files:
            continue
        cur_text = read_current(args.root, path)
        if cur_text is None:
            no_history += 1
            continue
        cur_asserts = parse_asserts(cur_text)

        for key, ref_value in sorted(ref_asserts.items()):
            kind, struct, member = key
            label = f"{path}: {kind}({struct}{', ' + member if member else ''})"
            if key not in cur_asserts:
                # A member may simply have been renamed while keeping its offset;
                # that is harmless, so separate it from a real dropped assert.
                renamed = any(
                    k[0] == kind and k[1] == struct and v == ref_value
                    for k, v in cur_asserts.items()
                )
                if renamed:
                    renames.append(f"{label} = 0x{ref_value:X} (renamed, offset kept)")
                else:
                    missing_in_port.append(f"{label} = 0x{ref_value:X} (assert absent in port)")
                continue
            cur_value = cur_asserts[key]
            if cur_value == ref_value:
                same += 1
            else:
                differing.append(
                    f"{label}: port 0x{cur_value:X} != 1.5.97 0x{ref_value:X}"
                    f" (delta {cur_value - ref_value:+d})"
                )

    # Structs that only exist in the port (magic/combat/... added after AE);
    # there is no pre-AE reference for them at all.
    only_in_port = sorted(cur_files - ref_files)

    print(f"reference commit: {args.ref} (last pre-AE / Skyrim SE 1.5.97 state)")
    print(f"files compared:   {len(ref_files & cur_files)}")
    print(f"asserts matching: {same}")
    print(f"asserts differing: {len(differing)}")
    print(f"asserts renamed (offset kept): {len(renames)}")
    print(f"asserts absent in port: {len(missing_in_port)}")
    print(f"headers only in port (no 1.5.97 reference): {len(only_in_port)}")

    if differing:
        print("\n== LAYOUT REGRESSIONS (must be fixed) ==")
        for line in differing:
            print("  " + line)
    if renames:
        print("\n== RENAMED MEMBERS (same offset, different name - ok) ==")
        for line in renames:
            print("  " + line)
    if missing_in_port:
        print("\n== ASSERTS DROPPED BY THE PORT (review each) ==")
        for line in missing_in_port:
            print("  " + line)
    if only_in_port:
        print("\n== HEADERS WITH NO PRE-AE REFERENCE (need in-game/exe audit) ==")
        for path in only_in_port:
            print("  " + path)

    if differing and args.fail_on_regression:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
