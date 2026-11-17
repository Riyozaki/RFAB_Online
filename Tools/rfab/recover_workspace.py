#!/usr/bin/env python3
"""Bring a restored working copy back in line with the branch on the server, without losing work.

Why this exists
---------------
The development sandbox can be reset in the middle of a session. What comes back is a snapshot of the
files plus a fresh shallow clone: the branch points at the first commit of the repository, the commits
made during the session are only on the server. This happened twice in one day (2026-11-14).

The obvious recipe - "move the branch to the server tip, then commit whatever differs" - is only right
when the snapshot is at least as new as the server. The second time it was not: the snapshot was seven
commits OLDER than the server, so every difference was an old version of a file or a file that did not
exist yet. Committing "whatever differs" would have deleted two stages of work and pushed the deletion.

So the question to answer before touching anything is, for every file that differs from the server:
is this content the server has never seen, or is it merely an older version?

  stale      the file equals an earlier version of itself in the server's history  -> safe to replace
  missing    the file exists on the server and not here                            -> safe to restore
  artifact   a binary the snapshot mechanism is known to damage (ARTIFACT_GLOBS)   -> safe to replace
  LOCAL      the content appears nowhere in the server's history of that file      -> unpushed work
  untracked  a file the server does not have at all (never touched by a restore)   -> listed only

Report mode (the default) fetches and compares through a temporary index: HEAD, the real index and the
files are not modified. --apply moves the branch to the server tip and restores the stale, missing and
artifact files. It refuses when there is LOCAL content or when this clone has commits the server lacks,
because then the working copy, not the server, holds the newest work.

Usage:  recover_workspace.py                 report only
        recover_workspace.py --apply         restore, if and only if nothing here is newer than the server
        recover_workspace.py --self-test     prove every verdict on throwaway repositories

Exit code: 0 in line with the server (or brought in line); 2 local work present, nothing was changed;
1 the comparison itself failed.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# Binaries the snapshot mechanism has been seen to damage on every restore. They are upstream art
# sources nobody edits here; anything else that differs is judged by its content.
ARTIFACT_GLOBS = ("Code/skyrim_ui/src/assets/images/*.ai",)

EXIT_OK, EXIT_FAILED, EXIT_LOCAL_WORK = 0, 1, 2


class GitError(RuntimeError):
    pass


def git(repo: Path, *args: str, env: dict | None = None, check: bool = True, stdin: str | None = None) -> str:
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    proc = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, env=full_env, input=stdin)
    if check and proc.returncode != 0:
        raise GitError(f"git {' '.join(args)} failed ({proc.returncode}): {proc.stderr.strip()}")
    return proc.stdout


def git_ok(repo: Path, *args: str) -> bool:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True).returncode == 0


@dataclass
class Report:
    branch: str = ""
    tip: str = ""
    head: str = ""
    relation: str = ""          # "equal", "behind", "ahead", "diverged"
    ahead_by: int = 0
    stale: list[str] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    artifact: list[str] = field(default_factory=list)
    local: list[str] = field(default_factory=list)
    untracked: list[str] = field(default_factory=list)

    @property
    def restorable(self) -> list[str]:
        return self.stale + self.missing + self.artifact

    @property
    def has_local_work(self) -> bool:
        return bool(self.local) or self.relation in ("ahead", "diverged")

    @property
    def in_line(self) -> bool:
        return self.relation == "equal" and not self.restorable and not self.local


def is_artifact(path: str) -> bool:
    return any(fnmatch.fnmatch(path, glob) for glob in ARTIFACT_GLOBS)


def historical_blobs(repo: Path, tip: str, path: str) -> set[str]:
    """Every content this path ever had in the history of the server tip."""
    commits = git(repo, "rev-list", tip, "--", path).split()
    if not commits:
        return set()
    query = "".join(f"{commit}:{path}\n" for commit in commits)
    blobs = set()
    for line in git(repo, "cat-file", "--batch-check", stdin=query).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "blob":   # "<sha> blob <size>"; a deleting commit answers "missing"
            blobs.add(parts[0])
    return blobs


def inspect(repo: Path, branch: str | None, remote: str, fetch: bool = True) -> Report:
    report = Report()
    report.branch = branch or git(repo, "rev-parse", "--abbrev-ref", "HEAD").strip()
    if report.branch in ("", "HEAD"):
        raise GitError("HEAD is detached: name the branch with --branch")

    if fetch:
        # An explicit refspec: a restored clone may not track this branch at all. History is needed to
        # tell "older version" from "new content", so a shallow clone is completed first.
        args = ["fetch", "--quiet"]
        if git(repo, "rev-parse", "--is-shallow-repository").strip() == "true":
            args.append("--unshallow")
        args += [remote, f"+refs/heads/{report.branch}:refs/remotes/{remote}/{report.branch}"]
        git(repo, *args)

    report.tip = git(repo, "rev-parse", "--verify", f"refs/remotes/{remote}/{report.branch}^{{commit}}").strip()
    report.head = git(repo, "rev-parse", "--verify", "HEAD^{commit}").strip()

    if report.head == report.tip:
        report.relation = "equal"
    elif git_ok(repo, "merge-base", "--is-ancestor", report.head, report.tip):
        report.relation = "behind"
    elif git_ok(repo, "merge-base", "--is-ancestor", report.tip, report.head):
        report.relation = "ahead"
    else:
        report.relation = "diverged"
    if report.relation in ("ahead", "diverged"):
        report.ahead_by = int(git(repo, "rev-list", "--count", f"{report.tip}..{report.head}").strip() or 0)

    # Compare the files with the server tip through a throwaway index, so that looking changes nothing.
    with tempfile.TemporaryDirectory() as tmp:
        env = {"GIT_INDEX_FILE": str(Path(tmp) / "index")}
        git(repo, "read-tree", report.tip, env=env)
        git(repo, "update-index", "-q", "--refresh", env=env, check=False)   # non-zero just means "something differs"
        differing = [p for p in git(repo, "diff-files", "--name-only", "-z", env=env).split("\0") if p]
        untracked = [p for p in git(repo, "ls-files", "--others", "--exclude-standard", "-z", env=env).split("\0") if p]

    root = Path(git(repo, "rev-parse", "--show-toplevel").strip())
    for path in differing:
        if not (root / path).exists():
            report.missing.append(path)
        elif is_artifact(path):
            report.artifact.append(path)
        else:
            mine = git(repo, "hash-object", "--", path).strip()
            (report.stale if mine in historical_blobs(repo, report.tip, path) else report.local).append(path)
    report.untracked = untracked
    return report


def describe(report: Report, out=sys.stdout) -> None:
    def show(title: str, paths: list[str], limit: int = 40) -> None:
        if not paths:
            return
        print(f"  {title}: {len(paths)}", file=out)
        for path in paths[:limit]:
            print(f"      {path}", file=out)
        if len(paths) > limit:
            print(f"      ... and {len(paths) - limit} more", file=out)

    print(f"branch {report.branch}: server tip {report.tip[:10]}, this clone at {report.head[:10]} ({report.relation})", file=out)
    show("stale (an older version of the file, already on the server)", report.stale)
    show("missing (on the server, not here)", report.missing)
    show("artifact (binary damaged by the snapshot restore)", report.artifact)
    show("LOCAL (content the server has never seen: unpushed work)", report.local)
    show("untracked (not on the server; a restore does not touch them)", report.untracked, limit=15)

    if report.relation in ("ahead", "diverged"):
        print(f"VERDICT: this clone has {report.ahead_by} commit(s) the server does not have ({report.relation}). "
              "Nothing to recover from the server: push them.", file=out)
    elif report.local:
        print(f"VERDICT: {len(report.local)} file(s) hold work that is not on the server. Do NOT restore over them: "
              "review them, then commit on top of the server tip.", file=out)
    elif report.in_line:
        print("VERDICT: the working copy is in line with the server.", file=out)
    else:
        print(f"VERDICT: nothing here is newer than the server; {len(report.restorable)} file(s) can be restored "
              "(run again with --apply).", file=out)


def apply(repo: Path, report: Report) -> None:
    """Only called when report.has_local_work is False."""
    if report.relation == "behind":
        git(repo, "reset", "--quiet", report.tip)   # mixed: moves the branch and the index, never the files
    paths = report.restorable
    for start in range(0, len(paths), 100):
        git(repo, "checkout", "--quiet", report.tip, "--", *paths[start : start + 100])


def run(repo: Path, branch: str | None, remote: str, do_apply: bool, out=sys.stdout) -> int:
    try:
        report = inspect(repo, branch, remote)
        describe(report, out)
        if report.has_local_work:
            if do_apply:
                print("refused: --apply would overwrite or abandon work that only exists here. Nothing was changed.", file=out)
            return EXIT_LOCAL_WORK
        if do_apply and not report.in_line:
            apply(repo, report)
            after = inspect(repo, branch, remote, fetch=False)
            if not after.in_line:
                describe(after, out)
                print("FAILED: the working copy still differs from the server after the restore.", file=out)
                return EXIT_FAILED
            print(f"restored {len(report.restorable)} file(s); the working copy is in line with {report.tip[:10]}.", file=out)
        return EXIT_OK
    except GitError as error:
        print(f"FAILED: {error}", file=out)
        return EXIT_FAILED


# ---------------------------------------------------------------------------------------------------
# Self-test: every verdict, on real repositories
# ---------------------------------------------------------------------------------------------------

IDENTITY = ["-c", "user.name=self-test", "-c", "user.email=self-test@example.invalid", "-c", "commit.gpgsign=false",
            "-c", "core.autocrlf=false", "-c", "init.defaultBranch=work", "-c", "protocol.file.allow=always"]
ART = "Code/skyrim_ui/src/assets/images/logo.ai"


def _g(repo: Path, *args: str) -> str:
    return git(repo, *IDENTITY, *args)


def _write(repo: Path, rel: str, text: str) -> None:
    path = repo / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def _commit(repo: Path, message: str) -> str:
    _g(repo, "add", "-A")
    _g(repo, "commit", "--quiet", "-m", message)
    return _g(repo, "rev-parse", "HEAD").strip()


def _make_server(tmp: Path) -> tuple[Path, list[str]]:
    """A server with three commits on branch 'work'. Returns the bare repo and the commit ids."""
    server = tmp / "server.git"
    subprocess.run(["git", "init", "--quiet", "--bare", str(server)], check=True, capture_output=True)
    dev = tmp / "dev"
    dev.mkdir()
    _g(dev, "init", "--quiet")
    _g(dev, "symbolic-ref", "HEAD", "refs/heads/work")
    _write(dev, "a.txt", "a v1\n")
    _write(dev, "b.txt", "b v1\n")
    _write(dev, ART, "art v1\n")
    c1 = _commit(dev, "c1")
    _write(dev, "a.txt", "a v2\n")
    _write(dev, "c.txt", "c v1\n")
    c2 = _commit(dev, "c2")
    _write(dev, "b.txt", "b v3\n")
    _write(dev, "sub/d.txt", "d v1\n")
    c3 = _commit(dev, "c3")
    _g(dev, "remote", "add", "origin", str(server))
    _g(dev, "push", "--quiet", "origin", "work")
    return server, [c1, c2, c3]


def _restored_snapshot(tmp: Path, name: str, server: Path, files_of: str, branch_at: str) -> Path:
    """What a sandbox reset leaves: the files of one commit, the branch and the index at another."""
    ws = tmp / name
    _g(tmp, "clone", "--quiet", "--branch", "work", str(server), str(ws))
    _g(ws, "reset", "--quiet", "--hard", files_of)
    _g(ws, "reset", "--quiet", branch_at)                        # mixed: the files stay
    _g(ws, "update-ref", "-d", "refs/remotes/origin/work")       # a fresh clone does not track the branch yet
    return ws


def self_test() -> tuple[list[str], int]:
    """Returns the failures and the number of checks that ran."""
    failures: list[str] = []
    checks = 0

    def expect(name: str, condition: bool, detail: str = "") -> None:
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(f"{name}{': ' + detail if detail else ''}")

    class Sink:
        def __init__(self) -> None:
            self.text = ""

        def write(self, s: str) -> None:
            self.text += s

    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        server, (c1, c2, c3) = _make_server(tmp)
        head = lambda ws: git(ws, "rev-parse", "HEAD").strip()   # noqa: E731

        # 1. A clone at the tip: nothing to do, --apply is a no-op.
        ws = tmp / "clean"
        _g(tmp, "clone", "--quiet", "--branch", "work", str(server), str(ws))
        report = inspect(ws, None, "origin")
        expect("clean: in line", report.in_line and report.relation == "equal", str(report))
        expect("clean: exit 0", run(ws, None, "origin", True, Sink()) == EXIT_OK)

        # 2. THE INCIDENT: the snapshot (files of c2) is older than the server (c3), the branch is at c1.
        ws = _restored_snapshot(tmp, "older", server, files_of=c2, branch_at=c1)
        report = inspect(ws, None, "origin")
        expect("older: behind", report.relation == "behind", report.relation)
        expect("older: b.txt is stale", report.stale == ["b.txt"], str(report.stale))
        expect("older: sub/d.txt is missing", report.missing == ["sub/d.txt"], str(report.missing))
        expect("older: nothing local", not report.local and not report.has_local_work, str(report.local))
        expect("older: report mode changed nothing", head(ws) == c1 and (ws / "b.txt").read_text() == "b v1\n")
        expect("older: report mode exit 0", run(ws, None, "origin", False, Sink()) == EXIT_OK)
        expect("older: apply exit 0", run(ws, None, "origin", True, Sink()) == EXIT_OK)
        expect("older: branch at the server tip", head(ws) == c3, head(ws))
        expect("older: b.txt restored", (ws / "b.txt").read_text() == "b v3\n")
        expect("older: sub/d.txt restored", (ws / "sub" / "d.txt").exists())
        expect("older: clean status", git(ws, "status", "--porcelain").strip() == "", git(ws, "status", "--porcelain"))

        # 3. The snapshot carries work the server never saw, next to stale files: refuse, change nothing.
        ws = _restored_snapshot(tmp, "local", server, files_of=c2, branch_at=c1)
        _write(ws, "a.txt", "a v2 plus an edit nobody pushed\n")
        _write(ws, "new/e.txt", "brand new\n")
        report = inspect(ws, None, "origin")
        expect("local: a.txt is LOCAL", report.local == ["a.txt"], str(report.local))
        expect("local: b.txt still classified stale", report.stale == ["b.txt"], str(report.stale))
        expect("local: new/e.txt is untracked", report.untracked == ["new/e.txt"], str(report.untracked))
        sink = Sink()
        expect("local: apply refuses with exit 2", run(ws, None, "origin", True, sink) == EXIT_LOCAL_WORK)
        expect("local: says so", "refused" in sink.text and "Do NOT restore" in sink.text, sink.text)
        expect("local: the edit survived", (ws / "a.txt").read_text() == "a v2 plus an edit nobody pushed\n")
        expect("local: the stale file was not touched either", (ws / "b.txt").read_text() == "b v1\n")
        expect("local: the branch did not move", head(ws) == c1, head(ws))
        expect("local: untracked file survived", (ws / "new" / "e.txt").exists())

        # 4. A commit that was never pushed: the clone is ahead, there is nothing to recover.
        ws = tmp / "ahead"
        _g(tmp, "clone", "--quiet", "--branch", "work", str(server), str(ws))
        _write(ws, "a.txt", "a v4 committed locally\n")
        c4 = _commit(ws, "c4, not pushed")
        report = inspect(ws, None, "origin")
        expect("ahead: relation", report.relation == "ahead" and report.ahead_by == 1, f"{report.relation} {report.ahead_by}")
        sink = Sink()
        expect("ahead: apply refuses with exit 2", run(ws, None, "origin", True, sink) == EXIT_LOCAL_WORK)
        expect("ahead: says push", "push them" in sink.text, sink.text)
        expect("ahead: the commit is still there", head(ws) == c4)

        # 5. A damaged binary on the known list is restored without being mistaken for work.
        ws = _restored_snapshot(tmp, "artifact", server, files_of=c3, branch_at=c1)
        _write(ws, ART, "damaged by the restore\n")
        report = inspect(ws, None, "origin")
        expect("artifact: classified", report.artifact == [ART] and not report.local, f"{report.artifact} {report.local}")
        expect("artifact: apply exit 0", run(ws, None, "origin", True, Sink()) == EXIT_OK)
        expect("artifact: restored", (ws / ART).read_text() == "art v1\n")

        # 6. The same damage anywhere else IS judged by content: it is unknown content, so it is LOCAL.
        ws = _restored_snapshot(tmp, "notartifact", server, files_of=c3, branch_at=c1)
        _write(ws, "c.txt", "damaged by the restore\n")
        expect("not on the artifact list: LOCAL", inspect(ws, None, "origin").local == ["c.txt"])

        # 7. A shallow clone cannot tell old from new until its history is complete: the tool completes it.
        ws = tmp / "shallow"
        _g(tmp, "clone", "--quiet", "--depth", "1", "--branch", "work", server.as_uri(), str(ws))
        expect("shallow: really shallow", git(ws, "rev-parse", "--is-shallow-repository").strip() == "true")
        _write(ws, "b.txt", "b v1\n")   # the content of c1, two commits below the shallow boundary
        report = inspect(ws, None, "origin")
        expect("shallow: completed", git(ws, "rev-parse", "--is-shallow-repository").strip() == "false")
        expect("shallow: old version recognised as stale", report.stale == ["b.txt"] and not report.local,
               f"stale={report.stale} local={report.local}")

        # 8. Histories that split: neither side is simply older.
        ws = _restored_snapshot(tmp, "diverged", server, files_of=c2, branch_at=c2)
        _write(ws, "a.txt", "a on a side branch\n")
        _commit(ws, "side")
        report = inspect(ws, None, "origin")
        expect("diverged: relation", report.relation == "diverged" and report.has_local_work, report.relation)
        expect("diverged: apply refuses", run(ws, None, "origin", True, Sink()) == EXIT_LOCAL_WORK)

    return failures, checks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--branch", help="branch to compare with (default: the current branch)")
    parser.add_argument("--remote", default="origin")
    parser.add_argument("--apply", action="store_true", help="restore the files; refused when local work is present")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        failures, checks = self_test()
        for failure in failures:
            print(f"SELF-TEST FAIL: {failure}")
        print(f"workspace recovery self-test: {checks} checks, {len(failures)} failed")
        return 1 if failures else 0

    return run(args.repo, args.branch, args.remote, args.apply)


if __name__ == "__main__":
    sys.exit(main())
