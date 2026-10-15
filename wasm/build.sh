#!/usr/bin/env bash
# Builds the browser REPL's WebAssembly module: build/wasm/diamond.wasm.
#
#   WASI_SDK=/opt/wasi-sdk wasm/build.sh
#
# Needs a WASI SDK (clang 19+ with the wasm32-wasip1 sysroot; https://github.com/WebAssembly/wasi-sdk)
# and nothing else: the build is MINIMAL=1's configuration, so SQLite, PostgreSQL, MySQL, zlib,
# TLS and OpenSSL are compiled out (their features raise an ordinary error; Digest, HMAC,
# SecureRandom and Base64 use src/portable_crypto.c) and no host library headers are read.
# See docs/internal/wasm.md.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${WASI_SDK:?set WASI_SDK to the wasi-sdk directory}"
out="$root/build/wasm"
jobs="${JOBS:-2}"
mkdir -p "$out/obj"

flags=(--target=wasm32-wasip1 --sysroot="$sdk/share/wasi-sysroot"
    -DDIAMOND_NO_MYSQL -DDIAMOND_NO_POSTGRES -DDIAMOND_NO_SQLITE -DDIAMOND_NO_ZLIB
    -DDIAMOND_NO_TLS -DDIAMOND_NO_CRYPTO
    -I"$root/wasm/include"
    -include "$root/wasm/include/diamond_wasm_shim.h"
    -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS
    -std=c23 -O2 -I"$root/src" -I"$root/reginold" -w)
cc="$sdk/bin/clang"

# main.c is the CLI and repl.c the terminal REPL; the page drives wasm/repl_driver.c instead.
sources=$(cd "$root" && ls src/*.c | grep -v -e src/main.c -e src/repl.c)
running=0
for source in $sources wasm/repl_driver.c; do
    object="$out/obj/$(basename "${source%.c}").o"
    if [ -f "$object" ] && [ ! "$root/$source" -nt "$object" ]; then continue; fi
    nice -n 19 "$cc" "${flags[@]}" -c "$root/$source" -o "$object" &
    running=$((running + 1))
    if [ "$running" -ge "$jobs" ]; then wait -n; running=$((running - 1)); fi
done
wait

# reginold (the regular-expression engine) for wasm, built from a scratch copy.
if [ ! -f "$out/reginold/libreginold.a" ]; then
    rm -rf "$out/reginold"
    cp -R "$root/reginold" "$out/reginold"
    find "$out/reginold" \( -name '*.o' -o -name '*.a' \) -delete
    make -C "$out/reginold" -j"$jobs" \
        CC="$cc --target=wasm32-wasip1 --sysroot=$sdk/share/wasi-sysroot -D_WASI_EMULATED_MMAN" \
        AR="$sdk/bin/llvm-ar" libreginold.a >/dev/null 2>&1
fi

"$cc" --target=wasm32-wasip1 --sysroot="$sdk/share/wasi-sysroot" -mexec-model=reactor \
    -Wl,--allow-undefined -Wl,-z,stack-size=16777216 \
    -Wl,--initial-memory=134217728 -Wl,--max-memory=1073741824 \
    -o "$out/diamond.wasm" "$out"/obj/*.o "$out/reginold/libreginold.a" \
    -lm -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks
ls -l "$out/diamond.wasm"
