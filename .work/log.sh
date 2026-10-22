#!/usr/bin/env bash
# Appends a row to .work/time_log.md with the real elapsed/remaining time.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
start=$(grep '^start_epoch:' "$root/.work/session_start.txt" | awk '{print $2}')
deadline=$(grep '^deadline_epoch:' "$root/.work/session_start.txt" | awk '{print $2}')
now=$(date -u +%s)
elapsed=$((now - start)); left=$((deadline - now))
fmt() { printf '%d:%02d' $(( $1 / 3600 )) $(( ($1 % 3600) / 60 )); }
entry="${1:-no description}"
printf '| %s | %s | %s | %s | %s |\n' "$(date -u +%H:%M:%S)" "$(fmt $elapsed)" "$(fmt $left)" "$entry" >> "$root/.work/time_log.md"
printf 'now=%s elapsed=%s left=%s\n' "$(date -u +%H:%M:%S)" "$(fmt $elapsed)" "$(fmt $left)"
