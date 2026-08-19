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

self_parse_result="$(echo "selfhost/parser.di" | $diamond selfhost/self_parse_check.di 2>&1)"
if [[ "$self_parse_result" != "PARSED OK"* ]]; then
    echo "self-hosted parser failed to parse its own source: $self_parse_result" >&2
    exit 1
fi

echo "self-hosted parser self-parse bootstrap check passed"

# The real Phase 4 bootstrap: not just compiling its own source (above),
# but the compiled result actually running, and correctly using its own
# compiled Parser/Lexer classes to compile-and-run a third, independent
# target program -- a compiler compiling itself and then doing real work
# with the result. self_run_check.di appends a driver (read a path, call
# parse_and_run_with_core on it) to the bundled parser.di+lexer.di+
# core.di source before compiling, so running the self-compiled result
# performs the same "compile and run an arbitrary program" operation
# natively-run code does, one VM level deeper.
self_run_target="tests/parser_cases/generic_collection_constraints.di"
self_run_expected="$("$diamond" "$self_run_target")"
self_run_actual="$(printf 'selfhost/parser.di\n%s\n' "$self_run_target" | \
    "$diamond" selfhost/self_run_check.di 2>&1 | sed '$d')"
if [[ "$self_run_actual" != "$self_run_expected" ]]; then
    echo "self-hosted parser self-run bootstrap mismatch" >&2
    echo "  expected: $self_run_expected" >&2
    echo "  actual:   $self_run_actual" >&2
    exit 1
fi

echo "self-hosted parser self-run bootstrap check passed"
