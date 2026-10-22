#!/usr/bin/env bash
# Exercise the real merge script with fake GitHub/git commands; never touch a PR.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/repo/tools" "$work/bin"
cp tools/merge_when_green.sh "$work/repo/tools/"

cat > "$work/bin/fake" <<'FAKE'
#!/usr/bin/env bash
set -euo pipefail
case "${0##*/}" in
    sleep) exit 0 ;;
    git)
        echo "git $*" >> "$FIXTURE/calls"
        if [[ "$1" == merge-base && "$SCENARIO" == moved ]]; then exit 1; fi
        if [[ "$1" == log ]]; then echo "abc fixture"; fi
        exit 0 ;;
    gh)
        echo "gh $*" >> "$FIXTURE/calls"
        if [[ "$2" == merge ]]; then exit 0; fi
        [[ "$*" == *"--json state --jq"* ]] || exit 2
        n=$(cat "$FIXTURE/count" 2>/dev/null || echo 0)
        n=$((n + 1))
        echo "$n" > "$FIXTURE/count"
        case "$SCENARIO" in
            error) echo "network unavailable" >&2; exit 1 ;;
            malformed) printf '3\nSUCCESS\nSUCCESS\n'; exit 0 ;;
            inconsistent) printf '2\nSUCCESS\nSUCCESS\n'; exit 1 ;;
            too_few) printf '1\nSUCCESS\n'; exit 0 ;;
            disappearing) if (( n > 1 )); then printf '0\n'; exit 0; fi ;;
            shrinking) if (( n > 1 )); then printf '1\nSUCCESS\n'; exit 0; fi ;;
            final_cancelled) if (( n > 1 )); then printf '2\nSUCCESS\nCANCELLED\n'; exit 1; fi ;;
            final_pending) if (( n > 1 )); then printf '2\nSUCCESS\nIN_PROGRESS\n'; exit 8; fi ;;
            pending) if (( n == 1 )); then printf '2\nSUCCESS\nQUEUED\n'; exit 8; fi ;;
            blocked) printf '2\nSUCCESS\n%s\n' "$CHECK_STATE"; exit 1 ;;
        esac
        printf '2\nSUCCESS\nSUCCESS\n'
        ;;
esac
FAKE
chmod +x "$work/bin/fake"
for command in gh git sleep; do ln -s fake "$work/bin/$command"; done

run_case() {
    local scenario="$1" expected="$2" state="${3:-}" output status=0
    local fixture="$work/$scenario-${state:-default}"
    mkdir -p "$fixture"
    output=$(PATH="$work/bin:$PATH" FIXTURE="$fixture" SCENARIO="$scenario" CHECK_STATE="$state" MIN_CHECKS=2 \
        bash "$work/repo/tools/merge_when_green.sh" 123 fixture-branch 2>&1) || status=$?
    if [[ "$status" != "$expected" ]]; then
        echo "FAIL: $scenario $state: exit $status, expected $expected: $output" >&2
        exit 1
    fi
    if [[ "$expected" == 0 ]]; then
        grep -q '^gh pr merge 123 --merge$' "$fixture/calls"
        grep -q '^git push origin --delete fixture-branch$' "$fixture/calls"
        [[ $(cat "$fixture/count") -ge 2 ]]
    elif grep -q '^gh pr merge\|^git checkout\|^git branch -D\|^git push' "$fixture/calls"; then
        echo "FAIL: $scenario $state: unsafe merge or cleanup: $output" >&2
        exit 1
    fi
}

run_case success 0
run_case pending 0
for state in CANCELLED FAILURE TIMED_OUT ACTION_REQUIRED STARTUP_FAILURE SKIPPED NEUTRAL STALE UNKNOWN; do
    run_case blocked 1 "$state"
done
for scenario in error malformed inconsistent too_few disappearing shrinking final_cancelled final_pending moved; do
    run_case "$scenario" 1
done
echo "merge_when_green tests passed"
