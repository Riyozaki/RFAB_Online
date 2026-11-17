#!/usr/bin/env bash
#
# The server's crash record, the portable half (work plan 4.5).
#
#   ./Tools/rfab/linux-server/crash-record-self-test.sh
#
# Builds a ten-line program around Code/server_runner/CrashRecord.cpp and makes it die the way a
# server dies when an exception escapes: std::terminate. Checks that the process ended with the
# self-test's exit code, that logs/STServer_Crash.log holds a [TERMINATE] record with the
# exception's text, and that Tools/rfab/symbolize_crash.py reads that record.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WORK_DIR="${REPO_ROOT}/build/crash-record-self-test"
rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"

fail() {
    echo "!! crash record self-test: $*" >&2
    exit 1
}

"${CXX:-g++}" -std=c++20 -O1 -w \
    -I"${REPO_ROOT}/Code/server_runner" \
    "${REPO_ROOT}/Tools/rfab/linux-server/crash-record/probe.cpp" \
    "${REPO_ROOT}/Code/server_runner/CrashRecord.cpp" \
    -o "${WORK_DIR}/probe"

cd "${WORK_DIR}"

# A normal run writes nothing.
./probe || fail "the probe failed without being asked to (exit $?)"
[[ ! -e logs/STServer_Crash.log ]] || fail "a normal run wrote a crash record"

# An unknown mode is refused, not crashed.
set +e
./probe --rfab-crash-self-test nonsense 2>/dev/null
code=$?
set -e
[[ "${code}" == "2" ]] || fail "an unknown self-test mode exited with ${code}, expected 2"

# An exception that escapes a noexcept function.
set +e
./probe --rfab-crash-self-test terminate 2> stderr.txt
code=$?
set -e
[[ "${code}" == "71" ]] || fail "terminate mode exited with ${code}, expected 71 (the record was not written before the process ended?)"
[[ -s logs/STServer_Crash.log ]] || fail "no logs/STServer_Crash.log after std::terminate"
grep -q '\[TERMINATE\] the server stopped itself: std::terminate was called; what()=\["RFAB crash self-test: this text must reach the crash log"\]' logs/STServer_Crash.log \
    || fail "the [TERMINATE] record does not carry the exception's text: $(cat logs/STServer_Crash.log)"
grep -q '\[TERMINATE\]' stderr.txt || fail "the record was not shown on the console"

# A second death appends; nothing is overwritten.
set +e
./probe --rfab-crash-self-test terminate 2>/dev/null
set -e
[[ "$(grep -c '\[TERMINATE\]' logs/STServer_Crash.log)" == "2" ]] || fail "the crash log is not append-only"

# The report tool reads it.
python3 "${REPO_ROOT}/Tools/rfab/symbolize_crash.py" logs/STServer_Crash.log > symbolized.txt
grep -q 'std::terminate: 2 record(s)' symbolized.txt || fail "symbolize_crash.py did not report the records: $(cat symbolized.txt)"
grep -q 'what(): RFAB crash self-test: this text must reach the crash log' symbolized.txt || fail "symbolize_crash.py lost the exception's text"

echo "[RFAB_RESULT] server crash record (portable half): std::terminate leaves a [TERMINATE] record with the exception's text, append-only, readable by symbolize_crash.py"
