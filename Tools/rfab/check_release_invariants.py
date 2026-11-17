#!/usr/bin/env python3
"""Release invariants for RFAB Online.

This replaces test_v120_hotfix_regressions.py, which asserted that ~130 source
files contained specific substrings (parameter names, comments, call expressions).
Those assertions could not fail when the game broke, and they *did* fail every time
a file was legitimately rewritten - the release pipeline refused the very changes
that fixed it.

What is checked here are facts about the release itself, each one a failure mode
this project actually hit:

  1. VERSION is a semver number and does not name an already published tag,
     so a build can never overwrite a release players are using.
  2. No UI build output is tracked in git. A committed bundle used to shadow
     Code/skyrim_ui, so UI changes never reached players.
  3. The UI sources can be built: package.json declares the script the release
     pipeline calls, and the lockfile that makes the build reproducible exists.
  4. Release backup manifests parse and describe assets with sizes and digests.
  5. The release workflow does not hardcode a version tag, so VERSION stays the
     single source of truth.
  6. The co-op config overlay and the installer undo share one key list.
  7. The four Tilted* libraries are submodule links pinned to the revisions
     patch_submodules.py was written against. An import once dropped the links;
     nothing failed on Linux and the client could not be built at all.
  8. No line disabled on purpose is left behind. A fix is proven by breaking it and
     watching its test fail; the broken line carries a marker, and a marker that
     survives into a commit ships the breakage.

Behaviour of the game itself is covered by the test suites in Code/tests, which
execute code instead of reading it.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SEMVER = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")

failures: list[str] = []
notes: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def git(*args: str) -> str | None:
    try:
        out = subprocess.run(
            ["git", *args], cwd=ROOT, capture_output=True, text=True, timeout=60
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return out.stdout if out.returncode == 0 else None


# 1. VERSION ------------------------------------------------------------------
version_file = ROOT / "VERSION"
check(version_file.is_file(), "VERSION is missing from the repository root")

version = version_file.read_text(encoding="utf-8").strip() if version_file.is_file() else ""
check(bool(SEMVER.match(version)), f"VERSION must be semver, got {version!r}")

compat_header = ROOT / "Code" / "common" / "RfabModpackCompat.h"
check(compat_header.is_file(), "Code/common/RfabModpackCompat.h is missing")
if compat_header.is_file():
    compat_match = re.search(
        r'kRfabOnlineVersion\s*=\s*"([^"]+)"',
        compat_header.read_text(encoding="utf-8"),
    )
    compat_ver = compat_match.group(1) if compat_match else None
    check(
        compat_ver == version,
        f"RfabModpackCompat.h kRfabOnlineVersion ({compat_ver!r}) disagrees with VERSION ({version!r})",
    )

stub_build_info = ROOT / "Tools" / "rfab" / "linux-server" / "stub" / "BuildInfo.h"
if stub_build_info.is_file():
    stub_match = re.search(
        r'#define\s+BUILD_COMMIT\s+"([^"]+)"',
        stub_build_info.read_text(encoding="utf-8"),
    )
    stub_ver = stub_match.group(1) if stub_match else None
    check(
        stub_ver == f"v{version}",
        f"linux-server stub/BuildInfo.h BUILD_COMMIT ({stub_ver!r}) disagrees with v{version!r}",
    )

for ps1_rel in (
    "Tools/rfab/Setup-RfabOnline.ps1",
    "Tools/rfab/Build-CloudRelease.ps1",
    "Tools/rfab/Package-Release.ps1",
    "Tools/rfab/Deploy-RfabOnline.ps1",
    "Tools/rfab/Restore-ModConfigs.ps1",
):
    ps1_file = ROOT / ps1_rel
    if ps1_file.is_file():
        check(
            ps1_file.read_bytes()[:3] == b"\xef\xbb\xbf",
            f"{ps1_rel} is missing UTF-8 BOM (EF BB BF) required by Windows PowerShell 5.1",
        )

tags = git("tag", "--list")
if tags is None:
    notes.append("git tags unavailable, skipped the published-tag check")
else:
    published = {t.strip() for t in tags.splitlines() if t.strip()}
    if os.environ.get("RFAB_PUBLISHING_RELEASE", "1") == "1":
        check(
            f"v{version}" not in published,
            f"VERSION {version} names an already published tag (v{version}); "
            "bump it instead of overwriting a release players are running",
        )
    elif f"v{version}" in published:
        notes.append(f"v{version} already published; PublishRelease=false artifact-only build")

# 2. No build output in git ----------------------------------------------------
tracked = git("ls-files")
if tracked is None:
    notes.append("git index unavailable, skipped the tracked-artifact check")
else:
    files = tracked.splitlines()
    bundled_ui = [
        f
        for f in files
        if re.search(r"dist_ready/.*/UI/", f) or f.startswith("Code/skyrim_ui/dist/")
    ]
    check(
        not bundled_ui,
        "UI build output is tracked in git "
        f"({len(bundled_ui)} files, e.g. {bundled_ui[:2]}); the release builds it from "
        "Code/skyrim_ui instead, and a committed copy silently shadows the sources",
    )

# 3. The UI can be built -------------------------------------------------------
ui_root = ROOT / "Code" / "skyrim_ui"
pkg_path = ui_root / "package.json"
check(pkg_path.is_file(), "Code/skyrim_ui/package.json is missing")
if pkg_path.is_file():
    pkg = json.loads(pkg_path.read_text(encoding="utf-8"))
    scripts = pkg.get("scripts", {})
    check(
        "deploy:production" in scripts,
        "Code/skyrim_ui/package.json must declare the deploy:production script "
        "called by Build-CloudRelease.ps1 step [6/8]",
    )
    check(
        (ui_root / "pnpm-lock.yaml").is_file(),
        "pnpm-lock.yaml is missing: the UI build must be reproducible",
    )

# 4. Backup manifests ----------------------------------------------------------
manifests = sorted((ROOT / "docs").glob("*RELEASE_BACKUP_MANIFEST.json"))
check(bool(manifests), "no release backup manifests found under docs/")
for manifest_path in manifests:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        failures.append(f"{manifest_path.name} is not valid JSON: {exc}")
        continue

    assets = manifest.get("assets")
    check(
        isinstance(assets, list) and bool(assets),
        f"{manifest_path.name} declares no assets",
    )
    for asset in assets or []:
        name = asset.get("name", "<unnamed>")
        check(bool(asset.get("name")), f"{manifest_path.name}: asset without a name")
        check(
            isinstance(asset.get("size"), int) and asset["size"] > 0,
            f"{manifest_path.name}: {name} has no usable size",
        )
        # Older manifests were captured straight from the GitHub API and carry
        # "digest": "sha256:<hex>"; newer ones store the bare hex under "sha256".
        digest = str(asset.get("sha256") or asset.get("digest") or "")
        digest = digest.split(":", 1)[-1]
        check(
            bool(re.fullmatch(r"[0-9a-fA-F]{64}", digest)),
            f"{manifest_path.name}: {name} has no sha256 digest",
        )

# 5. The workflow does not pin a version --------------------------------------
workflow = ROOT / ".github" / "workflows" / "release.yml"
if not workflow.is_file():
    alt_workflow = ROOT / ".github" / "workflows-ready" / "release.yml"
    if alt_workflow.is_file():
        workflow = alt_workflow
check(workflow.is_file(), ".github/workflows/release.yml (or .github/workflows-ready/release.yml) is missing")
if workflow.is_file():
    # Only the command matters; the word may legitimately appear in a comment.
    commands = [
        line
        for line in workflow.read_text(encoding="utf-8").splitlines()
        if re.match(r"\s*(run:|-\s)", line) and "Build-CloudRelease" in line
    ]
    check(
        all("-VersionTag" not in line for line in commands),
        "release.yml passes -VersionTag to Build-CloudRelease.ps1; the tag must come "
        "from VERSION so the artifact and the release number cannot disagree",
    )

# 6. Co-op config overlay and installer undo share one key list -------------------
# Code/common/Compat/ConfigOverlay.h lists, per file, every key RFAB Online writes into the co-op
# copy (kEngineFixesKeys ...); Tools/rfab/Restore-ModConfigs.ps1 restores a mod file from its
# backup only when it differs in those keys. A key known to one side only would either leave an
# old edit in place or revert a user's change.
overlay_h = ROOT / "Code" / "common" / "Compat" / "ConfigOverlay.h"
restore_ps1 = ROOT / "Tools" / "rfab" / "Restore-ModConfigs.ps1"
check(overlay_h.is_file(), "Code/common/Compat/ConfigOverlay.h is missing")
check(restore_ps1.is_file(), "Tools/rfab/Restore-ModConfigs.ps1 is missing")
if overlay_h.is_file() and restore_ps1.is_file():
    header = overlay_h.read_text(encoding="utf-8")
    script = restore_ps1.read_text(encoding="utf-8-sig")
    for array_name, file_name in (
        ("kEngineFixesKeys", "EngineFixes.toml"),
        ("kSkyrimSoulsKeys", "SkyrimSoulsRE.ini"),
        ("kRfabAutosaveKeys", "[RFAB] Autosave.ini"),
        ("kPo3TweaksKeys", "po3_Tweaks.ini"),
        ("kRfabPluginIniKeys", "RFAB.ini"),
        ("kSseDisplayTweaksKeys", "SSEDisplayTweaks.ini"),
        ("kSseDisplayTweaksKeys", "SSEDisplayTweaks_Custom.ini"),
    ):
        cpp_match = re.search(
            r"inline constexpr std::string_view " + array_name + r"\[\]\s*=\s*\{(.*?)\};", header, re.S
        )
        ps_match = re.search(r'"' + re.escape(file_name) + r'"\s*=\s*@\(([^)]*)\)', script)
        check(cpp_match is not None, f"ConfigOverlay.h no longer declares {array_name}")
        check(ps_match is not None, f"Restore-ModConfigs.ps1 no longer lists keys for {file_name!r}")
        if cpp_match and ps_match:
            cpp_body = re.sub(r"//[^\n]*", "", cpp_match.group(1))
            cpp_keys = sorted(re.findall(r'"([^"]*)"', cpp_body))
            ps_keys = sorted(re.findall(r'"([^"]*)"', ps_match.group(1)))
            check(
                cpp_keys == ps_keys,
                f"managed keys for {file_name!r} differ: ConfigOverlay.h {array_name} has "
                f"{sorted(set(cpp_keys) - set(ps_keys))} only, Restore-ModConfigs.ps1 has "
                f"{sorted(set(ps_keys) - set(cpp_keys))} only",
            )
            check(
                all(k == k.lower() for k in cpp_keys),
                f"{array_name} keys must be lower case (compared case-insensitively on both sides)",
            )

# 7. Submodules are pinned ------------------------------------------------------
# Build-CloudRelease.ps1 step [1/8] runs `git submodule update --init` and then
# patch_submodules.py, which replaces exact source fragments of these libraries.
SUBMODULE_PINS = {
    "Libraries/TiltedConnect": "e6f5759364a64462e717db1153781a7eae26a13e",
    "Libraries/TiltedHooks": "d7c175b86975679eb05883c097d7eb1c2165226a",
    "Libraries/TiltedReverse": "55ee3f29c62fea247148e57b94ec4941055139ed",
    "Libraries/TiltedUI": "0bec46b174d730832cbe0a10e8162bf6b0d3d66b",
}
gitmodules = ROOT / ".gitmodules"
check(gitmodules.is_file(), ".gitmodules is missing")
if gitmodules.is_file():
    declared = set(re.findall(r"^\s*path\s*=\s*(\S+)\s*$", gitmodules.read_text(encoding="utf-8"), re.M))
    check(
        declared == set(SUBMODULE_PINS),
        f".gitmodules declares {sorted(declared)}, the pinned set is {sorted(SUBMODULE_PINS)}; "
        "update SUBMODULE_PINS in this script together with .gitmodules",
    )

staged = git("ls-files", "-s", "--", *SUBMODULE_PINS)
if staged is None:
    notes.append("git index unavailable, skipped the submodule pin check")
else:
    index_entries: dict[str, tuple[str, str]] = {}
    for line in staged.splitlines():
        meta, _, path = line.partition("\t")
        fields = meta.split()
        if len(fields) >= 2:
            index_entries[path] = (fields[0], fields[1])
    for path, pin in SUBMODULE_PINS.items():
        mode, sha = index_entries.get(path, ("", ""))
        check(
            mode == "160000",
            f"{path} is not a submodule link in the git index, so the Windows build has no sources "
            f"for it; restore with: git update-index --add --cacheinfo 160000,{pin},{path}",
        )
        if mode == "160000":
            check(
                sha == pin,
                f"{path} points at {sha}, expected {pin}; patch_submodules.py is written against "
                "the pinned revision - move the pin and the patches together",
            )

# 8. No deliberate breakage left behind -------------------------------------------
# The marker is spelled in two halves so that this file does not contain it.
BREAKAGE_MARKER = "MUTATION" + "-TEST"
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".inl", ".c", ".py", ".sh", ".ps1", ".bat", ".lua", ".ini", ".json"}
tracked = git("ls-files", "--cached", "--others", "--exclude-standard", "--", "Code", "Tools", "dist_ready")
if tracked is None:
    notes.append("git index unavailable, skipped the deliberate-breakage marker check")
else:
    for relative in tracked.splitlines():
        candidate = ROOT / relative
        if candidate.suffix.lower() not in SOURCE_SUFFIXES and candidate.name != "Makefile":
            continue
        try:
            text = candidate.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        if BREAKAGE_MARKER not in text:
            continue
        for number, line in enumerate(text.splitlines(), start=1):
            if BREAKAGE_MARKER in line:
                check(
                    False,
                    f"{relative}:{number} carries the {BREAKAGE_MARKER} marker: a line was disabled to "
                    "prove that a test can fail and was not restored",
                )

# -----------------------------------------------------------------------------
for note in notes:
    print(f"[release-invariants] note: {note}")

if failures:
    print(f"[release-invariants] FAILED ({len(failures)}):")
    for failure in failures:
        print(f"  - {failure}")
    sys.exit(1)

print(
    f"[release-invariants] OK: version {version}, "
    f"{len(manifests)} backup manifest(s), {len(SUBMODULE_PINS)} submodule pin(s), "
    "no build output tracked in git."
)
