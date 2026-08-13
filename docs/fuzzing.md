# Fuzzing

`fuzz/compile_fuzzer.c` is a [libFuzzer](https://llvm.org/docs/LibFuzzer.html)
harness for `diamond_compile` — the lexer, parser, and bytecode emitter
together, the exact surface a `.di` file's raw bytes are exposed to. It's a
Clang-only build (`-fsanitize=fuzzer` is a Clang/LLVM feature GCC doesn't
implement at all), the one thing in this repository that isn't built with
the same compiler as everything else.

## Why compile-only, never execute

The harness calls `diamond_compile` and stops — it never calls
`diamond_vm_run` on the result. Diamond has real native bridges (`File`,
`TCPSocket`/`TCPServer`, `Regexp` — see `docs/io.md`), so actually
*executing* an arbitrary fuzzer-mutated program isn't safe without
sandboxing and resource limits this harness doesn't attempt: a mutated
program could open/write/delete real files, hold open sockets, or loop
forever, none of which a bare libFuzzer harness guards against on its own.
Compile-only fuzzing already covers the highest-value target — memory
safety in code that runs on 100% of untrusted input, before any of it is
ever considered "valid enough to run" — without that risk.

Fuzzing execution (the VM interpreting already-*compiled* bytecode) is a
distinct, separable, higher-effort project for later: it would need at
least a wall-clock/instruction budget per run and denying or faking out the
I/O bridges, neither of which exist today.

## Building and running

```sh
make fuzz          # build/compile_fuzzer, needs clang (not part of the default build)
make test-fuzz      # tests/fuzz_smoke.sh: a bounded 20-second regression check
```

`make test-fuzz` is deliberately short — a regression smoke test wired into
`make test-all`, not a real campaign, so the full test suite stays fast. It
seeds `compile_fuzzer`'s corpus from `tests/parser_cases/*.di` (the curated,
one-example-per-language-feature corpus — fast to load, still broad
coverage; the full 820-file `tests/cases/` would slow corpus loading for
no real benefit to a 20-second run) and fails loudly with the crashing
input's bytes if libFuzzer finds one.

For a real campaign — hours or days, not seconds — run the binary directly
against a persistent corpus directory so libFuzzer's own coverage-guided
mutation and corpus minimization actually have room to work:

```sh
mkdir -p corpus && cp tests/parser_cases/*.di tests/cases/*.di corpus/
./build/compile_fuzzer -max_len=8192 corpus/
```

Any crash gets written to `crash-<hash>` in the working directory (or
wherever `-artifact_prefix=` points) — reproduce it directly:

```sh
./build/compile_fuzzer crash-<hash>
```

## What's deliberately out of scope so far

- **Executing the compiled program** — see above.
- **Fuzzing `diamond_load_program`/`require` resolution** — involves real
  filesystem I/O (reading required files relative to a given path), which
  a pure in-memory `LLVMFuzzerTestOneInput(data, size)` harness has no
  natural way to drive; would need its own harness generating both source
  text and a small synthetic filesystem layout.
- **AFL++/honggfuzz** — libFuzzer was already available (Clang 22, this
  machine) with zero additional installation; no reason to add a second
  fuzzing engine until libFuzzer's coverage-guided mutation stops finding
  anything new.
