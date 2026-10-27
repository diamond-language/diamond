#!/usr/bin/env bash
# Self-hosting is in minimal-compat maintenance mode (see docs/roadmap.md):
# the native language is still changing too fast for keeping full
# self-hosted parity to be worth its ongoing cost, so the exhaustive
# differential corpus (tests/lexer_diff.sh, tests/parser_diff.sh --
# together `make test-self-host`) no longer runs as part of `make
# test-all`. This is what does: two cheap, single-invocation bootstrap
# checks confirming the self-hosted frontend hasn't gone completely
# stale -- it can still parse its own source, and the result of doing so
# can still compile and run an independent third program correctly. Real
# regressions narrower than that (a specific construct newly diverging)
# won't be caught here; run `make test-self-host` for that.
set -euo pipefail

diamond=./build/diamond

# find_builtin_class in selfhost/parser.d6 hardcodes the VM's builtin
# exception classes and their indices; it has fallen behind the native table
# twice. Each entry of diamond_program_init_fresh's table (listed in enum
# order) must appear there with the same index.
builtin_index=0
while read -r builtin_name; do
    if ! grep -Fq "[\"$builtin_name\", $builtin_index," selfhost/parser.d6; then
        echo "selfhost/parser.d6 find_builtin_class lacks $builtin_name at index $builtin_index" >&2
        exit 1
    fi
    builtin_index=$((builtin_index + 1))
done < <(sed -n 's/^ *\[DIAMOND_CLASS_[A-Z0-9_]*\]={"\([A-Za-z0-9]*\)".*/\1/p' src/compiler_program.c)
if [[ "$builtin_index" -lt 23 ]]; then
    echo "found only $builtin_index builtin classes in src/compiler_program.c" >&2
    exit 1
fi
echo "self-hosted builtin exception classes match the native table"

self_parse_result="$(echo "selfhost/parser.d6" | $diamond selfhost/self_parse_check.d6 2>&1)"
if [[ "$self_parse_result" != "PARSED OK"* ]]; then
    echo "self-hosted parser failed to parse its own source: $self_parse_result" >&2
    exit 1
fi

echo "self-hosted parser self-parse bootstrap check passed"

# The real Phase 4 bootstrap: not just compiling its own source (above),
# but the compiled result actually running, and correctly using its own
# compiled Parser/Lexer classes to compile-and-run a third, independent
# target program -- a compiler compiling itself and then doing real work
# with the result. self_run_check.d6 appends a driver (read a path, call
# parse_and_run_with_core on it) to the bundled parser.d6+lexer.d6+
# core.d6 source before compiling, so running the self-compiled result
# performs the same "compile and run an arbitrary program" operation
# natively-run code does, one VM level deeper.
self_run_target="tests/parser_cases/generic_collection_constraints.d6"
self_run_expected="$("$diamond" "$self_run_target")"
self_run_actual="$(printf 'selfhost/parser.d6\n%s\n' "$self_run_target" | \
    "$diamond" selfhost/self_run_check.d6 2>&1 | sed '$d')"
if [[ "$self_run_actual" != "$self_run_expected" ]]; then
    echo "self-hosted parser self-run bootstrap mismatch" >&2
    echo "  expected: $self_run_expected" >&2
    echo "  actual:   $self_run_actual" >&2
    exit 1
fi

echo "self-hosted parser self-run bootstrap check passed"
