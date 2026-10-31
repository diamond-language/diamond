#!/usr/bin/env bash
# Runs every tests/cases program with an .expected file through the self-hosted compiler and tallies the results.
#   tools/self_host_survey.sh [OUTPUT_FILE] [CASE_LIST_FILE]   (run from anywhere; needs a built build/diamond)
# OUTPUT_FILE gets one tab-separated line per case: pass, fail or skip, the case name, and the first error line.
# CASE_LIST_FILE limits the run to the names it lists, one per line.
# Options: SURVEY_JOBS (default 10), SURVEY_TIMEOUT seconds per case (default 30).
#
# The compiler and the program share a process, so a case's .env reaches the compiler too. A sandbox or resource
# limit would stop the compiler itself, so those cases are skipped. JIT and GC-stress settings test the runtime,
# not the language, and only slow the compiler down, so they are dropped. Other variables are kept.
set -uo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" == --case ]]; then
    name="$2"
    env_file="tests/cases/$name.env"
    if [[ -f "$env_file" ]] && LC_ALL=C command grep -Eq '^DIAMOND_(SANDBOX|MAX_|GRACE_)' "$env_file"; then
        printf 'skip\t%s\tneeds a limit the compiler would hit\n' "$name"
        exit 0
    fi
    env_args=()
    if [[ -f "$env_file" ]]; then
        while IFS= read -r line; do
            case "$line" in
                ''|DIAMOND_JIT*|DIAMOND_STRESS*|DIAMOND_TRACE_JIT*) ;;
                *) env_args+=("$line") ;;
            esac
        done < "$env_file"
    fi
    work="$(mktemp -d)"
    status=0
    printf '%s\n' "tests/cases/$name.d6" |
        env ${env_args[@]+"${env_args[@]}"} timeout "${SURVEY_TIMEOUT:-30}" \
        build/diamond selfhost/parser_run_with_core.d6 > "$work/out" 2> "$work/err" || status=$?
    # Compared without trailing newlines, as the test harness does.
    if [[ "$status" == 0 ]] && [[ "$(cat "$work/out")" == "$(cat "tests/cases/$name.expected")" ]]; then
        printf 'pass\t%s\t\n' "$name"
    else
        reason="$(head -1 "$work/err" | tr '\t' ' ' | cut -c1-240)"
        [[ -n "$reason" ]] || { [[ "$status" == 124 ]] && reason="timeout"; }
        [[ -n "$reason" ]] || reason="output differs"
        printf 'fail\t%s\t%s\n' "$name" "$reason"
    fi
    rm -rf "$work"
    exit 0
fi

output="${1:-/dev/stdout}"
if [[ -n "${2:-}" ]]; then
    cases="$(cat "$2")"
else
    cases="$(find tests/cases -maxdepth 1 -name '*.expected' -printf '%f\n' | sed 's/\.expected$//' | sort)"
fi
results="$(printf '%s\n' "$cases" | xargs -P "${SURVEY_JOBS:-10}" -I{} "$0" --case {})"
printf '%s\n' "$results" | sort -k2 > "$output"
printf '%s\n' "$results" | awk -F'\t' '
    { count[$1]++ }
    END { printf "%d of %d pass (%d skipped)\n", count["pass"], count["pass"] + count["fail"], count["skip"] }' >&2
