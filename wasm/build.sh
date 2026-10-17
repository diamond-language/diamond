#!/usr/bin/env bash
# Builds the browser REPL's WebAssembly module: build/wasm/diamond.wasm.
#
#   WASI_SDK=/opt/wasi-sdk wasm/build.sh
#
# Needs a WASI SDK (clang 19+ with the wasm32-wasip1 sysroot; https://github.com/WebAssembly/wasi-sdk)
# (plus curl, tar and openssl, and network access once, to fetch the SQLite source). PostgreSQL, MySQL,
# zlib, TLS and OpenSSL are compiled out (their features raise an ordinary error; Digest, HMAC,
# SecureRandom and Base64 use src/portable_crypto.c) and no host library headers are read.
# SQLite is the pinned amalgamation below, in-memory databases only. See docs/internal/wasm.md.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${WASI_SDK:?set WASI_SDK to the wasi-sdk directory}"
out="$root/build/wasm"
jobs="${JOBS:-2}"
mkdir -p "$out/obj"

# SQLite, pinned by SHA3-256 (the digest sqlite.org publishes next to the download). The
# autoconf tarball carries the amalgamation (sqlite3.c, sqlite3.h) and needs only tar and the
# openssl command to unpack and check.
sqlite_version=3540000
sqlite_year=2026
sqlite_sha3=f5e3e724e9c9b3b5d512c76b02082091a59eeea9becd169a9fbf4d376403acce
sqlite_dir="$out/sqlite-autoconf-$sqlite_version"
if [ ! -f "$sqlite_dir/sqlite3.c" ]; then
    curl -fsSL -o "$out/sqlite.tar.gz" \
        "https://www.sqlite.org/$sqlite_year/sqlite-autoconf-$sqlite_version.tar.gz"
    actual="$(openssl dgst -sha3-256 "$out/sqlite.tar.gz")"
    if [ "${actual##* }" != "$sqlite_sha3" ]; then
        echo "wasm/build.sh: SQLite source does not match its pinned SHA3-256" >&2
        exit 1
    fi
    tar xzf "$out/sqlite.tar.gz" -C "$out"
fi

flags=(--target=wasm32-wasip1 --sysroot="$sdk/share/wasi-sysroot"
    -DDIAMOND_NO_MYSQL -DDIAMOND_NO_POSTGRES -DDIAMOND_NO_ZLIB
    -DDIAMOND_NO_TLS -DDIAMOND_NO_CRYPTO
    -I"$root/wasm/include" -I"$sqlite_dir"
    -include "$root/wasm/include/diamond_wasm_shim.h"
    -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS
    -std=c23 -O2 -I"$root/src" -I"$root/lsp" -I"$root/reginold" -w)
cc="$sdk/bin/clang"

# Object files do not track flags: start over when they change.
stamp="$(printf '%s\n' "${flags[@]}" | sha256sum | cut -d' ' -f1)"
if [ "$(cat "$out/obj/.flags" 2>/dev/null)" != "$stamp" ]; then
    rm -f "$out"/obj/*.o
    printf '%s\n' "$stamp" > "$out/obj/.flags"
fi

# main.c is the CLI and repl.c the terminal REPL; the page drives wasm/repl_driver.c instead.
# The completion engine the terminal REPL's Tab key uses comes from lsp/ (the same list as
# REPL_COMPLETION_SOURCES in the Makefile).
sources=$(cd "$root" && ls src/*.c | grep -v -e src/main.c -e src/repl.c)
sources="$sources lsp/completion.c lsp/compile_buffer.c lsp/analysis_cache.c lsp/receiver.c lsp/json.c lsp/repl_completion.c"
running=0
for source in $sources wasm/repl_driver.c wasm/posix_stubs.c wasm/sqlite_vfs.c; do
    object="$out/obj/$(basename "${source%.c}").o"
    if [ -f "$object" ] && [ ! "$root/$source" -nt "$object" ]; then continue; fi
    nice -n 19 "$cc" "${flags[@]}" -c "$root/$source" -o "$object" &
    running=$((running + 1))
    if [ "$running" -ge "$jobs" ]; then wait -n; running=$((running - 1)); fi
done
wait
for source in $sources wasm/repl_driver.c wasm/posix_stubs.c wasm/sqlite_vfs.c; do
    [ -f "$out/obj/$(basename "${source%.c}").o" ] || { echo "wasm/build.sh: $source failed to compile" >&2; exit 1; }
done

# SQLite itself: no OS layer (wasm/sqlite_vfs.c supplies the few services an in-memory database
# needs), single-threaded, no extension loading, no WAL.
if [ ! -f "$out/obj/sqlite3.o" ] || [ "$sqlite_dir/sqlite3.c" -nt "$out/obj/sqlite3.o" ]; then
    nice -n 19 "$cc" --target=wasm32-wasip1 --sysroot="$sdk/share/wasi-sysroot" -Os -w \
        -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
        -DSQLITE_OMIT_WAL -DSQLITE_OMIT_SHARED_MEM -DSQLITE_TEMP_STORE=3 \
        -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 \
        -c "$sqlite_dir/sqlite3.c" -o "$out/obj/sqlite3.o"
fi

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
    -Wl,-z,stack-size=16777216 \
    -Wl,--initial-memory=134217728 -Wl,--max-memory=1073741824 \
    -o "$out/diamond.wasm" "$out"/obj/*.o "$out/reginold/libreginold.a" \
    -lm -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks
ls -l "$out/diamond.wasm"
