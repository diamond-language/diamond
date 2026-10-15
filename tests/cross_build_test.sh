#!/usr/bin/env bash
# `diamond build --target=TRIPLE` links from a per-target kit. The kit lookup
# and validation always run; building and running a real cross binary needs a
# toolchain and sysroot, so it only runs when DIAMOND_CROSS_SYSROOT and
# DIAMOND_CROSS_TARGET are set (DIAMOND_CROSS_FLAGS_EXTRA passes
# CROSS_FLAGS_EXTRA, DIAMOND_CROSS_MAKE_ARGS adds make variables such as
# "NO_MYSQL=1 STATIC=1" for a static musl kit, and DIAMOND_CROSS_RUN names an
# emulator such as "qemu-aarch64-static -L /path/to/sysroot").
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT
prefix="$test_dir/prefix"

make -C "$repo_dir" --no-print-directory install PREFIX="$prefix" > "$test_dir/install.log"
diamond="$prefix/bin/diamond"

mkdir "$test_dir/app"
cd "$test_dir/app"
printf '%s\n' 'def double(value: Int) -> Int = value * 2' 'double(21)' > app.di

# No kit for the target: a clear error, exit 74, and no output file.
set +e
"$diamond" build app.di -o missing --target=nowhere-linux-gnu 2> missing.err
status=$?
set -e
test "$status" -eq 74
grep -q "no AOT kit for target 'nowhere-linux-gnu'" missing.err
test ! -e missing

# A target name is a directory name, so path characters are refused.
set +e
"$diamond" build app.di -o bad --target=../escape 2> bad.err
status=$?
set -e
test "$status" -eq 64
grep -q "invalid build target" bad.err

# A kit recorded for a different target is rejected rather than linked.
mkdir -p "$prefix/lib/diamond/aot/targets"
cp -R "$prefix/lib/diamond/aot" "$test_dir/wrong-kit"
rm -rf "$test_dir/wrong-kit/targets"
printf '%s\n' 'other-linux-gnu' > "$test_dir/wrong-kit/target"
cp -R "$test_dir/wrong-kit" "$prefix/lib/diamond/aot/targets/wanted-linux-gnu"
set +e
"$diamond" build app.di -o wrong --target=wanted-linux-gnu 2> wrong.err
status=$?
set -e
test "$status" -eq 74
grep -q "is for target 'other-linux-gnu', not 'wanted-linux-gnu'" wrong.err
test ! -e wrong

# A real cross build, when a toolchain is available.
if [ -n "${DIAMOND_CROSS_SYSROOT:-}" ] && [ -n "${DIAMOND_CROSS_TARGET:-}" ]; then
    make -C "$repo_dir" --no-print-directory aot-kit-cross \
        TARGET_TRIPLE="$DIAMOND_CROSS_TARGET" SYSROOT="$DIAMOND_CROSS_SYSROOT" \
        CROSS_FLAGS_EXTRA="${DIAMOND_CROSS_FLAGS_EXTRA:-}" \
        ${DIAMOND_CROSS_MAKE_ARGS:-} > "$test_dir/kit.log"
    DIAMOND_AOT_KIT= "$repo_dir/build/diamond" build app.di -o cross \
        --target="$DIAMOND_CROSS_TARGET" > cross.log
    grep -q "built 'cross' for $DIAMOND_CROSS_TARGET" cross.log
    case "$(file cross)" in
        *ELF*) ;;
        *) echo "cross build did not produce an ELF executable" >&2; exit 1 ;;
    esac
    case " ${DIAMOND_CROSS_MAKE_ARGS:-} " in
        *" STATIC=1 "*)
            case "$(file cross)" in
                *"statically linked"*) ;;
                *) echo "STATIC=1 build is not statically linked" >&2; exit 1 ;;
            esac
            ;;
    esac
    if [ -n "${DIAMOND_CROSS_RUN:-}" ]; then
        # shellcheck disable=SC2086
        test "$($DIAMOND_CROSS_RUN ./cross)" = "42"
    fi
fi

printf '%s\n' "cross build kit lookup passed"
