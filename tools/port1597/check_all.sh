#!/usr/bin/env bash
# One-shot offline verification for the 1.5.97 port.
#
# Everything here can run without Windows, Skyrim or SKSE. The checks that need
# external data (the 1.5.97 Address Library database, the pre-AE tree of the
# fork, a CommonLibSSE-NG checkout) are skipped with a notice when the matching
# variable is not set.
#
#   TP_ADDRESS_LIBRARY   version-1-5-97-0.bin (or al-1-5-97.bin)
#   TP_LEGACY_TREE       Code/client of the fork before the AE switch
#   TP_FORK_REPO         full clone of TiltedEvolution (for history_pairs.py)
#   TP_CLIBNG_TREE       CommonLibSSE-NG checkout (for extract_clibng_pairs.py)
#
# Usage: bash tools/port1597/check_all.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

DB="${TP_ADDRESS_LIBRARY:-}"
LEGACY="${TP_LEGACY_TREE:-}"
FORK="${TP_FORK_REPO:-}"
CLIBNG="${TP_CLIBNG_TREE:-}"

skipped=0
failed=0

section() { printf '\n=== %s\n' "$1"; }
skip() { printf '  skipped: %s\n' "$1"; skipped=$((skipped + 1)); }
fail() { printf '  FAILED: %s\n' "$1"; failed=$((failed + 1)); }

section "generated files are up to date"
if python3 tools/port1597/gen_inl.py --check >/dev/null; then
    printf '  ok\n'
else
    fail "gen_inl.py --check (run: python3 tools/port1597/gen_inl.py)"
fi

section "structure profiles compile"
cat > /tmp/tp_profile_check.cpp <<'EOF'
#include <Games/GameProfile.h>
static_assert(TP_TARGET_GAME_VERSION_MAJOR == EXPECTED_MAJOR, "major");
static_assert(TP_TARGET_GAME_VERSION_MINOR == EXPECTED_MINOR, "minor");
static_assert(TP_TARGET_GAME_VERSION_REVISION == EXPECTED_REVISION, "revision");
int main() { return 0; }
EOF

for spec in "TP_GAME_PROFILE_1597:1:5:97" "TP_GAME_PROFILE_1600:1:6:629"; do
    IFS=: read -r define major minor revision <<<"$spec"
    if g++ -std=c++20 -I Code/client -D"$define" -DEXPECTED_MAJOR="$major" -DEXPECTED_MINOR="$minor" \
        -DEXPECTED_REVISION="$revision" -c /tmp/tp_profile_check.cpp -o /tmp/tp_profile_check.o 2>/dev/null; then
        printf '  %s ok\n' "$define"
    else
        fail "profile $define does not compile"
    fi
done

if g++ -std=c++20 -I Code/client -DTP_GAME_PROFILE_1597 -DTP_GAME_PROFILE_1600 \
    -DEXPECTED_MAJOR=1 -DEXPECTED_MINOR=5 -DEXPECTED_REVISION=97 \
    -c /tmp/tp_profile_check.cpp -o /tmp/tp_profile_check.o 2>/dev/null; then
    fail "two profiles at once were accepted"
else
    printf '  both profiles at once are rejected\n'
fi

section "address library self test"
g++ -std=c++20 -O1 -Wall -Wextra -I Code/common tools/port1597/selftest.cpp \
    Code/common/address_library/*.cpp -o /tmp/tp_selftest
if [ -n "$DB" ]; then
    if /tmp/tp_selftest --database "$DB"; then
        printf '  ok\n'
    else
        fail "selftest with database"
    fi
else
    if /tmp/tp_selftest; then
        printf '  ok (no database: coverage checks skipped)\n'
    else
        fail "selftest"
    fi
    skip "database-backed coverage (set TP_ADDRESS_LIBRARY)"
fi

section "generated lists match the sources"
python3 tools/port1597/id_worklist.py --out /tmp/tp_worklist.csv >/dev/null
if diff -q /tmp/tp_worklist.csv docs/phase1/id_worklist.csv >/dev/null; then
    printf '  id_worklist.csv ok\n'
else
    fail "id_worklist.csv is stale (run: python3 tools/port1597/id_worklist.py)"
fi

if [ -n "$DB" ]; then
    python3 tools/port1597/id_candidates.py --database "$DB" --out /tmp/tp_candidates.csv >/dev/null
    if diff -q /tmp/tp_candidates.csv docs/phase1/id_candidates.csv >/dev/null; then
        printf '  id_candidates.csv ok\n'
    else
        fail "id_candidates.csv is stale (run: python3 tools/port1597/id_candidates.py --database ...)"
    fi
else
    skip "id_candidates.csv (set TP_ADDRESS_LIBRARY)"
fi

section "pairs against the 2021 tree"
if [ -n "$DB" ] && [ -n "$LEGACY" ]; then
    if python3 tools/port1597/verify_pairs.py --database "$DB" --legacy-tree "$LEGACY" | tail -1 | grep -q "mismatched: 0"; then
        printf '  ok\n'
    else
        fail "verify_pairs.py found mismatches"
    fi
else
    skip "verify_pairs.py (set TP_ADDRESS_LIBRARY and TP_LEGACY_TREE)"
fi

section "pairs recovered from history"
if [ -n "$DB" ] && [ -n "$FORK" ]; then
    if python3 tools/port1597/history_pairs.py --repo "$FORK" --database "$DB" --check | tail -1 | grep -q "agree"; then
        printf '  ok\n'
    else
        fail "history_pairs.py --check disagrees with docs/phase1/id_pairs_curated.csv"
    fi
else
    skip "history_pairs.py (set TP_ADDRESS_LIBRARY and TP_FORK_REPO)"
fi

section "CommonLibSSE-NG pair harvest"
if [ -n "$CLIBNG" ]; then
    if python3 tools/port1597/extract_clibng_pairs.py "$CLIBNG" --check >/dev/null; then
        printf '  ok\n'
    else
        fail "clibng_id_pairs.csv is stale"
    fi
else
    skip "extract_clibng_pairs.py (set TP_CLIBNG_TREE)"
fi

printf '\n=== summary: %d failed, %d skipped\n' "$failed" "$skipped"
[ "$failed" -eq 0 ]
