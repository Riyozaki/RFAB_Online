#!/usr/bin/env python3
"""Recover 1.5.97 address ids from the fork's own history.

The client was a Skyrim SE 1.5.97 mod until November 2021 and was then moved to
Skyrim AE 1.6.x.  The AE migration is plain to see in the history:

    -    POINTER_SKYRIMSE(TAnimationStep, PerformIdleAction, 0x140661070 - 0x140000000);
    +    POINTER_SKYRIMSE(TAnimationStep, PerformIdleAction, 38952);

Those removed `0x14......` values are *AE* addresses, so they cannot be used for
the port.  The tree *before* the migration however used 1.5.97 addresses for the
same symbols.  Looking those addresses up in the 1.5.97 Address Library database
gives the SE id, which is exactly the pair the port needs.

Matching is done on the pointer *type* (`TEquip`, `TUnEquipShout`, ...), because
the variable name is reused for whole families of functions (`s_equipFunc` is
four different addresses).  Only when a type is missing from the old tree the
variable name is used as a fallback.

Because the Address Library database is a complete id -> offset table, a lookup
is only a candidate: the result is accepted only when every occurrence of the
type resolves to the same SE id.  Everything else is reported as ambiguous and
left for manual reversing.

Usage:
    python tools/port1597/history_pairs.py \
        --repo ~/.ref/te-rwf --database ~/.ref/rfab/al-1-5-97.bin
"""

from __future__ import annotations

import argparse
import collections
import csv
import pathlib
import re
import struct
import subprocess

# The tree just before the switch to Skyrim AE: still uses 1.5.97 addresses.
DEFAULT_SE_COMMIT = "03c16aa97d9b943fe8184dcce96f59a3b9ffcecf"
# "feat: Use address library instead of hardcoded addresses" (2022-02-12):
# every hardcoded address was replaced with an Address Library id.
DEFAULT_CONVERSION_COMMIT = "6b2afebce3cf1545137c84adfd75ae4840828625"

DEFAULT_WORKLIST = "docs/phase1/id_worklist.csv"
DEFAULT_OUT = "docs/phase1/id_pairs_derived.csv"
DEFAULT_CURATED = "docs/phase1/id_pairs_curated.csv"

# Two spellings are used by the pre-AE tree:
#   POINTER_SKYRIMSE(T, name, 0x14063B060 - 0x140000000)   absolute address
#   POINTER_SKYRIMSE(T, name, 0x63B060)                    relative address
RAW_RVA_ABSOLUTE = re.compile(
    r"POINTER_SKYRIMSE\s*\(\s*([\w:*&]+)\s*,\s*(\w+)\s*,\s*0x14([0-9A-Fa-f]{6,7})\s*-\s*0x140000000\s*\)"
)
RAW_RVA_RELATIVE = re.compile(
    r"POINTER_SKYRIMSE\s*\(\s*([\w:*&]+)\s*,\s*(\w+)\s*,\s*0x([0-9A-Fa-f]{5,7})\s*\)"
)
CONVERTED = re.compile(
    r"POINTER_SKYRIMSE\s*\(\s*([\w:*&]+)\s*,\s*(\w+)\s*,\s*(\d{1,6}|0x14[0-9A-Fa-f]{6,7})"
)


def git(repo: pathlib.Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), *args], check=True, capture_output=True, text=True
    ).stdout


def ensure_commit(repo: pathlib.Path, commit: str) -> None:
    if subprocess.run(
        ["git", "-C", str(repo), "cat-file", "-e", f"{commit}^{{commit}}"], capture_output=True
    ).returncode:
        raise SystemExit(
            f"commit {commit} is not available in {repo}; fetch the full history "
            "(git fetch --unshallow) or pass --se-commit/--conversion-commit"
        )


