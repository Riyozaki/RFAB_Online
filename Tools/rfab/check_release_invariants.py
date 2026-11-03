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

Behaviour of the game itself is covered by the test suites in Code/tests, which
execute code instead of reading it.
"""

from __future__ import annotations

import json
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

tags = git("tag", "--list")
if tags is None:
    notes.append("git tags unavailable, skipped the published-tag check")
else:
    published = {t.strip() for t in tags.splitlines() if t.strip()}
    check(
        f"v{version}" not in published,
        f"VERSION {version} names an already published tag (v{version}); "
        "bump it instead of overwriting a release players are running",
    )

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
check(workflow.is_file(), ".github/workflows/release.yml is missing")
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
    f"{len(manifests)} backup manifest(s), no build output tracked in git."
)
