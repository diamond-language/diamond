#!/usr/bin/env bash
# An installed diamond links standalone programs from its prebuilt AOT kit,
# without this checkout or make.
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT
prefix="$test_dir/prefix"

make -C "$repo_dir" --no-print-directory install PREFIX="$prefix" > "$test_dir/install.log"
test -x "$prefix/bin/diamond"
for file in libdiamond-aot.a libreginold.a version cc compile.args link.args; do
    test -s "$prefix/lib/diamond/aot/$file"
done

# Any attempt to fall back to the checkout's Makefile fails loudly.
mkdir "$test_dir/shim"
printf '#!/bin/sh\necho "make must not run" >&2\nexit 99\n' > "$test_dir/shim/make"
chmod +x "$test_dir/shim/make"

mkdir "$test_dir/app"
cd "$test_dir/app"
printf '%s\n' 'def double(value: Int) -> Int = value * 2' 'double(21)' > app.di
PATH="$test_dir/shim:$PATH" "$prefix/bin/diamond" build app.di > build.log
grep -q "diamond: built 'app'" build.log
test "$(./app)" = "42"

# A kit from another Diamond version is rejected instead of linked.
cp -R "$prefix/lib/diamond/aot" "$test_dir/other-kit"
printf '%s\n' '0.0.0-other' > "$test_dir/other-kit/version"
if DIAMOND_AOT_KIT="$test_dir/other-kit" "$prefix/bin/diamond" build app.di -o other 2> other.err; then
    echo "mismatched AOT kit was accepted" >&2
    exit 1
fi
grep -q 'is for Diamond 0.0.0-other' other.err
test ! -e other

printf '%s\n' "installed AOT kit build passed"
