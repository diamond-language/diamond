#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
diamond_bin="$repo_dir/build/diamond"
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT

cat > "$test_dir/app.di" <<'EOF'
def increment(value: Int) -> Int = value + 1
i = 0
while i < 100
  i = increment(i)
end
i
EOF

cd "$test_dir"
"$diamond_bin" build app.di
test -x "$test_dir/app"
output="$("$test_dir/app")"
test "$output" = "100"
DIAMOND_TRACE_JIT=1 "$test_dir/app" > "$test_dir/interpreted.out" 2> "$test_dir/interpreted.trace"
grep -q 'jit: 0 compiled function(s)' "$test_dir/interpreted.trace"
DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_TRACE_JIT=1 "$test_dir/app" > "$test_dir/jit.out" 2> "$test_dir/jit.trace"
test "$(cat "$test_dir/jit.out")" = "100"
grep -Eq 'jit: [1-9][0-9]* compiled function\(s\)' "$test_dir/jit.trace"
DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=200 DIAMOND_TRACE_JIT=1 "$test_dir/app" > "$test_dir/threshold.out" 2> "$test_dir/threshold.trace"
grep -q 'jit: 0 compiled function(s)' "$test_dir/threshold.trace"
printf '%s\n' "build from application directory passed"
