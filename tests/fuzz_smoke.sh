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
# (register_count selector byte 0x00 -> register_count=1, then a count byte, then a MOVE
# r64999, r0 -- opcode 5, dest 0xFDE7=64999, src 0x0000), so this
# specific regression stays covered even though libFuzzer starts every
# other input from nothing.
execute_corpus="$work/execute_corpus"
mkdir -p "$execute_corpus"
printf '\x00\x00\x00\x05\xfd\xe7\x00\x00' > "$execute_corpus/move_out_of_range_register"

# Seeded the same way, for a diamond_format_value_type SEGV a 2026-09-27
# fuzzing run found (exit_helper's "exit code must be an Int, got %s"
# formatting a DIAMOND_VALUE_CLASS register as if it held
# DIAMOND_VALUE_OBJECT, reinterpreting its small class_index byte as a
# pointer) -- pre-existing on main before the register-recycling merge
# that happened to surface it, not caused by it, and NOT limited to
# adversarial bytecode: `exit(Foo.bar())` from a `def self.bar() = self`
# singleton method crashes it too (tests/cases/exit_with_class_value.di).
# A single-bit mutation of the byte sequence below (libFuzzer's own
# ChangeBit) found a second variant of the same missing-case bug before
# the real fix (an explicit DIAMOND_VALUE_CLASS branch, not just a
# null-object-pointer guard) closed both at once.
printf '\x05\x00\x00\x6e\x00\x00\x00\x80\x00\x00\x09\x92\xff' \
    > "$execute_corpus/format_value_type_null_object"

# A third consumer of the same CLASS-kind-read-as-object-pointer bug:
# raising a class value (`raise Foo.bar()`, tests/cases/raise_class_value.di)
# reached format_uncaught_exception_message's unconditional `.as.object`
# read. Found by libFuzzer on CI after the fix above shipped.
printf '\x1b\x00\x00\x6e\x00\x00\x10\x3a\x00\x00\x0b\x4c\x16' \
    > "$execute_corpus/uncaught_exception_class_value"

# COLLECT_VARIADIC naming fixed parameter 4 on the entry function, which has
# no argument array at all: &arguments[4] from a null pointer, found by the
# extended harness (UBSan, clang).
printf '\x00\x00\x00\x81\x00\x00\x00\x04\x00\x00\x39\x00\x00' \
    > "$execute_corpus/collect_variadic_null_arguments"

# The four opcodes the sandbox does not gate (IO_POLL, SIGNAL_TRAP, THREAD_NEW, and
# TLS_START_HANDSHAKE, which the sandbox refuses before reading its operands) used to be
# rejected by the harness. libFuzzer essentially never builds the register setup they need
# unaided (a closure for the thread and the trap, two Arrays for the poll), so each gets a
# hand-built seed that reaches it: empty-Array IO.poll with a 5 ms timeout and with none (the
# wall-clock budget has to end that one), Signal.trap("INT") with a callable, Thread.new of a
# capture-free callable, and TLSSocket.start_handshake.
printf '\x04\x01\x00\x02\x05\x00\x00\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00\x33\x00\x01\x00\x00\x00\x00\x33\x00\x02\x00\x00\x00\x00\x4d\x00\x00\x00\x01\x00\x02\x00\x03' \
    > "$execute_corpus/io_poll_empty"
printf '\x04\x01\x00\x02\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x03\x00\x00\x33\x00\x01\x00\x00\x00\x00\x33\x00\x02\x00\x00\x00\x00\x4d\x00\x00\x00\x01\x00\x02\x00\x03' \
    > "$execute_corpus/io_poll_forever"
printf '\x03\x48\x00\x03\x49\x4e\x54\x00\x00\x00\x01\x00\x01\x00\x00\x1f\x00\x02\x00\x00\x00\x50\x00\x00\x00\x01\x00\x02' \
    > "$execute_corpus/signal_trap_int"
printf '\x03\x40\x00\x00\x00\x00\x1f\x00\x01\x00\x00\x00\x5b\x00\x00\x00\x01\x00\x02\x00' \
    > "$execute_corpus/thread_new_empty"
printf '\x03\x00\x00\xb1\x00\x00\x00\x01\x00\x02\x00\x03' \
    > "$execute_corpus/tls_start_handshake"

# The first three bytes of every execute_fuzzer input are the register count, a
# constants/strings/functions count byte and a type-set/interface/type-variable count
# byte (fuzz/execute_fuzzer.c), so the seeds above carry two 0x00 count bytes after the
# register byte (and a register byte with
# the top two bits clear: those say how many classes follow).
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
