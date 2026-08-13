#!/usr/bin/env bash
set -euo pipefail

# A bounded regression smoke test, not a real fuzzing campaign: seeds
# build/compile_fuzzer's corpus from tests/parser_cases (the curated,
# one-example-per-feature corpus, not the full 820-file tests/cases --
# fast to load, still broad coverage) and runs it for a fixed, short
# amount of wall-clock time so `make test-all` stays fast. A real
# campaign (hours/days, no time bound) is a separate, manual invocation
# -- see docs/fuzzing.md.

fuzzer="$(realpath ./build/compile_fuzzer)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

corpus="$work/corpus"
mkdir -p "$corpus"
cp tests/parser_cases/*.di "$corpus/"

artifacts="$work/artifacts"
mkdir -p "$artifacts"

cd "$work"
if ! "$fuzzer" -max_total_time=20 -max_len=4096 \
        -artifact_prefix="$artifacts/" "$corpus" \
        >fuzz_output.log 2>&1; then
    echo "fuzz_smoke: compile_fuzzer found a crash" >&2
    cat fuzz_output.log >&2
    echo "--- artifacts ---" >&2
    ls -la "$artifacts" >&2
    exit 1
fi

runs="$(grep -oE 'Done [0-9]+ runs' fuzz_output.log | grep -oE '[0-9]+' || echo "?")"
echo "fuzz smoke test passed ($runs runs, 20s bound, no crash)"
