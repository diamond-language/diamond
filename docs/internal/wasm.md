# WebAssembly build and the browser REPL

`wasm/build.sh` compiles the interpreter to `wasm32-wasip1` so a page can run Diamond with no
server. `build/wasm/diamond.wasm` is about 3.3 MB (950 KB gzipped).

```sh
WASI_SDK=/path/to/wasi-sdk wasm/build.sh        # needs wasi-sdk 24+ (clang 19+)
cd build/wasm && cp ../../wasm/web/* . && python3 -m http.server 8000   # open http://localhost:8000
```

## What is in the module

The runtime sources (built as `MINIMAL=1`), minus `src/main.c` and `src/repl.c`, plus
`wasm/repl_driver.c`, which exports
`diamond_wasm_eval(source)`: compile the candidate against a prelude template built once, run it in
a fresh VM, print the output, then a `0x1E` separator and the result value, or print a
`line:column: error: message` to stderr. `wasm/web/diamond-wasm.js` keeps the REPL session as text
and replays it for every line, as the terminal REPL does (`src/repl.c`): only output beyond what the
previous run printed is shown. An entry is unfinished when the parser's error lies past the last
newline-terminated line of the candidate.

## Building and testing

`make test-wasm WASI_SDK=/path/to/wasi-sdk` (or `wasm/build.sh` then `node tests/wasm_repl_test.mjs`)
builds the module and drives it through `wasm/web/diamond-wasm.js`: results, session state,
continuation lines, the portable crypto's known answers, and an ordinary error for every
unavailable feature. CI runs it as the `test-wasm` job, downloading wasi-sdk 34.

## What does not work, and how it fails

- **Fibers** need stack switching. `getcontext` is a stub that fails, so `Fiber.new` raises.
- **Threads, Supervisor, Channel** need pthreads; wasi-libc without threads has none.
- **SQLite, PostgreSQL, MySQL, zlib, TLS and OpenSSL** are compiled out (the `MINIMAL=1`
  switches): `SQLite3.open`, `PostgreSQL.open`, `MySQL.open`, `Gzip.compress`, `TLSServer.listen` and
  `Cipher.encrypt` raise an ordinary error ("... is not compiled into this build"), and none of
  their headers is needed to build. `Digest`, `HMAC`, `SecureRandom` and `Base64` still work, through
  `src/portable_crypto.c` (SHA-256, SHA-1, HMAC, Base64, and `getentropy` for random bytes).
- **Sockets, name lookup, subprocesses, signal handlers and temporary files** are not available.
  `wasm/posix_stubs.c` defines those calls to fail with `ENOSYS`, so a program gets an ordinary
  error ("cannot connect to 'example.com:80': name lookup is not available in the browser",
  `Process.run: pipe: Not supported`). The module has no imports besides WASI's, and links
  without `--allow-undefined`, so an unexpected new dependency is a build error.
- **Files**: the WASI shim exposes stdout and stderr only.
- **JIT**: x86-64 only, so the interpreter runs.
- **Deep recursion**: there is no 512 MiB main thread; the 16 MiB link-time stack bounds the call
  depth through the same scaling the fiber and thread stacks use.

## Layout assumptions that held

The compiler and VM compiled for 32-bit pointers with the existing `-Wconversion` discipline and
needed only: the `ucontext` stub in `src/vm.h`, missing-header stubs in `wasm/include`, and the
SHA-256 above. The compiled-prelude image is struct-dumped and fingerprinted by struct sizes, so
the 64-bit `build/compiled_prelude.bin` linked into the module is rejected at load and the prelude
is compiled from source once per page load (about 100 ms).

## Not done

- SQLite from the amalgamation, as an opt-in for the browser.
- AES-GCM (`Cipher`) in portable code.
