#!/usr/bin/env bash
# Runs one CI step and makes its outcome readable through the GitHub API.
#
#   ./Tools/rfab/ci_run.sh "<step name>" <command> [args...]
#
# The command's output is passed through unchanged and its exit code is returned. On failure the
# captured log goes through ci_diagnostics.py, which prints the first errors as annotations; on
# success one notice carries the result lines (how many tests ran). Without this a failed Linux job
# is visible from outside only as "Process completed with exit code 1".
#
# The workflow files call this script instead of the commands themselves on purpose: the GitHub App
# used for development cannot change .github/workflows, so everything that may need to change lives
# in scripts it can change.
set -uo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 \"<step name>\" <command> [args...]" >&2
    exit 2
fi

step="$1"
shift
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
log="$(mktemp "${TMPDIR:-/tmp}/rfab-ci-XXXXXX.log")"
trap 'rm -f "${log}"' EXIT

"$@" 2>&1 | tee "${log}"
code="${PIPESTATUS[0]}"

if [[ "${code}" -ne 0 ]]; then
    python3 "${here}/ci_diagnostics.py" --log "${log}" --step "${step}" --exit-code "${code}" || true
else
    python3 "${here}/ci_diagnostics.py" --log "${log}" --step "${step}" --ok || true
fi
exit "${code}"
