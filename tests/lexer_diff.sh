#!/usr/bin/env bash
# Self-hosting Phase 2 differential harness: for every tests/cases/*.di
# file, compares the C lexer's token stream (tests/lexer_dump.c) against
# selfhost/lexer.di's Diamond-language port, run via
# selfhost/lexer_dump.di. A mismatch here is the first real signal that
# the port isn't faithful, well before any parser/emitter work depends
# on it. See docs/roadmap.md's self-hosting Phase 2 entry.
set -euo pipefail

diamond=./build/diamond
lexer_dump=./build/lexer_dump

count=0
for case_file in tests/cases/*.di; do
    expected="$("$lexer_dump" "$case_file")"
    # Strip the trailing "nil" line: every diamond run prints its
    # program's own top-level result after the driver's own output, and
    # selfhost/lexer_dump.di's last statement (a bare `break`) always
    # evaluates to nil.
    actual="$(echo "$case_file" | "$diamond" selfhost/lexer_dump.di | sed '$d')"
    if [[ "$actual" != "$expected" ]]; then
        echo "lexer token stream mismatch for $case_file" >&2
        diff <(echo "$expected") <(echo "$actual") >&2 || true
        exit 1
    fi
    count=$((count + 1))
done

echo "$count lexer differential cases passed"
