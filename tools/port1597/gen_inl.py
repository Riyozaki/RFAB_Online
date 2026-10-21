#!/usr/bin/env python3
"""Build the AE(1.6.x) -> SE(1.5.97) Address Library id table used by the port.

Inputs
------
* Code/client/**                every Address Library id the client links against
* docs/phase0/id_map_1_5_97.csv  manual/heuristic reversal from Phase 0
* docs/phase1/clibng_id_pairs.csv  exact pairs harvested from CommonLibSSE-NG
* docs/phase1/id_pairs_curated.csv  pairs recovered from the 1.5.97 tree / intervals

Priority: curated > CommonLibSSE-NG > Phase 0 map. Where the two disagree the clibng value
wins and the conflict is listed in the generated inventory for review; Phase 0
mapped 38894 and 38897 onto the same id, which is a strong hint that its
heuristic picked a neighbouring function.

Outputs
-------
* Code/common/address_library/SkyrimIdMap1597.inl   (ae_id, se_id|0)
* docs/phase1/address_id_inventory.csv              (coverage + provenance)

Id 0 means "known but not reversed yet": the client still starts, and the
startup audit reports the complete list of missing ids.

Usage:
    python tools/port1597/gen_inl.py [--check]
"""

from __future__ import annotations

import argparse
import csv
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLIENT_ROOT = ROOT / "Code" / "client"
PHASE0_CSV = ROOT / "docs" / "phase0" / "id_map_1_5_97.csv"
CLIBNG_CSV = ROOT / "docs" / "phase1" / "clibng_id_pairs.csv"
HISTORY_CSV = ROOT / "docs" / "phase1" / "id_pairs_curated.csv"
INL_PATH = ROOT / "Code" / "common" / "address_library" / "SkyrimIdMap1597.inl"
INVENTORY_PATH = ROOT / "docs" / "phase1" / "address_id_inventory.csv"

PATTERNS = {
    # POINTER_SKYRIMSE(TAnimationStep, PerformIdleAction, 38952);
    "pointer": re.compile(r"POINTER_SKYRIMSE\s*\([^;]*?,\s*(\d+)\s*\)\s*;", re.S),
    # VersionDbPtr<uint8_t> ProcessHook(82082);
    "versiondb": re.compile(r"VersionDbPtr\s*<[^;>]*>\s*\w+\s*\(\s*(\d+)\s*\)\s*;"),
    # internal::RttiLocator<TESForm> registerRtti_TESForm(392216);
    "rtti": re.compile(r"registerRtti_\w+\s*\(\s*(\d+)\s*\)"),
}

SOURCE_PRIORITY = {"curated": 3, "clibng": 2, "phase0": 1, "quarantined": 0, "none": 0}

# The Phase 0 name heuristic mapped several AE ids onto one SE id. Two different
# functions cannot share an address, so these pairs are wrong by construction and
# are dropped instead of shipping a known-bad offset (the startup audit then
# reports the id as missing, which is a fixable state).
# Every id that Phase 0 collided has since been recovered from the 1.5.97 tree
# (docs/phase1/id_pairs_curated.csv), so the list is empty. The mechanism stays:
# if a future heuristic maps two AE ids onto one SE id, it must be dropped here.
QUARANTINE: dict[int, str] = {}


def scan_client() -> dict[int, dict]:
    """ae_id -> {kind, refs: [(file, line)], }"""
    found: dict[int, dict] = {}
    for path in sorted(CLIENT_ROOT.rglob("*")):
        if path.suffix not in {".cpp", ".h"}:
            continue
        relative = path.relative_to(ROOT).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_number, line in enumerate(text.splitlines(), 1):
            for kind, pattern in PATTERNS.items():
                for match in pattern.finditer(line):
                    ae = int(match.group(1))
                    entry = found.setdefault(ae, {"kind": kind, "refs": []})
                    entry["refs"].append(f"{relative}:{line_number}")
    return found


def load_phase0() -> tuple[dict[int, int], list[int]]:
    mapping: dict[int, int] = {}
    ambiguous: list[int] = []
    candidates: dict[int, set[int]] = {}
    if not PHASE0_CSV.exists():
        return mapping, ambiguous

    for row in csv.DictReader(PHASE0_CSV.open(encoding="utf-8")):
        if row["confidence"] != "A" or not row["se_1_5_97_id"]:
            candidates.setdefault(int(row["ae_id"]), set())
            continue
        candidates.setdefault(int(row["ae_id"]), set()).add(int(row["se_1_5_97_id"]))

    for ae, values in candidates.items():
        if len(values) != 1:
            ambiguous.append(ae)
            continue
        se = next(iter(values))
        if se == ae:
            ambiguous.append(ae)
            continue
        mapping[ae] = se
    return mapping, ambiguous


def load_clibng() -> dict[int, int]:
    mapping: dict[int, int] = {}
    if not CLIBNG_CSV.exists():
        return mapping

    for row in csv.DictReader(CLIBNG_CSV.open(encoding="utf-8")):
        ae = int(row["ae_id"])
        se = int(row["se_1_5_97_id"])
        if se and se != ae:
            mapping[ae] = se
    return mapping


