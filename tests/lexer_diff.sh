#!/usr/bin/env bash
# Self-hosting Phase 2 differential harness: for every tests/cases/*.d6
# file, compares the C lexer's token stream (tests/lexer_dump.c) against
# selfhost/lexer.d6's Diamond-language port. Driven by
# selfhost/lexer_diff_suite.d6, a single Minitest suite that requires
# lexer.d6 exactly once and loops over every case in-process -- see that
# file's own comment for why (a fresh `diamond` process per case used to
# dominate `make test-all`'s wall time, per docs/roadmap.md). A mismatch
# here is the first real signal that the port isn't faithful, well
# before any parser/emitter work depends on it.
set -euo pipefail

diamond=./build/diamond
case_files=(tests/cases/*.d6)
"$diamond" selfhost/lexer_diff_suite.d6 "${case_files[@]}"
