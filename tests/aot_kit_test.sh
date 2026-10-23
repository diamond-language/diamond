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
printf '%s\n' 'def double(value: Int) -> Int = value * 2' 'double(21)' > app.d6
PATH="$test_dir/shim:$PATH" "$prefix/bin/diamond" build app.d6 > build.log
grep -q "diamond: built 'app'" build.log
test "$(./app)" = "42"

# A kit from another Diamond version is rejected instead of linked.
cp -R "$prefix/lib/diamond/aot" "$test_dir/other-kit"
printf '%s\n' '0.0.0-other' > "$test_dir/other-kit/version"
if DIAMOND_AOT_KIT="$test_dir/other-kit" "$prefix/bin/diamond" build app.d6 -o other 2> other.err; then
    echo "mismatched AOT kit was accepted" >&2
    exit 1
fi
grep -q 'is for Diamond 0.0.0-other' other.err
test ! -e other

# --lto needs the kit's LTO runtime: a kit without one says how to build it.
if "$prefix/bin/diamond" build app.d6 --lto -o nolto 2> nolto.err; then
    echo "--lto linked without an LTO runtime" >&2
    exit 1
fi
grep -q 'AOT_LTO=1' nolto.err
test ! -e nolto

# With the runtime built (AOT_LTO=1) --lto links and runs. Skipped when the compiler cannot link
# LTO objects at all (Clang without a linker plugin or lld).
cc_name="$(cat "$prefix/lib/diamond/aot/cc")"
printf 'int main(void) { return 0; }\n' > "$test_dir/lto_probe.c"
if "$cc_name" -flto "$test_dir/lto_probe.c" -o "$test_dir/lto_probe" 2> /dev/null; then
    make -C "$repo_dir" --no-print-directory aot-kit AOT_LTO=1 AOT_KIT_DIR="$test_dir/lto-kit" > "$test_dir/lto-kit.log"
    test -s "$test_dir/lto-kit/libdiamond-aot-lto.a"
    test -s "$test_dir/lto-kit/compile-lto.args"
    DIAMOND_AOT_KIT="$test_dir/lto-kit" PATH="$test_dir/shim:$PATH" \
        "$prefix/bin/diamond" build app.d6 --lto -o app_lto > lto.log
    grep -q "diamond: built 'app_lto'" lto.log
    test "$(./app_lto)" = "42"
else
    echo "skipping the --lto build: $cc_name cannot link LTO objects here"
fi

printf '%s\n' "installed AOT kit build passed"
