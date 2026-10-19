#!/usr/bin/env bash
# tools/test_sanitize.sh against fake test runners: a pass runs once, a plain failure is retried once and announced, a sanitizer
# finding is never retried, and two failures in a row still fail.
set -uo pipefail
cd "$(dirname "$0")/.."
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# A runner that fails according to $1 (a string of p/f/s, one letter per call: pass, plain fail, fail with a sanitizer report).
cat > "$work/runner.sh" <<'RUNNER'
#!/usr/bin/env bash
plan="$1"; counter="$2"
n=$(cat "$counter" 2>/dev/null || echo 0)
echo $((n + 1)) > "$counter"
case "${plan:$n:1}" in
    p) echo "tests ok"; exit 0 ;;
    f) echo "DIAGNOSTIC: failed at line 12: some harness step"; exit 1 ;;
    s) echo "==1==ERROR: AddressSanitizer: heap-use-after-free"; echo "SUMMARY: AddressSanitizer: heap-use-after-free in f"; exit 1 ;;
esac
RUNNER

run() { # plan -> prints "<exit>|<calls>|<combined output>"
    local plan="$1" counter="$work/count" output status
    rm -f "$counter"
    output="$(RUN_TESTS="bash $work/runner.sh $plan $counter" bash tools/test_sanitize.sh 2>&1)"
    status=$?
    printf '%s|%s|%s' "$status" "$(cat "$counter" 2>/dev/null || echo 0)" "$output"
}

check() { # label plan expected-exit expected-calls [needle...]
    local label="$1" plan="$2" want_status="$3" want_calls="$4"; shift 4
    local result status calls
    result="$(run "$plan")"
    status="${result%%|*}"; result="${result#*|}"
    calls="${result%%|*}"; result="${result#*|}"
    [[ "$status" == "$want_status" && "$calls" == "$want_calls" ]] || {
        echo "FAIL: $label: exit $status after $calls run(s), wanted exit $want_status after $want_calls" >&2
        echo "$result" >&2
        exit 1
    }
    local needle
    for needle in "$@"; do
        [[ "$result" == *"$needle"* ]] || { echo "FAIL: $label: output lacks '$needle'" >&2; echo "$result" >&2; exit 1; }
    done
}

check "passes first time" p 0 1
check "flake then pass is retried and announced" fp 0 2 \
    "test-sanitize: attempt 1 failed; retrying once (DIAGNOSTIC: failed at line 12: some harness step)" \
    "::warning title=test-sanitize retried::"
check "a sanitizer finding is not retried" sp 1 1 "attempt 1 reported a sanitizer finding; not retrying"
check "two plain failures still fail" ff 1 2 "retrying once"
if [[ "$(run sp)" == *"retrying once"* ]]; then echo "FAIL: a sanitizer finding was announced as a retry" >&2; exit 1; fi

echo "test_sanitize tests passed"
