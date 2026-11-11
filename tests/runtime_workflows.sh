#!/usr/bin/env bash
# Cross-feature runtime contracts, with fresh/cached bytecode and GC/JIT modes.
set -euo pipefail
cd "$(dirname "$0")/.."
diamond=$(realpath "${DIAMOND_BIN:-build/diamond}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cases=(runtime_workflow_json runtime_workflow_collections runtime_workflow_numbers
       runtime_workflow_control runtime_workflow_regex runtime_workflow_dispatch
       sqlite3_workflow_recovery process_workflow_streams channel_workflow_payloads
       file_workflow_atomic)
count=0
for mode in interpreter jit major-gc minor-gc; do
    jit=1
    major=0
    minor=0
    case "$mode" in
        interpreter) jit=0 ;;
        major-gc) major=1 ;;
        minor-gc) minor=1 ;;
    esac
    for name in "${cases[@]}"; do
        if ! timeout 30 env -u DIAMOND_TRACE_JIT -u DIAMOND_TRACE_CACHE \
            DIAMOND_PRINT_RESULT=1 DIAMOND_NO_CACHE=1 DIAMOND_JIT=$jit \
            DIAMOND_JIT_THRESHOLD=1 DIAMOND_STRESS_GC=$major DIAMOND_STRESS_MINOR_GC=$minor \
            "$diamond" "tests/cases/$name.d6" > "$work/actual" 2> "$work/error"; then
            echo "FAIL $mode $name" >&2
            cat "$work/error" >&2
            exit 1
        fi
        diff -u "tests/cases/$name.expected" "$work/actual"
        count=$((count + 1))
    done
    echo "runtime workflows: $mode passed"
done

# Cold then hot runs use private copies so stale caches cannot hide a failure.
for name in "${cases[@]}"; do
    cp "tests/cases/$name.d6" "$work/$name.d6"
    for state in miss hit; do
        if ! timeout 30 env -u DIAMOND_TRACE_JIT -u DIAMOND_STRESS_GC -u DIAMOND_STRESS_MINOR_GC \
            DIAMOND_NO_CACHE=0 DIAMOND_PRINT_RESULT=1 DIAMOND_JIT=1 \
            DIAMOND_JIT_THRESHOLD=1 DIAMOND_TRACE_CACHE=1 \
            "$diamond" "$work/$name.d6" > "$work/actual" 2> "$work/trace"; then
            echo "FAIL cache $state $name" >&2
            cat "$work/trace" >&2
            exit 1
        fi
        diff -u "tests/cases/$name.expected" "$work/actual"
        if ! grep -Fq "cache: $state " "$work/trace"; then
            echo "FAIL missing cache $state: $name" >&2
            cat "$work/trace" >&2
            exit 1
        fi
        count=$((count + 1))
    done
done
echo "runtime workflows: cold/hot bytecode passed"

# Compiled entry points must preserve the same semantics and native services.
# AOT is part of the integration target, rather than every ordinary unit run.
if [[ ${1:-} == --aot ]]; then
    for name in runtime_workflow_control runtime_workflow_json sqlite3_workflow_recovery; do
        if ! "$diamond" build "tests/cases/$name.d6" -o "$work/app" > "$work/build.log" 2>&1; then
            echo "FAIL standalone build $name" >&2
            cat "$work/build.log" >&2
            exit 1
        fi
        for jit in 0 1; do
            if ! timeout 30 env -u DIAMOND_TRACE_JIT -u DIAMOND_STRESS_GC -u DIAMOND_STRESS_MINOR_GC \
                DIAMOND_PRINT_RESULT=1 DIAMOND_JIT=$jit DIAMOND_JIT_THRESHOLD=1 \
                "$work/app" > "$work/actual" 2> "$work/error"; then
                echo "FAIL standalone JIT=$jit $name" >&2
                cat "$work/error" >&2
                exit 1
            fi
            diff -u "tests/cases/$name.expected" "$work/actual"
            count=$((count + 1))
        done
    done
    echo "runtime workflows: standalone binaries passed"
fi
echo "$count runtime workflow executions passed"
