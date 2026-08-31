#!/usr/bin/env bash
# Baseline benchmark runner. Times each bench/*.di program via
# DIAMOND_REPEAT (repeats execution in-process against the same
# compiled chunk, so results measure interpreter throughput rather
# than process startup/compile overhead), then reports per-iteration
# wall time. Also captures DIAMOND_TRACE_OPCODES for one representative
# run of each benchmark to show which opcodes actually dominate.
#
# Usage: bash bench/run.sh [quicken]
#   With no argument: default settings (DIAMOND_QUICKEN unset).
#   "quicken": also sets DIAMOND_QUICKEN=1 for a second pass.
set -euo pipefail
cd "$(dirname "$0")/.."

diamond=./build/diamond
if [[ ! -x "$diamond" ]]; then
    echo "build/diamond not found -- run 'make release' first" >&2
    exit 1
fi

declare -A REPEATS=(
    [int_arithmetic]=15
    [int_arithmetic_dynamic]=10
    [dispatch_monomorphic]=15
    [dispatch_polymorphic]=12
    [dispatch_polymorphic_no_index]=12
    [dispatch_reassign_control]=15
    [dispatch_megamorphic]=12
    [fibonacci]=15
    [array_ops]=30
    [hash_ops]=100
    [string_ops]=40
    [closures]=25
    [fiber_switch]=10
    [exception_handling]=12
    [iterator_blocks]=5
    [range_iteration]=20
    [case_pattern_matching]=7
    [typed_dispatch]=6
)

run_pass() {
    local quicken_env="$1"
    for path in bench/*.di; do
        local name
        name="$(basename "$path" .di)"
        local repeat="${REPEATS[$name]:-10}"
        local start end elapsed per_iter
        start="$(date +%s.%N)"
        env $quicken_env DIAMOND_REPEAT="$repeat" "$diamond" "$path" >/dev/null
        end="$(date +%s.%N)"
        elapsed="$(echo "$end - $start" | bc)"
        per_iter="$(echo "scale=5; $elapsed / $repeat" | bc)"
        printf '%-24s repeat=%-4d total=%7.3fs  per-iter=%8.5fs\n' \
            "$name" "$repeat" "$elapsed" "$per_iter"
    done
}

echo "== default (DIAMOND_QUICKEN unset) =="
run_pass ""

if [[ "${1:-}" == "quicken" ]]; then
    echo
    echo "== DIAMOND_QUICKEN=1 =="
    run_pass "DIAMOND_QUICKEN=1"
fi

# DIAMOND_TRACE_OPCODES prints "opcode[N]: count" using the numeric
# DiamondOpCode enum value -- this array maps N back to its name.
# Extracted directly from src/vm.h's enum declaration order at run
# time, rather than hand-maintained, so it can never silently desync
# the way selfhost/parser.di's own hand-maintained opcode mirror once
# did (a real bug found and fixed 2026-08-31) -- any new opcode just
# shows up correctly named on the next run, no manual sync step.
mapfile -t OPCODE_NAMES < <(
    sed -n '/^typedef enum DiamondOpCode/,/^} DiamondOpCode;/p' src/vm.h \
        | grep -oP '(?<=    )DIAMOND_OP_\w+' \
        | sed -e 's/^DIAMOND_OP_//' -e '/^COUNT$/d'
)

echo
echo "== opcode hot-path breakdown (single run, top 12 by count) =="
for path in bench/*.di; do
    name="$(basename "$path" .di)"
    echo "--- $name ---"
    DIAMOND_TRACE_OPCODES=1 "$diamond" "$path" >/dev/null 2>"/tmp/${name}.opcodes"
    sed -E 's/opcode\[([0-9]+)\]: ([0-9]+)/\1 \2/' "/tmp/${name}.opcodes" \
        | sort -k2 -rn | head -12 \
        | while read -r idx count; do
              printf '  %-20s %s\n' "${OPCODE_NAMES[$idx]:-opcode$idx}" "$count"
          done
    rm -f "/tmp/${name}.opcodes"
done
