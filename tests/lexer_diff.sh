#!/usr/bin/env bash
# Self-hosting Phase 2 differential harness: for every tests/cases/*.di
# file, compares the C lexer's token stream (tests/lexer_dump.c) against
# selfhost/lexer.di's Diamond-language port. Driven by
# selfhost/lexer_diff_suite.di, a single Minitest suite that requires
# lexer.di exactly once and loops over every case in-process -- see that
# file's own comment for why (a fresh `diamond` process per case used to
# dominate `make test-all`'s wall time, per docs/roadmap.md). A mismatch
# here is the first real signal that the port isn't faithful, well
# before any parser/emitter work depends on it.
set -euo pipefail

diamond=./build/diamond
case_files=(tests/cases/*.di)
"$diamond" selfhost/lexer_diff_suite.di "${case_files[@]}"