def load_history() -> dict[int, int]:
    """Pairs recovered from the pre-1.6.x tree plus the 1.5.97 Address Library.

    These are the strongest evidence available offline: the symbol carried a real
    1.5.97 address that the Address Library database resolves to exactly one id.
    """
    mapping: dict[int, int] = {}
    if not HISTORY_CSV.exists():
        return mapping

    for row in csv.DictReader(HISTORY_CSV.open(encoding="utf-8")):
        if not row.get("ae_id") or not row.get("se_1_5_97_id"):
            continue
        ae = int(row["ae_id"])
        se = int(row["se_1_5_97_id"])
        if se and se != ae:
            mapping[ae] = se
    return mapping


def build() -> tuple[dict[int, tuple[int, str]], list[str], dict[int, dict]]:
    used = scan_client()
    phase0, phase0_ambiguous = load_phase0()
    clibng = load_clibng()
    history = load_history()

    resolved: dict[int, tuple[int, str]] = {}
    conflicts: list[str] = []

    for ae in used:
        if ae in QUARANTINE and ae not in history and ae not in clibng:
            resolved[ae] = (0, "quarantined")
            conflicts.append(f"ae {ae}: {QUARANTINE[ae]}")
            continue

        from_history = history.get(ae)
        from_clibng = clibng.get(ae)
        from_phase0 = phase0.get(ae)

        if from_history and from_clibng and from_history != from_clibng:
            conflicts.append(f"ae {ae}: curated {from_history} vs clibng {from_clibng} (curated wins)")
        if from_history and from_phase0 and from_history != from_phase0:
            conflicts.append(f"ae {ae}: curated {from_history} vs phase0 {from_phase0} (curated wins)")
        if not from_history and from_clibng and from_phase0 and from_clibng != from_phase0:
            conflicts.append(f"ae {ae}: clibng {from_clibng} vs phase0 {from_phase0} (clibng wins)")

        if from_history:
            resolved[ae] = (from_history, "curated")
        elif from_clibng:
            resolved[ae] = (from_clibng, "clibng")
        elif from_phase0:
            resolved[ae] = (from_phase0, "phase0")
        else:
            resolved[ae] = (0, "none")

    # A phase 0 id that was ambiguous (the heuristic found two candidate SE ids)
    # and is now resolved by a stronger source is expected, not a conflict; only
    # count them for the report.
    if phase0_ambiguous:
        resolved_ambiguous = sum(
            1 for ae in phase0_ambiguous if ae in used and resolved.get(ae, (0,))[0]
        )
        print(f"phase0 ambiguous ids resolved by a better source: {resolved_ambiguous}/{len(phase0_ambiguous)}")

    return resolved, conflicts, used


def render_inl(resolved: dict[int, tuple[int, str]]) -> str:
    mapped = sum(1 for se, _ in resolved.values() if se)
    lines = [
        "// Generated by tools/port1597/gen_inl.py -- do not edit by hand.",
        "// Sources: docs/phase1/id_pairs_curated.csv (1.5.97 tree, strongest),",
        "//          docs/phase1/clibng_id_pairs.csv (CommonLibSSE-NG),",
        "//          docs/phase0/id_map_1_5_97.csv (fallback).",
        f"// Entries: {len(resolved)}  mapped: {mapped}  unmapped: {len(resolved) - mapped}",
        "// clang-format off",
    ]
    for ae in sorted(resolved):
        lines.append(f"    {{{ae}u, {resolved[ae][0]}u}},")
    lines.append("// clang-format on")
    return "\n".join(lines) + "\n"


def render_inventory(resolved: dict[int, tuple[int, str]], used: dict[int, dict]) -> str:
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(["ae_id", "se_1_5_97_id", "source", "kind", "ref_count", "first_reference"])
    for ae in sorted(resolved):
        se, source = resolved[ae]
        info = used[ae]
        writer.writerow([ae, se if se else "", source, info["kind"], len(info["refs"]), info["refs"][0]])
    return buffer.getvalue()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail if generated files are stale")
    args = parser.parse_args()

    resolved, conflicts, used = build()
    inl_text = render_inl(resolved)
    inventory_text = render_inventory(resolved, used)

    mapped = sum(1 for se, _ in resolved.values() if se)
    by_source: dict[str, int] = {}
    for se, source in resolved.values():
        by_source[source] = by_source.get(source, 0) + 1

    print(f"ids in client: {len(resolved)}  mapped: {mapped}  unmapped: {len(resolved) - mapped}")
    print("by source: " + ", ".join(f"{k}={v}" for k, v in sorted(by_source.items())))
    if conflicts:
        print(f"source conflicts ({len(conflicts)}):")
        for conflict in conflicts:
            print("  " + conflict)

    unmapped = sorted(ae for ae, (se, _) in resolved.items() if not se)
    if unmapped:
        print(f"needs manual reversal ({len(unmapped)}): {unmapped}")

    if args.check:
        stale = []
        for path, text in ((INL_PATH, inl_text), (INVENTORY_PATH, inventory_text)):
            current = path.read_text(encoding="utf-8") if path.exists() else ""
            if current != text:
                stale.append(path)
        if stale:
            for path in stale:
                print(f"{path.relative_to(ROOT)} is out of date", file=sys.stderr)
            return 1
        return 0

    INL_PATH.parent.mkdir(parents=True, exist_ok=True)
    INL_PATH.write_text(inl_text, encoding="utf-8")
    INVENTORY_PATH.parent.mkdir(parents=True, exist_ok=True)
    INVENTORY_PATH.write_text(inventory_text, encoding="utf-8")
    print(f"wrote {INL_PATH.relative_to(ROOT)}")
    print(f"wrote {INVENTORY_PATH.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
