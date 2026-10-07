# VM source layout

`src/vm.c` is the interpreter, the garbage collector and the native service layer. It grew to over
26,000 lines; `vm.c` itself is now about 13,000 and `run_chunk` has its own file. This note records how it is organised, what has been split out and why, and the
measurements that decide what is worth splitting next.

## Where the compile time is

Measured with clang `-O1 -fsanitize=address,undefined` (what the fuzzers and `CC=clang make
sanitize` use), on one core:

| | lines | compile time |
|---|---|---|
| `vm.c` as it was | 26,544 | 363 s |
| `vm.c` with the body of `run_chunk` replaced by a stub | 16,713 | **3 s** |
| `vm_program_builder.c` | 1,734 | 2 s |
| `vm.c` after the per-handle `INVOKE_TYPED` blocks were outlined (`-O1 -g0`, asan+ubsan) | 19,080 | 273 s |
| `vm.c` after the Array/Hash/String and Int/Float blocks were outlined too | 19,145 | 227 s |
| `vm.c` once `run_chunk` moved to its own file | 13,053 | **6 s** |
| `vm_run_chunk.c` (`run_chunk`, `collection_invoke_fast` and the preamble) | 5,639 | 205 s |

So essentially all of the cost is `run_chunk`, the interpreter loop (8,171 lines, 31% of the file,
before the `INVOKE_TYPED` blocks were outlined; 5,521 lines now). Splitting `vm.c` cannot shorten a
full instrumented build, which still compiles `run_chunk` once, but it decides how much a change
*outside* `run_chunk` costs. With `run_chunk` in `vm_run_chunk.c`, an edit to the rest of the runtime
recompiles a 6-second file, and an edit to `run_chunk` recompiles only the 205-second one.
A change to `vm_internal.h` still recompiles both.

Under gcc at `-O0` the whole file takes about 7 s, and at `-O3` about 34 s, so this is a
sanitizer-build problem.

## What is inside `run_chunk`

148 opcode cases have braced bodies. One of them, `DIAMOND_OP_INVOKE_TYPED` (method calls), is
**1,808 lines** (3,104 before the cold per-handle blocks were outlined); the next largest is 238. It is
a chain of blocks keyed on the receiver's kind:

| block | lines |
|---|---|
| Array, Hash, String and other built-in natives | 1,115 |
| Int and Float natives | 254 |
| user-defined Instance dispatch and the rest | the remainder |

The per-kind blocks (Array/Hash/String, Int/Float, Supervisor, UDP socket, TLS socket, Channel,
Listener, Thread, File, Socket, Fiber, Regexp) are outlined into `*_invoke_helper` functions in `vm.c`, the same shape as
`time_dispatch_helper`, `tensor_dispatch_helper` and the database `*_dispatch_helper` functions. A
helper returns its status instead of using `VM_RETURN`; the call site propagates it.

The two hot blocks (`collection_invoke_helper`, `numeric_invoke_helper`) were measured with
`perf stat` user instructions and cycles over `bench/`: instructions rose about 1% (the extra call),
and cycles were neutral to better (string and native-read benchmarks 8-10% fewer). The exception was
`array_ops`, 2% more cycles: its loop is `Array#push` and `#length`, and the helper's frame saves
every callee-saved register and spills nine arguments, which costs more than those two calls do.
`collection_invoke_fast` answers exactly those two, plus `length` on Hash and String, at the call
site with the helper's own checks in the helper's order (`#push` still defers to a user-defined
`array_push` extension, and a frozen array, a type constraint, an arity mismatch or a cold extension
cache fall through to the helper). With it `array_ops` is 2% below its pre-outlining cycles.

## What has been split out

Code moves into its own file only when the interpreter reaches it through out-of-line helpers, so
that the move cannot change what the compiler inlines into `run_chunk` (`run_chunk` itself is the
exception, below). 123 `*_helper` functions
(6,802 lines) were already out of line on purpose, to keep `run_chunk`'s stack frame small.

| file | contents |
|---|---|
| `vm_program_builder.c` | the ProgramBuilder native bridge (`allocate_program_builder`, `program_builder_run_helper`, `program_builder_invoke_helper` and their small helpers) |
| `vm_tensor.c` | the Tensor natives |
| `vm_regexp.c` | the Regexp natives |
| `vm_json.c` | the native JSON parser and writer |
| `vm_time.c` | the Time natives |
| `vm_database.c` | the SQLite, PostgreSQL and MySQL drivers |
| `vm_process_io.c` | process spawning and file natives |
| `vm_network.c` | TLS, TCP, UDP and DNS resolution |
| `vm_run_chunk.c` | the interpreter loop, `run_chunk` |

What a file needs from `vm.c` is declared in `src/vm_internal.h`. It is not the embedding API, and its
symbols are hidden. Each cluster needed only 3 to 9 shared primitives (`allocate_string`,
`allocate_array`, `allocate_hash`, `array_push`, `builder_append`, ...).

## `run_chunk`'s own file

`src/vm_run_chunk.c` holds `run_chunk` and `collection_invoke_fast`. What it needs from `vm.c` is in
`src/vm_internal.h`: the frame, thread, supervisor and unwinding types, a prototype for each of the
roughly 120 functions it calls that `vm.c` defines (the `static` is removed from those definitions),
the four tables and counters it reads, and the small functions it inlined before. Those are
`static inline` in the header: `is_truthy`, `is_int_value`, `class_is_a`, `lookup_singleton_method`,
`cached_extension_lookup`, `allocate_closure`, `allocate_cell`, `gc_unprotect`, `value_is_bignum`
and a few more. They are the ones a baseline binary's `run_chunk` never called out of line. A
`#define` that only exists inside `vm.c` is invisible to the new file: `DIAMOND_ASAN_FIBERS` was one,
and without it the ASan fiber-switch annotations silently disappeared from the resume path.

What the split costs at run time is cross-unit inlining and interprocedural register allocation.
`run_chunk` is the only file that loses them, and a plain `-O3` build executes about 3% more
instructions (1.5% more cycles, geometric mean over `bench/`, up to 6% on
`jit_native_collection_reads`) than the one-file build. The release build therefore links with
`-flto` (`LTO_FLAGS` in the Makefile): instructions are then within 0.1% of the one-file build and
cycles 1% below it. The sanitizer and debug builds do not use it. Neither does the AOT runtime
archive, which `diamond build` links at `-O2` (see the roadmap for what an LTO build of it would cost).

## How a move is done

A move is mechanical: the function text, with its leading comment, leaves `vm.c` and arrives
unchanged. The only edits are `static` removed from the functions that now cross a file boundary.
That is checked by comparing the multiset of source lines before and after: the only original lines
missing afterwards are the changed signatures.
