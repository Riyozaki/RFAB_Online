#!/usr/bin/env bash
# Every check that reads the sources instead of running them, behind one entry point.
#
#   ./Tools/rfab/run-static-checks.sh
#
# A new check is added to the list below, not to .github/workflows: the GitHub App used for
# development cannot change workflow files. All checks run even when one fails, so a single CI run
# reports everything that is wrong.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

failed=()

run() {
    local name="$1"
    shift
    echo "==> ${name}"
    if ! "$@"; then
        echo "!! ${name} failed: $*"
        failed+=("${name}")
    fi
}

# The tools first prove that each of their rules still catches the mistake it exists for.
run "ci diagnostics self-test"    python3 Tools/rfab/ci_diagnostics.py --self-test
run "hook discipline self-test"   python3 Tools/rfab/check_hook_discipline.py --self-test
run "actor lifetime self-test"    python3 Tools/rfab/check_actor_lifetime.py --self-test
run "filesystem calls self-test"  python3 Tools/rfab/check_filesystem_calls.py --self-test
run "wire contract self-test"     python3 Tools/rfab/check_wire_contract.py --self-test
run "installed storage profile tests" python3 Tools/rfab/test_storage_profile.py
run "native Papyrus declarations" python3 Tools/rfab/test_native_psc.py
run "crash symbolizer self-test"  python3 Tools/rfab/symbolize_crash.py --self-test
# Not a check of the sources: the tool that brings a reset working copy back in line with the server
# must keep telling "an older version" from "work that was never pushed" (it runs on throwaway repos).
run "workspace recovery self-test" python3 Tools/rfab/recover_workspace.py --self-test

# Production checks are re-enabled: the clean core now includes RFAB-specific native hooks,
# atomically published snapshots, id-only events and a world-generation-scoped Runner.
run "hook discipline"             python3 Tools/rfab/check_hook_discipline.py
run "actor lifetime"              python3 Tools/rfab/check_actor_lifetime.py
# No filesystem call that reports an error by exception: path::string(), range-for over a
# directory_iterator, exists()/is_directory()/current_path() without an error_code.
run "filesystem calls"            python3 Tools/rfab/check_filesystem_calls.py
# Every message and event kind has a sender and a receiver in shipped code.
run "wire contract"               python3 Tools/rfab/check_wire_contract.py
# VERSION agrees everywhere, submodules are pinned, no build output in git, manifests parse.
run "release invariants"          python3 Tools/rfab/check_release_invariants.py
# Address Library ids and struct layouts match Skyrim SE 1.5.97.
run "SE 1.5.97 port audit"        python3 Tools/rfab/verify_se1597_port.py

# PowerShell scripts (build, packaging, installer, report collector) are parsed by the real
# PowerShell parser, and the ones that carry a self-test run it. GitHub's Linux runners ship
# PowerShell 7; where there is none the check is skipped out loud. The Windows build runs the same
# scripts under Windows PowerShell 5.1, half an hour later.
if command -v pwsh >/dev/null 2>&1; then
    run "powershell syntax"           pwsh -NoProfile -NonInteractive -File Tools/rfab/Test-PowerShellSyntax.ps1
    run "report collector self-test"  pwsh -NoProfile -NonInteractive -File Tools/rfab/Collect-RfabReport.ps1 -SelfTest
    run "runtime install self-test"   pwsh -NoProfile -NonInteractive -File Tools/rfab/Preserve-RuntimeData.ps1 -SelfTest
else
    echo "==> powershell checks skipped: pwsh is not installed on this machine"
fi

if [[ ${#failed[@]} -gt 0 ]]; then
    echo "!! ${#failed[@]} static check(s) failed: ${failed[*]}"
    exit 1
fi
echo "==> all static checks passed"
