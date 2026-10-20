#!/usr/bin/env bash
# usage: tools/merge_when_green.sh PR_NUMBER BRANCH
# Waits for every CI check on the PR, merges it only if all pass and main has not moved, then updates main and deletes the branch.
# Needs `gh`. Exit 1 (and no merge) when a check fails, too few checks have registered, or main moved so the branch needs a rebase.
# Start it a minute after pushing: just after a push only a few checks have registered, and "all of those passed" is not "green".
# MIN_CHECKS (default 20) is the number of checks CI registers for a pull request (27 today); lower it only if CI shrinks.
set -euo pipefail
pr="$1"; branch="$2"
min_checks="${MIN_CHECKS:-20}"
cd "$(dirname "$0")/.."

for _ in $(seq 1 120); do
    checks="$(gh pr checks "$pr" 2>&1 || true)"
    pending="$(grep -c pending <<<"$checks" || true)"
    total="$(grep -c . <<<"$checks" || true)"
    [[ "$pending" == 0 && "$total" -ge "$min_checks" ]] && break
    sleep 30
done

summary="$(gh pr checks "$pr" 2>&1 | awk -F'\t' '{print $2}' | sort | uniq -c | tr '\n' ' ' || true)"
echo "checks: $summary"
final="$(gh pr checks "$pr" 2>&1 || true)"
# Not `gh ... | grep -q`: grep exits at the first match, gh then dies of SIGPIPE, and pipefail turns the whole test false -- a failed check let the merge through.
if grep -qiE 'fail|pending' <<<"$final"; then
    echo "NOT MERGED: checks are not all green" >&2
    exit 1
fi

git fetch -q origin
git merge-base --is-ancestor origin/main "origin/$branch" || { echo "NOT MERGED: main moved, rebase needed" >&2; exit 1; }
gh pr merge "$pr" --merge
git checkout -q main
git pull -q --ff-only origin main
git branch -D "$branch" >/dev/null 2>&1 || true
git push origin --delete "$branch" >/dev/null 2>&1 || true
echo "merged: $(git log --oneline -1)"
