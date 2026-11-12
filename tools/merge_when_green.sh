#!/usr/bin/env bash
# usage: tools/merge_when_green.sh PR_NUMBER BRANCH [BASE_BRANCH]
# Waits for every CI check, merges only if all pass and the base has not moved,
# then updates the base and deletes the feature branch. The default base is main.
# Start it a minute after pushing: just after a push only a few checks have registered, and "all of those passed" is not "green".
# MIN_CHECKS (default 34) is the number of checks CI registers for a pull request; lower it only if CI shrinks.
set -euo pipefail
pr="$1"; branch="$2"; base="${3:-main}"
min_checks="${MIN_CHECKS:-34}"
cd "$(dirname "$0")/.."

[[ "$min_checks" =~ ^[1-9][0-9]*$ ]] || { echo "MIN_CHECKS must be a positive integer" >&2; exit 1; }
git check-ref-format --branch "$base" >/dev/null
[[ "$base" != "$branch" ]] || { echo "NOT MERGED: feature branch is the base" >&2; exit 1; }
require_base() {
    local actual_base
    actual_base=$(gh pr view "$pr" --json baseRefName --jq .baseRefName) || return 1
    [[ "$actual_base" == "$base" ]] || { echo "NOT MERGED: PR base is not $base" >&2; return 1; }
}
require_base

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
            # A check can report NEUTRAL briefly while it registers; keep waiting, and never merge on it.
            QUEUED|IN_PROGRESS|PENDING|REQUESTED|WAITING|NEUTRAL) checks_green=false ;;
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
git merge-base --is-ancestor "origin/$base" "origin/$branch" || { echo "NOT MERGED: $base moved, rebase needed" >&2; exit 1; }
# Recheck the count as well as every state immediately before the merge.
read_checks
require_green
require_base
echo "checks: $total pass"
gh pr merge "$pr" --merge
git checkout -q "$base"
git pull -q --ff-only origin "$base"
git branch -D "$branch" >/dev/null 2>&1 || true
git push origin --delete "$branch" >/dev/null 2>&1 || true
echo "merged: $(git log --oneline -1)"
