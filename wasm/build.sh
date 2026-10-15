#!/usr/bin/env bash
# Builds the browser REPL's WebAssembly module: build/wasm/diamond.wasm.
#
#   WASI_SDK=/opt/wasi-sdk wasm/build.sh
#
# Needs a WASI SDK (clang 19+ with the wasm32-wasip1 sysroot; https://github.com/WebAssembly/wasi-sdk)
# and the same development headers Diamond itself builds against (OpenSSL, SQLite, libpq, zlib):
# the runtime source still names them, and the headers are only read, never linked. The native
# calls they declare are imports the page replaces with errors (wasm/web/diamond-wasm.js), so a
# program that reaches TLS, a database server or a subprocess fails instead of linking.
# See docs/internal/wasm.md.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${WASI_SDK:?set WASI_SDK to the wasi-sdk directory}"
out="$root/build/wasm"
jobs="${JOBS:-2}"
mkdir -p "$out/include/openssl" "$out/obj"

# Headers the wasi sysroot lacks. Host copies are linked in; OpenSSL's per-architecture
# configuration.h is replaced by a concrete one (the wasm build only reads declarations).
for header in zlib.h zconf.h zlib_name_mangling.h sqlite3.h libpq-fe.h postgres_ext.h pg_config_ext.h; do
    for dir in /usr/include /usr/include/postgresql /usr/local/include; do
        [ -f "$dir/$header" ] && { ln -sf "$dir/$header" "$out/include/$header"; break; }
    done
done
openssl_dir=""
for dir in /usr/include/openssl /usr/include/x86_64-linux-gnu/openssl /usr/local/include/openssl; do
    [ -f "$dir/ssl.h" ] && { openssl_dir="$dir"; break; }
done
[ -n "$openssl_dir" ] || { echo "wasm/build.sh: OpenSSL development headers not found" >&2; exit 1; }
for header in "$openssl_dir"/*.h; do ln -sf "$header" "$out/include/openssl/$(basename "$header")"; done
python3 - "$openssl_dir" "$out/include/openssl/configuration.h" <<'PY'
import glob, os, sys
directory, target = sys.argv[1], sys.argv[2]
candidates = sorted(glob.glob(os.path.join(directory, "configuration-*.h")))
source = candidates[0] if candidates else os.path.join(directory, "configuration.h")
text = open(source).read()
marker = text.find("/*\n * WARNING: do not edit!")
if os.path.lexists(target): os.unlink(target)
open(target, "w").write(text[marker:] if marker >= 0 else text)
PY

flags=(--target=wasm32-wasip1 --sysroot="$sdk/share/wasi-sysroot"
    -DDIAMOND_NO_MYSQL -I"$root/wasm/include" -I"$out/include"
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
