# Fuzzing

`fuzz/compile_fuzzer.c` is a [libFuzzer](https://llvm.org/docs/LibFuzzer.html)
harness for `diamond_compile` — the lexer, parser, and bytecode emitter
together, the exact surface a `.d6` file's raw bytes are exposed to.
`fuzz/execute_fuzzer.c` is a second harness for `run_chunk` (via
`diamond_vm_run`) — the exact surface `ProgramBuilder`-constructed
bytecode exposes, and the one the pre-release audit's register-bounds
finding actually lived on (see "Fuzzing bytecode execution" below).
`fuzz/differential_fuzzer.c` compares deterministic source programs in the
interpreter and JIT, with GC stress in the native run. All three
are Clang-only builds (`-fsanitize=fuzzer` is a Clang/LLVM feature GCC
doesn't implement). These targets use Clang even when the regular build uses GCC.

## Compiler-only coverage

The compiler harness calls `diamond_compile` and stops — it never calls
`diamond_vm_run` on the result. d6 has real native bridges (`File`,
`TCPSocket`/`TCPServer`, `Regexp` — see `docs/io.md`), so actually
*executing* an arbitrary fuzzer-mutated program isn't safe without
sandboxing and resource limits this harness doesn't attempt: a mutated
program could open/write/delete real files, hold open sockets, or loop
forever, none of which a bare libFuzzer harness guards against on its own.
Compile-only fuzzing already covers the highest-value target — memory
safety in code that runs on 100% of untrusted input, before any of it is
ever considered "valid enough to run" — without that risk.

The differential harness below adds source-program execution with sandbox mode,
a bounded interpreter preflight and a terminating native timeout. It filters out
nondeterministic and I/O features; the compiler-only harness still accepts those
inputs without executing them.

## Fuzzing bytecode execution

`execute_fuzzer` fuzzes a narrower, already-safe-to-run slice of that same
surface: `run_chunk` directly, called on a synthetic `DiamondChunk` built
straight from the fuzzer's raw input bytes. Byte 0 picks the entry's
`register_count` (1..64, low six bits) and how many classes follow (top two
bits), byte 1 says how many constants (0..7), string constants (0..7) and
extra functions (0..3) follow, byte 2 says how many type sets per function (0..3), interfaces
(0..3) and type variables per function (0..3) there are, then come the constants
(Nil/Bool/Int/Float/Class), the strings, each function's arity, register count
and code, each class (superclass, field count, and methods bound to the
functions above, declared the way `declare_class`/`declare_field`/
`declare_method` do), each function's type sets and parameter/return annotations (built
under the rules `declare_type_set` enforces: a set references only earlier sets, ids are
primitives, existing classes or interfaces, or declared type variables), the interfaces'
methods, and finally the entry's own code. A function the builder declares has every
type-set field set to "none"; leaving them zeroed means "set 0", which does not exist yet. Every function gets the same
constants and strings, so an index valid in one is valid in all. Class constants
are in on purpose: a script can put one there (`add_constant(f, self)` inside a
singleton method) and its index only means something to the chunk it came from.
Without constants, strings and functions the harness could not reach any opcode
that names one -- the verifier rejected them all. This is
exactly what `ProgramBuilder`-constructed bytecode looks like from
`run_chunk`'s point of view — `ProgramBuilder#run`'s `#emit_byte`/
`#patch_byte` API lets d6 code append raw bytes to a function's code
array with no idea what instruction it's building, bypassing every
guarantee the compiler's own register allocator normally provides. It was
exactly this gap — a register operand read straight off the bytecode
stream with no check against the frame's own `register_count` — that the
pre-release audit found and `diamond_verify_bytecode` (`src/disassemble.c`)
now closes. `compile_fuzzer` structurally can't reach this bug class at
all: `diamond_compile`'s own register allocator (`allocate_register`)
guarantees every register a *compiled* program references was actually
reserved, so only hand-assembled bytecode can violate it.

This harness still respects the "never execute arbitrary I/O" rule above:
any chunk whose disassembly mentions an I/O-, thread-, or signal-capable
opcode (`FILE_OPEN`, `TCP_CONNECT`, `TLS_LISTEN`, `THREAD_NEW`, etc.) is
rejected before `diamond_vm_run` ever sees it. It's safe to run
unattended for the same reason `compile_fuzzer` is: nothing it does can
reach the filesystem, network, or spawn anything.

## Building and running

```sh
make fuzz          # build/compile_fuzzer and build/execute_fuzzer, needs clang (not part of the default build)
make test-fuzz      # tests/fuzz_smoke.sh: a bounded 20-second regression check for each
```

`make test-fuzz` is deliberately short — a regression smoke test wired into
`make test-all`, not a real campaign, so the full test suite stays fast. It
seeds `compile_fuzzer`'s corpus from `tests/parser_cases/*.d6` (the curated,
one-example-per-language-feature corpus — fast to load, still broad
coverage; the full 820-file `tests/cases/` would slow corpus loading for
no real benefit to a 20-second run) and fails loudly with the crashing
input's bytes if libFuzzer finds one. `execute_fuzzer`'s corpus is a single
seed: the exact byte sequence that reproduced the audit's register-bounds
bug (a `MOVE r64999, r0` inside a declared-1-register frame), so that
regression stays covered even though every other input starts from
nothing — raw bytecode bytes have no natural text corpus to seed from the
way `compile_fuzzer` seeds from `.d6` source.

