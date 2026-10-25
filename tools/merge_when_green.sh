#!/usr/bin/env bash
# usage: tools/merge_when_green.sh PR_NUMBER BRANCH
# Waits for every CI check on the PR, merges it only if all pass and main has not moved, then updates main and deletes the branch.
# Needs `gh`. Exit 1 (and no merge) unless every registered check succeeds and main has not moved.
# Start it a minute after pushing: just after a push only a few checks have registered, and "all of those passed" is not "green".
# MIN_CHECKS (default 31) is the number of checks CI registers for a pull request; lower it only if CI shrinks.
set -euo pipefail
pr="$1"; branch="$2"
min_checks="${MIN_CHECKS:-31}"
cd "$(dirname "$0")/.."

[[ "$min_checks" =~ ^[1-9][0-9]*$ ]] || { echo "MIN_CHECKS must be a positive integer" >&2; exit 1; }

# gh exits 8 for pending checks and 1 for failed checks. Both may still
# supply a valid report; API errors or missing/malformed output never do.
read_checks() {
    local report status=0 state
    report="$(gh pr checks "$pr" --json state --jq 'length, (.[] | .state)')" || status=$?
    if [[ "$status" != 0 && "$status" != 1 && "$status" != 8 ]]; then
        echo "NOT MERGED: could not read checks" >&2
        return 1
    fi
    local -a states
    mapfile -t states <<<"$report"
    total="${states[0]}"
    if [[ ! "$total" =~ ^[0-9]+$ ]] || (( ${#states[@]} != total + 1 )); then
        echo "NOT MERGED: invalid check report" >&2
        return 1
    fi
    checks_green=true
    for state in "${states[@]:1}"; do
        case "$state" in
            SUCCESS) ;;
            QUEUED|IN_PROGRESS|PENDING|REQUESTED|WAITING) checks_green=false ;;
            *) echo "NOT MERGED: check state is $state" >&2; return 1 ;;
        esac
    done
    # A nonzero status without a non-successful state is contradictory.
    if [[ "$checks_green" == true && "$status" != 0 ]]; then
        echo "NOT MERGED: inconsistent check report" >&2
        return 1
    fi
}

require_green() {
    if [[ "$checks_green" != true ]] || (( total < min_checks )); then
        echo "NOT MERGED: checks are incomplete or not all successful" >&2
        return 1
    fi
}

for _ in $(seq 1 120); do
    read_checks
    if [[ "$checks_green" == true ]] && (( total >= min_checks )); then break; fi
    sleep 30
done
require_green

git fetch -q origin
git merge-base --is-ancestor origin/main "origin/$branch" || { echo "NOT MERGED: main moved, rebase needed" >&2; exit 1; }
# Recheck the count as well as every state immediately before the merge.
read_checks
require_green
echo "checks: $total pass"
gh pr merge "$pr" --merge
git checkout -q main
git pull -q --ff-only origin main
git branch -D "$branch" >/dev/null 2>&1 || true
git push origin --delete "$branch" >/dev/null 2>&1 || true
echo "merged: $(git log --oneline -1)"
