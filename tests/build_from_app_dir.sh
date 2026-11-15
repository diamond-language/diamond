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

cat > "$test_dir/app.d6" <<'EOF'
def increment(value: Int) -> Int = value + 1
i = 0
while i < 100
  i = increment(i)
end
puts(i)
EOF

cd "$test_dir"
"$diamond_bin" build app.d6 > "$test_dir/first-build.log"
test -x "$test_dir/app"
output="$("$test_dir/app")"
test "$output" = "100"
"$diamond_bin" build app.d6 -o app-again > "$test_dir/second-build.log"
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
"$diamond_bin" build app.d6 -o app-after-abi-change > "$test_dir/third-build.log"
test "$("$test_dir/app-after-abi-change")" = "100"
if ! grep -Eq ' -c src/|ar rcs ' "$test_dir/third-build.log"; then
    echo "content change with preserved mtime did not rebuild the cached runtime" >&2
    exit 1
fi
DIAMOND_TRACE_JIT=1 "$test_dir/app" > "$test_dir/interpreted.out" 2> "$test_dir/interpreted.trace"
grep -q 'jit: 0 compiled function(s)' "$test_dir/interpreted.trace"
# The standalone binary compiles exactly what d6 itself compiles for the same program: some
# functions where the JIT exists (x86-64), none where it does not, so the reference is measured
# here rather than assumed from the architecture.
jit_count() { sed -n 's/^jit: \([0-9]*\) compiled function(s).*$/\1/p' "$1"; }
DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_TRACE_JIT=1 "$diamond_bin" app.d6 > /dev/null 2> "$test_dir/reference.trace"
DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_TRACE_JIT=1 "$test_dir/app" > "$test_dir/jit.out" 2> "$test_dir/jit.trace"
test "$(cat "$test_dir/jit.out")" = "100"
reference_count="$(jit_count "$test_dir/reference.trace")"
test -n "$reference_count"
if [ "$(jit_count "$test_dir/jit.trace")" != "$reference_count" ]; then
    echo "standalone binary compiled $(jit_count "$test_dir/jit.trace") function(s), d6 compiled $reference_count" >&2
    exit 1
fi
DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=200 DIAMOND_TRACE_JIT=1 "$test_dir/app" > "$test_dir/threshold.out" 2> "$test_dir/threshold.trace"
grep -q 'jit: 0 compiled function(s)' "$test_dir/threshold.trace"

# Plain `d6 build` builds ./app.d6 and names the binary after the directory.
mkdir "$test_dir/greeter"
cp "$test_dir/app.d6" "$test_dir/greeter/app.d6"
cd "$test_dir/greeter"
"$diamond_bin" build > "$test_dir/default-build.log"
grep -q "built 'greeter'" "$test_dir/default-build.log"
test "$(./greeter)" = "100"
test ! -e ./app
# Options still work without SOURCE, and -o still wins.
"$diamond_bin" build -o named > /dev/null
test "$(./named)" = "100"
# A legacy app.di is used when there is no app.d6.
mkdir "$test_dir/legacy"
cp "$test_dir/app.d6" "$test_dir/legacy/app.di"
cd "$test_dir/legacy"
"$diamond_bin" build > /dev/null
test "$(./legacy)" = "100"
# No app file: usage error (64), not a crash or a build of something else.
mkdir "$test_dir/empty"
cd "$test_dir/empty"
status=0
"$diamond_bin" build 2> "$test_dir/missing.err" || status=$?
test "$status" = 64
grep -q "no main.d6 or app.d6 in the current directory" "$test_dir/missing.err"
# main.d6 (what `facet new` generates) comes first when both exist.
mkdir "$test_dir/both"
cp "$test_dir/app.d6" "$test_dir/both/app.d6"
printf '%s\n' 'puts("from main")' > "$test_dir/both/main.d6"
cd "$test_dir/both"
"$diamond_bin" build > /dev/null
test "$(./both)" = "from main"
# The directory name taken by a subdirectory: refuse instead of failing to link.
mkdir -p "$test_dir/clash/clash"
cp "$test_dir/app.d6" "$test_dir/clash/app.d6"
cd "$test_dir/clash"
status=0
"$diamond_bin" build 2> "$test_dir/clash.err" || status=$?
test "$status" = 64
grep -q "output 'clash' is a directory" "$test_dir/clash.err"
# `--version` alone names the binary: Cutfile name and version, else the binary's
# name and the git revision, else no version; the d6 version comes last. With other
# arguments it reaches the program like any argument.
d6_version="$("$diamond_bin" --version | awk '{print $2}')"
mkdir "$test_dir/versioned"
printf '%s\n' 'puts(ARGV.join(","))' > "$test_dir/versioned/main.d6"
printf '%s\n' '{"name": "greeter", "version": "2.3.1", "dependencies": {}}' > "$test_dir/versioned/Cutfile"
cd "$test_dir/versioned"
"$diamond_bin" build > /dev/null
test "$(./versioned --version)" = "greeter 2.3.1 (d6 $d6_version)"
test "$(./versioned --version extra)" = "--version,extra"
"$diamond_bin" build --app-name=svc --app-version='v1 "q"' -o svc > /dev/null
test "$(./svc --version)" = 'svc v1 "q" (d6 '"$d6_version)"
status=0
"$diamond_bin" build --app-version= 2> "$test_dir/empty-version.err" || status=$?
test "$status" = 64
mkdir "$test_dir/plain"
cp "$test_dir/versioned/main.d6" "$test_dir/plain/main.d6"
cd "$test_dir/plain"
GIT_CEILING_DIRECTORIES="$test_dir" "$diamond_bin" build > /dev/null
test "$(./plain --version)" = "plain (d6 $d6_version)"
if command -v git > /dev/null; then
    git init -q && git add main.d6
    git -c user.email=test@example.invalid -c user.name=test commit -qm init
    revision="$(git rev-parse --short=12 HEAD)"
    "$diamond_bin" build > /dev/null
    test "$(./plain --version)" = "plain $revision (d6 $d6_version)"
    printf '%s\n' '# changed' >> main.d6
    "$diamond_bin" build > /dev/null
    test "$(./plain --version)" = "plain $revision-dirty (d6 $d6_version)"
fi
printf '%s\n' "build from application directory passed"