For a real campaign — hours or days, not seconds — run either binary
directly against a persistent corpus directory so libFuzzer's own
coverage-guided mutation and corpus minimization actually have room to
work:

```sh
mkdir -p corpus && cp tests/parser_cases/*.d6 tests/cases/*.d6 corpus/
./build/compile_fuzzer -max_len=8192 corpus/

mkdir -p execute_corpus && printf '\x00\x00\x00\x05\xfd\xe7\x00\x00' > execute_corpus/seed
./build/execute_fuzzer -max_len=4096 execute_corpus/
```

Any crash gets written to `crash-<hash>` in the working directory (or
wherever `-artifact_prefix=` points) — reproduce it directly:

```sh
./build/compile_fuzzer crash-<hash>
./build/execute_fuzzer crash-<hash>
```

The bytecode execution run is bounded by the VM's own `DIAMOND_MAX_INSTRUCTIONS` and
`DIAMOND_MAX_WALL_MILLISECONDS` budgets, not a timer that jumps out of the run:
nothing may `siglongjmp` out of the VM, because doing so from inside a garbage
collection leaves the thread-local mark stack mid-drain and the next input's
collection marks stale pointers. (An earlier watchdog did that, and the fuzzer
reported the resulting use-after-free as a runtime bug until it was traced.)

## Differential execution

`fuzz/differential_fuzzer.c` (`make build/differential_fuzzer`) compiles and runs each
source input twice in one process, once plain and once with `DIAMOND_JIT=1`,
`DIAMOND_JIT_THRESHOLD=1` and `DIAMOND_STRESS_MINOR_GC=1`, and aborts when exit status,
stdout or stderr differ. It runs sandboxed and skips inputs that name nondeterministic or
I/O features. The interpreter preflight has instruction and wall-clock budgets; inputs
that exhaust them are skipped before native execution. The native run clears VM budgets,
which otherwise disable the JIT, and uses libFuzzer's terminating timeout instead. The
harness defaults to `-timeout=10`; supply a larger positive value for a slower campaign.
A native hang terminates the fuzzer and records a timeout artifact rather than resuming
an interrupted VM. Seed it from small deterministic files in `tests/cases`.

From the repository root, this builds the executable and runs in the foreground
with progress visible. It stops after 10,000 inputs or about 90 seconds, or sooner
if it encounters an error or an individual input exceeds the timeout:

```sh
mkdir -p /tmp/d6-differential/corpus /tmp/d6-differential/artifacts
cp tests/cases/jit_captured_store_error.d6 tests/cases/retry_captured_locals.d6 /tmp/d6-differential/corpus/
make -j2 build/differential_fuzzer && \
  ./build/differential_fuzzer -runs=10000 -max_total_time=90 -max_len=4096 \
    -artifact_prefix=/tmp/d6-differential/artifacts/ /tmp/d6-differential/corpus/
status=$?
printf 'Exit status: %s\n' "$status"
```

The harness accepts source inputs up to 4096 bytes. Copy `.d6` source files rather
than generated `.d6c` caches into the corpus. LibFuzzer adds and minimizes inputs
in that directory during the run. `make clean`, including the clean step at the
start of `make test-integration`, removes the executable; rebuild it afterwards
before starting another run. Keep the corpus and artifacts outside `build/` to
preserve them across clean builds.

A successful bounded run prints `Done ... runs` and exits 0. Progress lines such
as `NEW`, `REDUCE`, or `pulse` are not a completion result. A nonzero exit or a
return to the shell without that summary needs investigation. Inspect the saved
`crash-*` or `timeout-*` input in the artifact directory; a timeout alone does not
establish a language defect. Because program stdout/stderr are captured during
comparison, a terminating diagnostic may not appear in the terminal even though
an artifact was saved.

It is not part of `make test-fuzz`. Each input costs two ASan-instrumented runs, so run it
with few workers at low priority (`nice -n 19 build/differential_fuzzer -jobs=2 -workers=2
...`); six workers plus a desktop session made a development machine unresponsive. Avoid
`-fork=N`: libFuzzer's fork driver crashed inside its own bookkeeping after a few thousand
runs while the harness itself was healthy.

## What's deliberately out of scope so far

- **General I/O and nondeterministic program execution** — the differential
  harness deliberately filters these features; `execute_fuzzer` covers synthetic
  bytecode execution instead.
- **Fuzzing `diamond_load_program`/`require` resolution** — involves real
  filesystem I/O (reading required files relative to a given path), which
  a pure in-memory `LLVMFuzzerTestOneInput(data, size)` harness has no
  natural way to drive; would need its own harness generating both source
  text and a small synthetic filesystem layout.
- **AFL++/honggfuzz** — libFuzzer was already available (Clang 22, this
  machine) with zero additional installation; no reason to add a second
  fuzzing engine until libFuzzer's coverage-guided mutation stops finding
  anything new.
