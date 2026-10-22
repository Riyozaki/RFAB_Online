#!/usr/bin/env python3
"""
Inspect and optionally patch an MO2 / game root directory of the drake1ch/RFAB
modpack for full compatibility with RFAB Online (Skyrim SE 1.5.97.0.8).

Mirrors Code/common/RfabModpackCompat.h:
  - Inspects & optionally patches all EngineFixes.toml files (including MO2 override mods
    such as 'Disable Quick Saves/skse/plugins/EngineFixes.toml'):
      MemoryManager = false
      ScaleformAllocator = false
      RegularQuicksaves = true
      MaxStdio = 8192
  - Inspects & optionally disables '[RFAB] Autosave.ini' (RFAB.dll Autosave::MakeSave)
    so uncoordinated local autosaves cannot desynchronize multiplayer sessions.
  - Verifies 'po3_Tweaks.ini' ('Load EditorIDs = true') required by TESForm::LookupByEditorID.
  - Parses and summarizes live 'StorageUtilData/' tables (Experience.ini, Quests.ini,
    BossList.json, RaceList.json) and 'RFAB_Online_Bindings.json'.
"""

from __future__ import annotations

import argparse
import configparser
import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Tuple


def patch_toml_key(content: str, key: str, value: str) -> Tuple[str, bool]:
    newline = "\r\n" if "\r\n" in content else "\n"
    pattern = re.compile(rf"^(\s*{re.escape(key)}\s*=\s*)([^\r\n#]+)(.*)$", re.MULTILINE)
    changed = False

    def repl(match: re.Match[str]) -> str:
        nonlocal changed
        old_val = match.group(2).strip()
        if old_val != value:
            changed = True
            return f"{match.group(1)}{value}{match.group(3)}"
        return match.group(0)

    updated = pattern.sub(repl, content)
    return updated, changed


def patch_engine_fixes_toml(content: str) -> Tuple[str, List[str]]:
    diffs: List[str] = []
    rules = [
        ("MemoryManager", "false"),
        ("ScaleformAllocator", "false"),
        ("RegularQuicksaves", "true"),
        ("MaxStdio", "8192"),
    ]
    cur = content
    for key, val in rules:
        cur, changed = patch_toml_key(cur, key, val)
        if changed:
            diffs.append(f"{key} -> {val}")
    return cur, diffs


def patch_rfab_autosave_ini(content: str) -> Tuple[str, bool]:
    cur, changed = patch_toml_key(content, "Enabled", "false")
    return cur, changed


def find_case_insensitive(root: Path, filename_lower: str) -> List[Path]:
    hits: List[Path] = []
    if not root.exists():
        return hits
    for p in root.rglob("*"):
        if p.is_file() and p.name.lower() == filename_lower:
            hits.append(p)
    return sorted(hits)


def inspect_storage_util(root: Path) -> Dict[str, int]:
    summary: Dict[str, int] = {}
    exp_files = find_case_insensitive(root, "experience.ini")
    if exp_files:
        cp = configparser.ConfigParser(strict=False, inline_comment_prefixes=(";", "#"))
        cp.read_string(exp_files[0].read_text(encoding="utf-8", errors="replace"))
        for sect in cp.sections():
            if sect.lower() == "levels":
                summary["experience_levels"] = len(cp.items(sect))

    quest_files = find_case_insensitive(root, "quests.ini")
    if quest_files:
        cp = configparser.ConfigParser(strict=False, inline_comment_prefixes=(";", "#"))
        cp.read_string(quest_files[0].read_text(encoding="utf-8", errors="replace"))
        summary["quest_xp_entries"] = sum(len(cp.items(s)) for s in cp.sections())

    boss_files = find_case_insensitive(root, "bosslist.json")
    if boss_files:
        data = json.loads(boss_files[0].read_text(encoding="utf-8", errors="replace"))
        int_section = data.get("int", data)
        summary["boss_xp_entries"] = len(int_section) if isinstance(int_section, dict) else 0

    race_files = find_case_insensitive(root, "racelist.json")
    if race_files:
        data = json.loads(race_files[0].read_text(encoding="utf-8", errors="replace"))
        int_section = data.get("int", data)
        summary["race_xp_entries"] = len(int_section) if isinstance(int_section, dict) else 0

    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect and patch RFAB MO2/game directory for RFAB Online.")
    parser.add_argument("--root", type=Path, required=True, help="Path to MO2 modpack root or Skyrim SE Data/game root")
    parser.add_argument("--bindings", type=Path, default=Path("Data/SKSE/Plugins/RFAB_Online_Bindings.json"),
                        help="Path to RFAB_Online_Bindings.json manifest")
    parser.add_argument("--apply", action="store_true", help="Write compatible edits in-place (default: dry-run check)")
    args = parser.parse_args()

    if not args.root.exists():
        print(f"ERROR: Root directory does not exist: {args.root}", file=sys.stderr)
        return 1

    mode_str = "APPLY" if args.apply else "DRY-RUN CHECK"
    print(f"=== RFAB Online Modpack Compatibility Tool ({mode_str}) ===")
    print(f"Target root: {args.root}")

    engine_toml_files = find_case_insensitive(args.root, "enginefixes.toml")
    print(f"\n[1] EngineFixes.toml files found: {len(engine_toml_files)}")
    for tf in engine_toml_files:
        raw = tf.read_text(encoding="utf-8", errors="replace")
        patched, diffs = patch_engine_fixes_toml(raw)
        if diffs:
            print(f"  - {tf}: needs {', '.join(diffs)}")
            if args.apply:
                tf.write_text(patched, encoding="utf-8")
                print("    -> PATCHED")
        else:
            print(f"  - {tf}: OK (already compatible)")

    autosave_files = find_case_insensitive(args.root, "[rfab] autosave.ini")
    print(f"\n[2] [RFAB] Autosave.ini files found: {len(autosave_files)}")
    for af in autosave_files:
        raw = af.read_text(encoding="utf-8", errors="replace")
        patched, changed = patch_rfab_autosave_ini(raw)
        if changed:
            print(f"  - {af}: Enabled = true -> false (server handles post-combat checkpoints)")
            if args.apply:
                af.write_text(patched, encoding="utf-8")
                print("    -> PATCHED")
        else:
            print(f"  - {af}: OK (already disabled)")

    po3_files = find_case_insensitive(args.root, "po3_tweaks.ini")
    print(f"\n[3] po3_Tweaks.ini EditorID cache check ({len(po3_files)} file(s)):")
    for pf in po3_files:
        raw = pf.read_text(encoding="utf-8", errors="replace")
        ok = bool(re.search(r"Load\s+EditorIDs\s*=\s*true", raw, re.IGNORECASE))
        print(f"  - {pf}: {'OK (Load EditorIDs = true)' if ok else 'WARNING: set Load EditorIDs = true!'}")

    su_summary = inspect_storage_util(args.root)
    print("\n[4] StorageUtilData live balance tables:")
    for k, v in sorted(su_summary.items()):
        print(f"  - {k}: {v}")

    if args.bindings.exists():
        manifest = json.loads(args.bindings.read_text(encoding="utf-8"))
        print(
            f"\n[5] External bindings manifest ({args.bindings}): schema v{manifest.get('schema_version')}, "
            f"{len(manifest.get('editor_ids', {}))} core EditorID bindings, "
            f"{len(manifest.get('synced_global_prefixes', []))} synced global prefixes, "
            f"{len(manifest.get('excluded_global_prefixes', []))} excluded local prefixes."
        )

    print("\nDone.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
