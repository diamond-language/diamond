# WebAssembly build and the browser REPL

`wasm/build.sh` compiles the interpreter to `wasm32-wasip1` so a page can run Diamond with no
server. `build/wasm/diamond.wasm` is about 3.3 MB (950 KB gzipped).

```sh
WASI_SDK=/path/to/wasi-sdk wasm/build.sh        # needs wasi-sdk 24+ (clang 19+)
cd build/wasm && cp ../../wasm/web/* . && python3 -m http.server 8000   # open http://localhost:8000
```

## What is in the module

The runtime sources, minus `src/main.c` and `src/repl.c`, plus `wasm/repl_driver.c`, which exports
`diamond_wasm_eval(source)`: compile the candidate against a prelude template built once, run it in
a fresh VM, print the output, then a `0x1E` separator and the result value, or print a
`line:column: error: message` to stderr. `wasm/web/diamond-wasm.js` keeps the REPL session as text
and replays it for every line, as the terminal REPL does (`src/repl.c`): only output beyond what the
previous run printed is shown. An entry is unfinished when the parser's error lies past the last
newline-terminated line of the candidate.

## What does not work, and how it fails

- **Fibers** need stack switching. `getcontext` is a stub that fails, so `Fiber.new` raises.
- **Threads, Supervisor, Channel** need pthreads; wasi-libc without threads has none.
- **Sockets, TLS, subprocesses, PostgreSQL, SQLite, MySQL, zlib, OpenSSL** are not linked.
  The calls are `env` imports the page fills with functions that throw; `DiamondRepl.run`
  catches that, reports "X is not available in the browser" and re-instantiates the module.
  Anything that would hash with OpenSSL (`Digest`, `HMAC`) is in this group; the source hash used
  for bytecode caching has a portable SHA-256 under `#ifdef __wasm__`.
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

- A page that embeds the module on dilang.tech (the site makeover).
- C-level stubs for the unavailable native calls, so they fail as Diamond errors instead of
  traps; a `NO_TLS`/`NO_POSTGRES`/`NO_SQLITE` family of switches like `NO_MYSQL`.
- SQLite from the amalgamation.
- A CI job (needs a cached wasi-sdk).
