#!/usr/bin/env python3
"""
Inspect and optionally patch an MO2 / game root directory of the drake1ch/RFAB
modpack for full compatibility with RFAB Online (Skyrim SE 1.5.97.0.8).

Mirrors Code/common/RfabModpackCompat.h:
  - Patches ALL EngineFixes.toml files across MO2/mods, MO2/overwrite, and Data/SKSE/Plugins
    (including v5/v6, v7 [MemoryManager] OverrideVanilla/OverrideScaleform, and partial MO2
    overrides like 'Disable Quick Saves/skse/plugins/EngineFixes.toml'):
      MemoryManager = false / OverrideVanilla = false
      ScaleformAllocator = false / OverrideScaleform = false
      RegularQuicksaves = true
      MaxStdio = 8192
      SaveGameMaxSize = true
  - Disables '[RFAB] Autosave.ini' (Enabled = false) so uncoordinated local autosaves cannot
    desynchronize multiplayer sessions while server-coordinated checkpoints handle saving.
  - Neutralizes 'SkyrimSoulsRE.ini' [SLOWMOTION] (fSlowMotionMultiplier = 1.000000,
    bEnableSlowMotion_* = false) while keeping all menus unpaused.
  - Hides incompatible 'BetterStealing.dll' -> 'BetterStealing.dll.mohidden' while preserving
    all other .NET Script Framework plugins.
  - Verifies 'po3_Tweaks.ini' ('Load EditorIDs = true') required by TESForm::LookupByEditorID.
"""

from __future__ import annotations

import argparse
import configparser
import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Tuple


def patch_toml_key(content: str, key: str, value: str) -> Tuple[str, bool, bool]:
    pattern = re.compile(rf"^(\s*{re.escape(key)}\s*=\s*)([^\r\n#;]+)(.*)$", re.MULTILINE | re.IGNORECASE)
    found = False
    changed = False

    def repl(match: re.Match[str]) -> str:
        nonlocal found, changed
        found = True
        old_val = match.group(2).strip()
        if old_val != value:
            changed = True
            return f"{match.group(1)}{value}{match.group(3)}"
        return match.group(0)

    updated = pattern.sub(repl, content)
    return updated, found, changed


def ensure_section_keys(content: str, section: str, entries: List[Tuple[str, str]]) -> Tuple[str, List[str]]:
    newline = "\r\n" if "\r\n" in content else "\n"
    diffs: List[str] = []
    missing: List[Tuple[str, str]] = []
    cur = content

    for key, val in entries:
        cur, found, changed = patch_toml_key(cur, key, val)
        if changed:
            diffs.append(f"{key} -> {val}")
        if not found:
            missing.append((key, val))

    if missing:
        sec_re = re.compile(rf"^(\s*\[{re.escape(section)}\][^\r\n]*\r?\n)", re.MULTILINE | re.IGNORECASE)
        add_block = "".join(f"{k} = {v}{newline}" for k, v in missing)
        m = sec_re.search(cur)
        if m:
            cur = cur[: m.end()] + add_block + cur[m.end() :]
        else:
            prefix = "" if (not cur or cur.endswith("\n")) else newline
            cur = f"{cur}{prefix}[{section}]{newline}{add_block}"
        for k, v in missing:
            diffs.append(f"+{section}.{k} = {v}")

    return cur, diffs


def patch_engine_fixes_toml(content: str) -> Tuple[str, List[str]]:
    diffs: List[str] = []
    cur = re.sub(r"(?m)^\s*(?:S\s*=\s*a|t\s*=\s*r|f\s*=\s*a)\s*\r?\n?", "", content)
    if cur != content:
        diffs.append("stripped invalid single-char TOML entries")
    has_v7_mm = bool(re.search(r"^\s*\[MemoryManager\]", cur, re.MULTILINE | re.IGNORECASE))

    if has_v7_mm:
        cur, d = ensure_section_keys(
            cur,
            "MemoryManager",
            [("OverrideVanilla", "false"), ("OverrideScaleform", "false")],
        )
        diffs.extend(d)

    patches_entries: List[Tuple[str, str]] = [
        ("FormCaching", "false"),
        ("TreeLODReferenceCaching", "false"),
        ("RegularQuicksaves", "true"),
        ("CleanSkseCosaves", "true"),
        ("MaxStdio", "8192"),
    ]
    if not has_v7_mm:
        patches_entries.insert(0, ("MemoryManager", "false"))
        patches_entries.insert(1, ("ScaleformAllocator", "false"))
    else:
        for k, v in [("MemoryManager", "false"), ("ScaleformAllocator", "false")]:
            cur, _, ch = patch_toml_key(cur, k, v)
            if ch:
                diffs.append(f"{k} -> {v}")

    cur, d = ensure_section_keys(cur, "Patches", patches_entries)
    diffs.extend(d)

    cur = re.sub(r"^\s*\[SaveGameMaxSize\]\s*$", "[Experimental]", cur, flags=re.MULTILINE | re.IGNORECASE)
    # CRITICAL: EngineFixes v5/v6 PatchSaveGameMaxSize() has no MemoryManager check and sets Win32FileType
    # buffer size to 128 MB while vanilla ScrapHeap stays 64 MB -> ScrapHeap::Allocate returns nullptr -> sSaveGameCorrupt!
    cur, d = ensure_section_keys(cur, "Experimental", [("SaveGameMaxSize", "false")])
    diffs.extend(d)
    return cur, diffs


