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
execute_fuzzer="$(realpath ./build/execute_fuzzer)"
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

# execute_fuzzer's input is raw bytecode bytes, not Diamond source, so
# there's no natural text corpus to seed from the way compile_fuzzer
# seeds from tests/parser_cases -- seed instead with the exact byte
# sequence that reproduced the pre-release audit's register-bounds bug
# (register_count selector byte 0x00 -> register_count=1, then a MOVE
# r64999, r0 -- opcode 5, dest 0xFDE7=64999, src 0x0000), so this
# specific regression stays covered even though libFuzzer starts every
# other input from nothing.
execute_corpus="$work/execute_corpus"
mkdir -p "$execute_corpus"
printf '\x00\x05\xfd\xe7\x00\x00' > "$execute_corpus/move_out_of_range_register"

execute_artifacts="$work/execute_artifacts"
mkdir -p "$execute_artifacts"

if ! "$execute_fuzzer" -max_total_time=20 -max_len=4096 \
        -artifact_prefix="$execute_artifacts/" "$execute_corpus" \
        >execute_fuzz_output.log 2>&1; then
    echo "fuzz_smoke: execute_fuzzer found a crash" >&2
    cat execute_fuzz_output.log >&2
    echo "--- artifacts ---" >&2
    ls -la "$execute_artifacts" >&2
    exit 1
fi

execute_runs="$(grep -oE 'Done [0-9]+ runs' execute_fuzz_output.log | grep -oE '[0-9]+' || echo "?")"
echo "fuzz smoke test passed ($execute_runs execute runs, 20s bound, no crash)"
