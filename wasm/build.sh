#!/usr/bin/env bash
# Builds the browser REPL's WebAssembly module: build/wasm/diamond.wasm.
#
#   WASI_SDK=/opt/wasi-sdk wasm/build.sh
#
# Needs a WASI SDK (clang 19+ with the wasm32-wasip1 sysroot; https://github.com/WebAssembly/wasi-sdk)
# (plus python3 and network access once, to fetch the SQLite amalgamation). PostgreSQL, MySQL,
# zlib, TLS and OpenSSL are compiled out (their features raise an ordinary error; Digest, HMAC,
# SecureRandom and Base64 use src/portable_crypto.c) and no host library headers are read.
# SQLite is the pinned amalgamation below, in-memory databases only. See docs/internal/wasm.md.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${WASI_SDK:?set WASI_SDK to the wasi-sdk directory}"
out="$root/build/wasm"
jobs="${JOBS:-2}"
mkdir -p "$out/obj"

# The SQLite amalgamation, pinned by SHA3-256 (the digest sqlite.org publishes).
sqlite_version=3540000
sqlite_year=2026
sqlite_sha3=7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93
sqlite_dir="$out/sqlite-amalgamation-$sqlite_version"
if [ ! -f "$sqlite_dir/sqlite3.c" ]; then
    curl -fsSL -o "$out/sqlite.zip" \
        "https://www.sqlite.org/$sqlite_year/sqlite-amalgamation-$sqlite_version.zip"
    python3 - "$out/sqlite.zip" "$sqlite_sha3" "$out" <<'PY'
import hashlib, sys, zipfile
path, expected, out = sys.argv[1:4]
data = open(path, "rb").read()
if hashlib.sha3_256(data).hexdigest() != expected:
    sys.exit("wasm/build.sh: SQLite amalgamation does not match its pinned SHA3-256")
zipfile.ZipFile(path).extractall(out)
PY
fi

flags=(--target=wasm32-wasip1 --sysroot="$sdk/share/wasi-sysroot"
    -DDIAMOND_NO_MYSQL -DDIAMOND_NO_POSTGRES -DDIAMOND_NO_ZLIB
    -DDIAMOND_NO_TLS -DDIAMOND_NO_CRYPTO
    -I"$root/wasm/include" -I"$sqlite_dir"
    -include "$root/wasm/include/diamond_wasm_shim.h"
    -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS
    -std=c23 -O2 -I"$root/src" -I"$root/reginold" -w)
cc="$sdk/bin/clang"

# Object files do not track flags: start over when they change.
stamp="$(printf '%s\n' "${flags[@]}" | sha256sum | cut -d' ' -f1)"
if [ "$(cat "$out/obj/.flags" 2>/dev/null)" != "$stamp" ]; then
    rm -f "$out"/obj/*.o
    printf '%s\n' "$stamp" > "$out/obj/.flags"
fi

# main.c is the CLI and repl.c the terminal REPL; the page drives wasm/repl_driver.c instead.
sources=$(cd "$root" && ls src/*.c | grep -v -e src/main.c -e src/repl.c)
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
