#!/usr/bin/env bash
set -euo pipefail

# Re-runs every test case that uses Thread, Channel or Supervisor many times,
# each run pinned to two CPUs and under a timeout. Threaded bugs are timing
# bugs: the supervisor child-count race (CHANGELOG 0.10.3) never showed in a
# normal run, hung CI about once in 40 pinned runs of one test, and was found
# only by pinning that test to 2 CPUs and looping it. A pinned, oversubscribed
# machine interleaves threads far more unevenly than an idle multi-core one.
# This makes that trick a standing check. A run that fails, differs from the
# expectation, or does not finish within the timeout is a failure.
#
# STRESS_RUNS (default 60) is how many times each case runs, STRESS_CPUS
# (default 0,1) the CPU list, STRESS_TIMEOUT (default 60) the per-run limit in
# seconds, STRESS_FILTER an optional regex that narrows the cases by file name.
# Needs Linux `taskset`; elsewhere it says so and passes, since the
# other CI platforms cannot pin CPUs.

runs="${STRESS_RUNS:-60}"
cpus="${STRESS_CPUS:-0,1}"
limit="${STRESS_TIMEOUT:-60}"
filter="${STRESS_FILTER:-}"

if ! command -v taskset >/dev/null 2>&1; then
    echo "stress_threads: taskset not available; skipping (Linux only)"
    exit 0
fi

diamond="$(realpath ./build/diamond)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The environment variables tests/run.sh clears before every case, so a stray
# one in the caller's shell cannot change a case's behavior.
clear_vars=(DIAMOND_STRESS_GC DIAMOND_STRESS_MINOR_GC DIAMOND_QUICKEN
    DIAMOND_QUICKEN_THRESHOLD DIAMOND_IC_MONO_THRESHOLD DIAMOND_REPEAT
    DIAMOND_INVALIDATE_IC_EACH_RUN DIAMOND_TRACE_IC_EACH_RUN DIAMOND_TRACE_IC
    DIAMOND_FORCE_REPL DIAMOND_SANDBOX DIAMOND_SANDBOX_ALLOW DIAMOND_NO_CACHE
    DIAMOND_MAX_INSTRUCTIONS DIAMOND_MAX_WALL_MILLISECONDS DIAMOND_MAX_MEMORY_BYTES
    DIAMOND_JIT DIAMOND_JIT_THRESHOLD DIAMOND_TRACE_JIT)

# Runs one case once; prints nothing and returns 0 when it matches its
# expectation, otherwise returns 1 with the reason on stdout. Mirrors the
# expectation kinds tests/run.sh checks.
run_once() { # case_file
    local file="$1" base="${1%.di}"
    local env_args=() flag_args=() line
    if [[ -f "$base.env" ]]; then
        while IFS= read -r line; do [[ -n "$line" ]] && env_args+=("$line"); done < "$base.env"
    fi
    if [[ -f "$base.flags" ]]; then
        while IFS= read -r line; do [[ -n "$line" ]] && flag_args+=("$line"); done < "$base.flags"
    fi
    local out="$work/out" err="$work/err" status=0
    env "${clear_vars[@]/#/-u}" "${env_args[@]}" \
        taskset -c "$cpus" timeout "$limit" "$diamond" "${flag_args[@]}" "$file" \
        >"$out" 2>"$err" || status=$?
    if (( status == 124 )); then echo "timed out after ${limit}s"; return 1; fi
    local actual expected pattern combined
    combined="$(cat "$out" "$err")"
    if [[ -f "$base.expected" ]]; then
        actual="$(<"$out")"; expected="$(cat "$base.expected")"
        (( status == 0 )) || { echo "exit status $status"; return 1; }
        [[ "$actual" == "$expected" ]] || {
            # Say what came back: a CI log that only reads "stdout differs" can't
            # tell a lost message from a reordering from a truncated run.
            echo "stdout differs: expected [${expected:0:300}] got [${actual:0:300}] stderr [$(head -c 300 "$err")]"
            return 1
        }
    elif [[ -f "$base.expected_error" ]]; then
        pattern="$(cat "$base.expected_error")"
        [[ "$combined" == *"$pattern"* ]] || { echo "error text differs"; return 1; }
    elif [[ -f "$base.expected_contains" || -f "$base.expected_lastline" ]]; then
        if [[ -f "$base.expected_contains" ]]; then
            while IFS= read -r pattern; do
                [[ -z "$pattern" ]] && continue
                grep -q -- "$pattern" <<<"$combined" || { echo "missing: $pattern"; return 1; }
            done < "$base.expected_contains"
        fi
        if [[ -f "$base.expected_lastline" ]]; then
            [[ "${combined##*$'\n'}" == "$(cat "$base.expected_lastline")" ]] ||
                { echo "last line differs"; return 1; }
        fi
    elif grep -q 'suite\.run!()' "$file"; then
        (( status == 0 )) || { echo "exit status $status"; return 1; }
    fi
    return 0
}

cases=()
for file in tests/cases/*.di; do
    [[ -z "$filter" ]] || [[ "$file" =~ $filter ]] || continue
    grep -qE 'Thread\.new|Supervisor|Channel' "$file" || continue
    base="${file%.di}"
    [[ -f "$base.expected" || -f "$base.expected_error" || -f "$base.expected_contains" ||
       -f "$base.expected_lastline" ]] || grep -q 'suite\.run!()' "$file" || continue
    cases+=("$file")
done
echo "stress_threads: ${#cases[@]} threaded cases x $runs runs on CPUs $cpus (limit ${limit}s)"

# A case that fails its first, unpinned run is not a timing problem (it may
# need files this script does not provide); leave it to the normal suite.
failures=0
for file in "${cases[@]}"; do
    saved_cpus="$cpus"; cpus="$(seq -s, 0 $(( $(nproc) - 1 )))"
    if ! reason="$(run_once "$file")"; then
        echo "stress_threads: leaving out $file (fails unpinned: $reason)"
        cpus="$saved_cpus"
        continue
    fi
    cpus="$saved_cpus"
    for run in $(seq 1 "$runs"); do
        if ! reason="$(run_once "$file")"; then
            echo "stress_threads: FAIL $file on run $run: $reason" >&2
            failures=$((failures + 1))
            break
        fi
    done
done

if (( failures > 0 )); then
    echo "stress_threads: $failures case(s) failed under pinned stress" >&2
    exit 1
fi
echo "stress_threads: all cases clean over $runs pinned runs each"