def load_version_file(path: pathlib.Path) -> dict[int, int]:
    """Parse a sparse (format 1 or 2) Address Library database into offset -> id."""
    data = path.read_bytes()
    position = 0

    def read(fmt: str) -> tuple:
        nonlocal position
        value = struct.unpack_from(fmt, data, position)
        position += struct.calcsize(fmt)
        return value

    (format_version,) = read("<i")
    if format_version not in (1, 2):
        raise SystemExit(f"unsupported database format {format_version} (expected 1 or 2)")

    version = read("<4i")
    (name_length,) = read("<i")
    name = data[position : position + name_length].decode("ascii", "replace")
    position += name_length
    (pointer_size,) = read("<i")
    (count,) = read("<i")

    offset_to_id: dict[int, int] = {}
    previous_id = 0
    previous_offset = 0

    for _ in range(count):
        (record_type,) = read("<B")
        id_kind = record_type & 0x0F
        offset_kind = record_type >> 4

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
        offset_to_id[offset] = address_id

    print(f"database: {name} {'.'.join(map(str, version))}, {len(offset_to_id)} entries")
    return offset_to_id


TreeScan = collections.namedtuple("TreeScan", "by_type by_variable ids_by_type ids_by_variable locations")


def scan_se_tree(repo: pathlib.Path, commit: str, address_to_id: dict[int, int]) -> TreeScan:
    """Collect type/variable -> 1.5.97 address (and -> SE id when the address is known)."""
    files = [
        name
        for name in git(repo, "ls-tree", "-r", "--name-only", commit, "Code/client").splitlines()
        if name.endswith((".cpp", ".h"))
    ]

    by_type: dict[str, set[int]] = collections.defaultdict(set)
    by_variable: dict[str, set[int]] = collections.defaultdict(set)
    ids_by_type: dict[str, set[int]] = collections.defaultdict(set)
    ids_by_variable: dict[str, set[int]] = collections.defaultdict(set)
    locations: dict[tuple[str, str, int], str] = {}

    for name in files:
        content = git(repo, "show", f"{commit}:{name}")
        for pattern in (RAW_RVA_ABSOLUTE, RAW_RVA_RELATIVE):
            for match in pattern.finditer(content):
                address = int("0x" + match.group(3), 16)
                # The relative form also matches unrelated constants; a code
                # offset is always below the image size, everything else is not.
                if address >= 0x4000000:
                    continue

                type_name, variable = match.group(1), match.group(2)
                line = content.count("\n", 0, match.start()) + 1
                where = f"{name}:{line}"

                by_type[type_name].add(address)
                by_variable[variable].add(address)
                locations.setdefault(("type", type_name, address), where)
                locations.setdefault(("var", variable, address), where)

                if address in address_to_id:
                    ids_by_type[type_name].add(address_to_id[address])
                    ids_by_variable[variable].add(address_to_id[address])

    print(
        f"1.5.97 tree {commit[:8]}: {len(files)} files, {len(by_type)} types, "
        f"{len(ids_by_type)} resolvable types"
    )
    return TreeScan(by_type, by_variable, ids_by_type, ids_by_variable, locations)


def scan_conversion(repo: pathlib.Path, commit: str) -> tuple[dict[str, set[int]], dict[str, set[int]]]:
    """Collect type/variable -> AE id from the commit that replaced addresses with ids."""
    ids_by_type: dict[str, set[int]] = collections.defaultdict(set)
    ids_by_variable: dict[str, set[int]] = collections.defaultdict(set)

    for line in git(repo, "show", commit).splitlines():
        if not line.startswith("+"):
            continue
        match = CONVERTED.search(line)
        if match and not match.group(3).startswith("0x"):
            ids_by_type[match.group(1)].add(int(match.group(3)))
            ids_by_variable[match.group(2)].add(int(match.group(3)))

    print(f"conversion commit {commit[:8]}: {len(ids_by_type)} types with ids")
    return ids_by_type, ids_by_variable


