#!/usr/bin/env bash
# Runs every check that does not need Windows, the game, or a full toolchain.
#
#   1. pure gameplay tests (g++ only)
#   2. settings/documentation cross-check
#   3. struct layout diff against the pre-AE reference (needs the git history)
#
# Exit code is non-zero if any check fails; the layout check is reported as
# skipped (not failed) when the reference commit is not in the clone.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
status=0

echo "== pure gameplay tests =="
if "${root}/tools/run_pure_tests.sh"; then :; else status=1; fi

echo
echo "== settings versus documentation =="
if python3 "${root}/tools/check_settings.py" --root "${root}"; then :; else status=1; fi

echo
echo "== struct layouts versus the pre-AE reference =="
python3 "${root}/tools/struct_layout_diff.py" --root "${root}"
case $? in
    0) : ;;
    2) echo "skipped (reference commit not fetched)" ;;
    *) status=1 ;;
esac

echo
if [ "${status}" -eq 0 ]; then
    echo "all checks passed"
else
    echo "SOME CHECKS FAILED"
fi

exit "${status}"
