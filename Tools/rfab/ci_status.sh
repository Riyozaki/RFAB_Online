#!/usr/bin/env bash
# Shows what CI says about a commit, using only what api.github.com serves: runs, jobs, steps and
# annotations (logs and artifacts are not reachable from the development sandbox, see docs/CI.md).
#
#   ./Tools/rfab/ci_status.sh                 # runs for HEAD of the current branch
#   ./Tools/rfab/ci_status.sh <sha|branch>    # runs for another commit or branch tip
#   ./Tools/rfab/ci_status.sh --wait [ref]    # poll until every run for the commit has finished
set -uo pipefail

wait_mode=0
if [[ "${1:-}" == "--wait" ]]; then
    wait_mode=1
    shift
fi
ref="${1:-HEAD}"
sha="$(git rev-parse "${ref}" 2>/dev/null || true)"
[[ -n "${sha}" ]] || { echo "unknown ref: ${ref}" >&2; exit 2; }
repo="$(gh repo view --json nameWithOwner --jq .nameWithOwner)"

report() {
    local runs
    runs="$(gh api "repos/${repo}/actions/runs?head_sha=${sha}&per_page=20" \
        --jq '.workflow_runs[] | "\(.id)\t\(.status)\t\(.conclusion // "-")\t\(.name)"')"
    if [[ -z "${runs}" ]]; then
        echo "no workflow runs for ${sha:0:7}"
        return 1
    fi
    local pending=0
    while IFS=$'\t' read -r run_id status conclusion name; do
        echo "== ${name}: ${status} ${conclusion} (run ${run_id}, ${sha:0:7})"
        [[ "${status}" == "completed" ]] || pending=1
        gh api "repos/${repo}/actions/runs/${run_id}/jobs?per_page=50" \
            --jq '.jobs[] | "\(.id)\t\(.status)\t\(.conclusion // "-")\t\(.name)\t\([.steps[]? | select(.status=="in_progress") | .name] | join(", "))\t\([.steps[]? | select(.conclusion=="failure") | .name] | join(", "))"' |
        while IFS=$'\t' read -r job_id job_status job_conclusion job_name running failed; do
            line="   ${job_name}: ${job_status} ${job_conclusion}"
            [[ -n "${running}" ]] && line+=" | running: ${running}"
            [[ -n "${failed}" ]] && line+=" | failed step: ${failed}"
            echo "${line}"
            # Our own annotations carry a title; the runner's generic ones (Node deprecation,
            # "exit code 1") are skipped unless nothing else explains a failure.
            gh api "repos/${repo}/check-runs/${job_id}/annotations?per_page=100" \
                --jq '.[] | select((.title // "") != "") | "      [\(.annotation_level)] \(.title): \(.message)"' 2>/dev/null |
                sed 's/^/   /' || true
        done
    done <<< "${runs}"
    return "${pending}"
}

if [[ "${wait_mode}" -eq 1 ]]; then
    until report > /tmp/rfab-ci-status.txt 2>&1; do
        sleep 45
    done
    cat /tmp/rfab-ci-status.txt
else
    report
fi
