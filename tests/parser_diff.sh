#!/usr/bin/env bash
# Self-hosting Phase 3 sub-phase 1 differential harness: for every
# tests/parser_cases/*.di file, compares the real C-compiled-and-run
# result against the same source run through selfhost/parser.di's
# Diamond-language Parser + the native ProgramBuilder bridge. Unlike
# Phase 2's lexer_diff.sh, this can't reuse the full tests/cases/*
# corpus -- sub-phase 1 only supports a deliberately narrow grammar
# slice (literals, arithmetic/comparison/logical expressions, locals,
# if/while/loop/break; no functions, classes, calls, or interpolation
# -- see selfhost/parser.di's own header comment), so this harness has
# its own small, hand-curated case set instead. See docs/roadmap.md's
# self-hosting Phase 3 entry.
set -euo pipefail

diamond=./build/diamond

count=0
for case_file in tests/parser_cases/*.di; do
    expected="$("$diamond" "$case_file")"
    actual="$(echo "$case_file" | "$diamond" selfhost/parser_run.di | sed '$d')"
    if [[ "$actual" != "$expected" ]]; then
        echo "parser result mismatch for $case_file" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        exit 1
    fi
    count=$((count + 1))
done

echo "$count parser differential cases passed"