def patch_rfab_autosave_ini(content: str) -> Tuple[str, bool]:
    cur = content
    any_changed = False
    for k, v in [
        ("Enabled", "false"),
        ("Enable autosave", "0"),
        ("Save after combat", "0"),
        ("Save on location change", "0"),
        ("Save on sleep/wait", "0"),
    ]:
        cur, _, ch = patch_toml_key(cur, k, v)
        any_changed = any_changed or ch
    return cur, any_changed


def patch_skyrim_souls_ini(content: str) -> Tuple[str, bool]:
    cur = content
    any_changed = False
    cur, _, ch = patch_toml_key(cur, "fSlowMotionMultiplier", "1.000000")
    any_changed = any_changed or ch
    cur, _, ch_stats = patch_toml_key(cur, "bStatsMenu", "false")
    any_changed = any_changed or ch_stats

    slow_re = re.compile(r"^(\s*bEnableSlowMotion[A-Za-z0-9_]*\s*=\s*)([^\r\n;#]+)(.*)$", re.MULTILINE | re.IGNORECASE)

    def repl(match: re.Match[str]) -> str:
        nonlocal any_changed
        if match.group(2).strip().lower() != "false":
            any_changed = True
            return f"{match.group(1)}false{match.group(3)}"
        return match.group(0)

    cur = slow_re.sub(repl, cur)
    return cur, any_changed


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
    parser.add_argument(
        "--bindings",
        type=Path,
        default=Path("Data/SKSE/Plugins/RFAB_Online_Bindings.json"),
        help="Path to RFAB_Online_Bindings.json manifest",
    )
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
            print(f"  - {tf}: {', '.join(diffs)}")
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
            print(f"  - {af}: Enabled -> false")
            if args.apply:
                af.write_text(patched, encoding="utf-8")
                print("    -> PATCHED")
        else:
            print(f"  - {af}: OK (already disabled)")

    souls_files = find_case_insensitive(args.root, "skyrimsoulsre.ini")
    print(f"\n[3] SkyrimSoulsRE.ini files found: {len(souls_files)}")
    for sf in souls_files:
        raw = sf.read_text(encoding="utf-8", errors="replace")
        patched, changed = patch_skyrim_souls_ini(raw)
        if changed:
            print(f"  - {sf}: [SLOWMOTION] -> fSlowMotionMultiplier=1.000000, bEnableSlowMotion*=false")
            if args.apply:
                sf.write_text(patched, encoding="utf-8")
                print("    -> PATCHED")
        else:
            print(f"  - {sf}: OK")

    bs_files = find_case_insensitive(args.root, "betterstealing.dll")
    print(f"\n[4] BetterStealing.dll check ({len(bs_files)} active file(s)):")
    for bf in bs_files:
        print(f"  - {bf}: incompatible hook on SE 36525")
        if args.apply:
            target = bf.with_name(bf.name + ".mohidden")
            bf.replace(target)
            print(f"    -> HIDDEN as {target.name}")

    po3_files = find_case_insensitive(args.root, "po3_tweaks.ini")
    print(f"\n[5] po3_Tweaks.ini EditorID cache check ({len(po3_files)} file(s)):")
    for pf in po3_files:
        raw = pf.read_text(encoding="utf-8", errors="replace")
        ok = bool(re.search(r"Load\s+EditorIDs\s*=\s*true", raw, re.IGNORECASE))
        print(f"  - {pf}: {'OK (Load EditorIDs = true)' if ok else 'WARNING: set Load EditorIDs = true!'}")

    su_summary = inspect_storage_util(args.root)
    print("\n[6] StorageUtilData live balance tables:")
    for k, v in sorted(su_summary.items()):
        print(f"  - {k}: {v}")

    if args.bindings.exists():
        manifest = json.loads(args.bindings.read_text(encoding="utf-8"))
        print(
            f"\n[7] External bindings manifest ({args.bindings}): schema v{manifest.get('schema_version')}, "
            f"{len(manifest.get('editor_ids', {}))} core EditorID bindings, "
            f"{len(manifest.get('synced_global_prefixes', []))} synced global prefixes, "
            f"{len(manifest.get('excluded_global_prefixes', []))} excluded local prefixes."
        )

    print("\nDone.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