def build_candidates(se: TreeScan, converted: tuple[dict[str, set[int]], dict[str, set[int]]]):
    """AE id -> {SE id: {"keys": {...}, "addresses": {...}}} using the type join first."""
    ids_by_type, ids_by_variable = converted
    candidates: dict[int, dict[int, dict]] = collections.defaultdict(dict)

    def join(id_table, kind, se_ids, address_table, target):
        for name, ids in id_table.items():
            for se_id in se_ids.get(name, ()):
                for ae_id in ids:
                    entry = target[ae_id].setdefault(se_id, {"keys": set(), "addresses": set()})
                    entry["keys"].add(f"{kind}:{name}")
                    entry["addresses"].update(address_table.get(name, ()))

    # Type names identify a function; variable names repeat across a family.
    join(ids_by_type, "type", se.ids_by_type, se.by_type, candidates)

    # Variable names are only a fallback for ids that no type resolved.
    by_variable: dict[int, dict[int, dict]] = collections.defaultdict(dict)
    join(ids_by_variable, "var", se.ids_by_variable, se.by_variable, by_variable)

    for ae_id, options in by_variable.items():
        if ae_id not in candidates:
            candidates[ae_id] = options

    return candidates


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=pathlib.Path, required=True)
    parser.add_argument("--database", type=pathlib.Path, required=True)
    parser.add_argument("--se-commit", default=DEFAULT_SE_COMMIT)
    parser.add_argument("--conversion-commit", default=DEFAULT_CONVERSION_COMMIT)
    parser.add_argument("--worklist", type=pathlib.Path, default=pathlib.Path(DEFAULT_WORKLIST))
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path(DEFAULT_OUT))
    parser.add_argument("--curated", type=pathlib.Path, default=pathlib.Path(DEFAULT_CURATED))
    parser.add_argument("--check", action="store_true", help="fail if the curated pairs drifted from the derivation")
    args = parser.parse_args()

    ensure_commit(args.repo, args.se_commit)
    ensure_commit(args.repo, args.conversion_commit)

    offset_to_id = load_version_file(args.database)
    se = scan_se_tree(args.repo, args.se_commit, offset_to_id)
    converted = scan_conversion(args.repo, args.conversion_commit)

    worklist = [int(row["ae_id"]) for row in csv.DictReader(args.worklist.open(encoding="utf-8"))]
    worklist_set = set(worklist)

    candidates = build_candidates(se, converted)

    report = []
    for ae_id in sorted(candidates):
        if ae_id not in worklist_set:
            continue

        options = candidates[ae_id]
        if len(options) == 1:
            se_id, entry = next(iter(options.items()))
            status = "unique"
            evidence = ", ".join(
                f"{key}@0x{address:X}" for key in sorted(entry["keys"]) for address in sorted(entry["addresses"])
            )
            notes = ""
        else:
            se_id = ""
            status = "ambiguous"
            evidence = ""
            notes = "кандидаты: " + ", ".join(
                f"{candidate} ({', '.join(sorted(entry['keys']))})"
                for candidate, entry in sorted(options.items())
            )

        report.append(
            {
                "ae_id": ae_id,
                "se_1_5_97_id": se_id,
                "status": status,
                "keys": ", ".join(sorted({key for entry in options.values() for key in entry["keys"]})),
                "evidence": evidence,
                "notes": notes,
            }
        )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(report[0].keys()) if report else [], lineterminator="\n")
        writer.writeheader()
        writer.writerows(report)

    if args.check:
        curated = {
            int(row["ae_id"]): int(row["se_1_5_97_id"])
            for row in csv.DictReader(args.curated.open(encoding="utf-8"))
            if row.get("se_1_5_97_id")
        }
        problems = []
        for row in report:
            ae_id = int(row["ae_id"])
            if row["status"] != "unique":
                continue
            if curated.get(ae_id) != int(row["se_1_5_97_id"]):
                problems.append(f"ae {ae_id}: derived {row['se_1_5_97_id']}, curated {curated.get(ae_id, 'missing')}")
        if problems:
            for problem in problems:
                print("DRIFT " + problem)
            return 1
        unique = sum(1 for row in report if row["status"] == "unique")
        print(f"curated pairs agree with the derivation ({unique} automatic, {len(curated)} curated)")
        return 0

    unique = [row for row in report if row["status"] == "unique"]
    ambiguous = [row for row in report if row["status"] == "ambiguous"]

    print(f"worklist ids: {len(worklist_set)}, derived: {len(report)} "
          f"(unique {len(unique)}, ambiguous {len(ambiguous)})")
    for row in unique:
        print(f"  {row['ae_id']:>6} -> {row['se_1_5_97_id']:>6}  {row['evidence']}")
    for row in ambiguous:
        print(f"  {row['ae_id']:>6} -> ?       {row['notes']}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
