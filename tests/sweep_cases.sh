#!/usr/bin/env bash
# Differential stress sweep: runs every case under tests/cases twice with the
# same binary, once as the suite does and once with extra environment
# (default: full stress GC plus the JIT forced on at the first call), and
# reports any case whose exit status or stdout differs, that dies on a signal,
# or that times out only under the sweep. It finds crashes and hangs in code
# the normal suite exercises but never under GC pressure or the JIT; it does
# not need an expectation file, because the plain run is the oracle.
#
#   tests/sweep_cases.sh                      # default sweep, 6 jobs, 120 s per run
#   SWEEP_ENV='DIAMOND_STRESS_MINOR_GC=1' tests/sweep_cases.sh 8 60
#
# Uses build/diamond (a release build is much faster). Cases that listen on
# fixed ports can collide when run in parallel: re-run reported cases with
# JOBS=1 before treating them as findings.
set -uo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_dir"
diamond="$repo_dir/build/diamond"
# Cases assert through their final top-level value, which a script run
# prints only with DIAMOND_PRINT_RESULT set.
export DIAMOND_PRINT_RESULT=1
jobs="${1:-${JOBS:-6}}"
limit="${2:-${LIMIT:-120}}"
sweep_env="${SWEEP_ENV:-DIAMOND_STRESS_GC=1 DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
export diamond limit sweep_env out

run_case() {
    local case_file="$1" name base
    name="${case_file%.d6}"
    base="$(basename "$name")"
    if [[ ! -f "$name.expected" && ! -f "$name.expected_error" &&
          ! -f "$name.expected_contains" && ! -f "$name.expected_lastline" &&
          ! -f "$name.flags" && ! -f "$name.env" ]]; then
        grep -q 'suite.run!()' "$case_file" || return 0
    fi
    local env_args=() flag_args=() line
    if [[ -f "$name.env" ]]; then
        while IFS= read -r line; do [[ -n "$line" ]] && env_args+=("$line"); done < "$name.env"
    fi
    if [[ -f "$name.flags" ]]; then
        while IFS= read -r line; do [[ -n "$line" ]] && flag_args+=("$line"); done < "$name.flags"
    fi
    local plain_status sweep_status
    timeout "$limit" env -u DIAMOND_JIT -u DIAMOND_JIT_THRESHOLD -u DIAMOND_STRESS_GC \
        "${env_args[@]}" "$diamond" "${flag_args[@]}" "$case_file" \
        >"$out/$base.plain" 2>"$out/$base.plain.err" </dev/null
    plain_status=$?
    # shellcheck disable=SC2086
    timeout "$limit" env $sweep_env "${env_args[@]}" "$diamond" "${flag_args[@]}" "$case_file" \
        >"$out/$base.sweep" 2>"$out/$base.sweep.err" </dev/null
    sweep_status=$?
    if [[ $plain_status -eq 124 ]]; then
        echo "SLOW-PLAIN $base"
    elif [[ $sweep_status -eq 124 ]]; then
        echo "TIMEOUT $base"
    elif [[ $sweep_status -ge 128 ]]; then
        echo "SIGNAL($((sweep_status - 128))) $base"
    elif [[ $plain_status -ne $sweep_status ]]; then
        echo "STATUS($plain_status->$sweep_status) $base"
    elif ! cmp -s "$out/$base.plain" "$out/$base.sweep"; then
        echo "OUTPUT $base"
    fi
    return 0
}
export -f run_case

printf '%s\n' tests/cases/*.d6 | xargs -P "$jobs" -I{} bash -c 'run_case {}' | sort | tee "$out/report"
count="$(wc -l < "$out/report")"
echo "sweep: $count case(s) differ or fail under: $sweep_env"
[[ "$count" -eq 0 ]]
