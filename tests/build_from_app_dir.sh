#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
diamond_bin="$repo_dir/build/diamond"
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT
export AOT_CACHE_ROOT="$test_dir/aot-cache"
fingerprint_extra="$test_dir/runtime-abi.marker"
export AOT_FINGERPRINT_EXTRA="$fingerprint_extra"
printf '%s\n' 'runtime ABI v1' > "$fingerprint_extra"

cat > "$test_dir/app.di" <<'EOF'
def increment(value: Int) -> Int = value + 1
i = 0
while i < 100
  i = increment(i)
end
i
EOF

cd "$test_dir"
"$diamond_bin" build app.di > "$test_dir/first-build.log"
test -x "$test_dir/app"
output="$("$test_dir/app")"
test "$output" = "100"
"$diamond_bin" build app.di -o app-again > "$test_dir/second-build.log"
test "$("$test_dir/app-again")" = "100"
if grep -Eq ' -c src/|ar rcs ' "$test_dir/second-build.log"; then
    echo "second standalone build rebuilt the cached runtime" >&2
    exit 1
fi
# A persistent cache may outlive the source copy that populated it. Prove that
# changing content invalidates the runtime archive even when its mtime is put
# back, as happens when an older-timestamped checkout replaces a newer one.
touch -r "$fingerprint_extra" "$test_dir/original-mtime"
printf '%s\n' 'runtime ABI v2' > "$fingerprint_extra"
touch -r "$test_dir/original-mtime" "$fingerprint_extra"
"$diamond_bin" build app.di -o app-after-abi-change > "$test_dir/third-build.log"
test "$("$test_dir/app-after-abi-change")" = "100"
if ! grep -Eq ' -c src/|ar rcs ' "$test_dir/third-build.log"; then
    echo "content change with preserved mtime did not rebuild the cached runtime" >&2
    exit 1
fi
DIAMOND_TRACE_JIT=1 "$test_dir/app" > "$test_dir/interpreted.out" 2> "$test_dir/interpreted.trace"
grep -q 'jit: 0 compiled function(s)' "$test_dir/interpreted.trace"
DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_TRACE_JIT=1 "$test_dir/app" > "$test_dir/jit.out" 2> "$test_dir/jit.trace"
test "$(cat "$test_dir/jit.out")" = "100"
grep -Eq 'jit: [1-9][0-9]* compiled function\(s\)' "$test_dir/jit.trace"
DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=200 DIAMOND_TRACE_JIT=1 "$test_dir/app" > "$test_dir/threshold.out" 2> "$test_dir/threshold.trace"
grep -q 'jit: 0 compiled function(s)' "$test_dir/threshold.trace"
printf '%s\n' "build from application directory passed"
