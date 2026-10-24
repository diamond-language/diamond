# VM source layout

`src/vm.c` is the interpreter, the garbage collector and the native service layer. It grew to over
26,000 lines; `vm.c` itself is now about 10,000 and `run_chunk` has its own file. This note records how it is organised, what has been split out and why, and the
measurements that decide what is worth splitting next.

## Where the compile time is

The timings and line counts below are historical measurements from the VM
extraction, not measurements of the current release. Later compiler and dispatch
helper refactors do not imply a build-speed improvement.

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
before the `INVOKE_TYPED` blocks were outlined; 3,559 lines now). Splitting `vm.c` cannot shorten a
full instrumented build, which still compiles `run_chunk` once, but it decides how much a change
*outside* `run_chunk` costs. With `run_chunk` in `vm_run_chunk.c`, an edit to the rest of the runtime
recompiles a 6-second file, and an edit to `run_chunk` recompiles only the 205-second one.
A change to `vm_internal.h` still recompiles both.

Under gcc at `-O0` the whole file takes about 7 s, and at `-O3` about 34 s, so this is a
sanitizer-build problem.

## What is inside `run_chunk`

152 opcode cases have braced bodies. The largest, `DIAMOND_OP_INVOKE_TYPED` (method calls), is
**258 lines** (3,104 before the cold per-handle blocks were outlined); the next largest is
`DIAMOND_OP_TAIL_CALL` at 132. It was a chain of blocks keyed on the receiver's kind:

| block | lines |
|---|---|
| Array, Hash, String and other built-in natives | 1,115 |
| Int and Float natives | 254 |
| user-defined Instance dispatch and the rest | the remainder |

The per-kind blocks (Array/Hash/String, Int/Float, Supervisor, UDP socket, TLS socket, Channel,
Listener, Thread, File, Socket, Fiber, Regexp) are outlined into `*_invoke_helper` functions (in `vm.c` or the files below), the same shape as
`time_dispatch_helper`, `tensor_dispatch_helper` and the database `*_dispatch_helper` functions. A
helper returns its status instead of using `VM_RETURN`; the call site propagates it. The handle
kinds from Channel through ProgramBuilder are dispatched from one more outlined function,
`outlined_native_object_invoke`, which the case skips for an Instance receiver. Thread stays
inline, since it hands a `DIAMOND_VM_EXCEPTION` to this frame's handlers itself.

The special methods every native receiver answers (`public_send`, `tap`, `shuffle`/`sample`,
`arity`, `to_s`/`inspect`, the `dup` family, bignum formatting) are name-checked inline, in their
original order, because the checks run on every native-receiver call. Only the longer bodies they
select (`tap`, `shuffle`/`sample`, `arity` and the two bignum conversions) are `outlined_*`
functions.

The two hot blocks (`collection_invoke_helper`, `numeric_invoke_helper`) were measured with
`perf stat` user instructions and cycles over `bench/`: instructions rose about 1% (the extra call),
and cycles were neutral to better (string and native-read benchmarks 8-10% fewer). The exception was
`array_ops`, 2% more cycles: its loop is `Array#push` and `#length`, and the helper's frame saves
every callee-saved register and spills nine arguments, which costs more than those two calls do.
`collection_invoke_fast` answers exactly those two, plus `length` on Hash and String, at the call
site with the helper's own checks in the helper's order (`#push` still defers to a user-defined
`array_push` extension, and a frozen array, a type constraint, an arity mismatch or a cold extension
cache fall through to the helper). With it `array_ops` is 2% below its pre-outlining cycles.

Whole opcode cases that are cold or carry large locals are outlined into `outlined_*` functions at
the top of `vm_run_chunk.c`: the database and TLS opens, `Thread.new`, `Channel.select`, `IO.poll`,
the spread and keyword call families, `NEW_SPREAD` and `BREAKPOINT_CHECK`, among others. The case
body moves unchanged. The macros it uses are redefined above `run_chunk` to return the status, and the
call site applies `VM_PROPAGATE` to it, so a failure reaches the same handlers it did inline. That
holds only because none of these bodies passes `DIAMOND_VM_EXCEPTION` to `VM_RETURN`: inline, that
skips this frame's rescue clauses (`catch_runtime_error` has no class for it), while the call site's
`VM_PROPAGATE` would offer it to them. Outlining matters for the call-depth limit as well as for size:
the frame of `run_chunk` is the per-call stack cost that `DIAMOND_MAX_CALL_DEPTH` is calibrated
against, and every case's locals share it. Outlining `IO.poll` (two 256-entry result arrays and a
`pollfd` array), the call families and `BREAKPOINT_CHECK` took it from 25.9 KB to 15.4 KB at `-O3`
and from 65.3 KB to 24.7 KB under ASan; measure it with `gcc -fstack-usage`.

## What has been split out

Code moves into its own file only when the interpreter reaches it through out-of-line helpers, so
that the move cannot change what the compiler inlines into `run_chunk` (`run_chunk` itself is the
exception, below). 123 `*_helper` functions
(6,802 lines) were already out of line on purpose, to keep `run_chunk`'s stack frame small.

| file | contents |
|---|---|
| `vm_program_builder.c` | the ProgramBuilder native bridge (`allocate_program_builder`, `program_builder_run_helper`, `program_builder_invoke_helper` and their small helpers) |
| `vm_tensor.c` | the Tensor natives |
| `vm_regexp.c` | the Regexp natives and their method dispatch (`regexp_invoke_helper`) |
| `vm_json.c` | the native JSON parser and writer |
| `vm_time.c` | the Time natives and Time formatting (`format_time_iso8601`, the `%z` substitution) |
| `vm_database.c` | the SQLite, PostgreSQL and MySQL drivers |
| `vm_process_io.c` | process spawning, file natives (`file_invoke_helper`) and interruptible line reading (`read_line`) |
| `vm_network.c` | TLS, TCP, UDP and DNS resolution, and the Socket, Listener, UDPSocket and TLSSocket methods |
| `vm_run_chunk.c` | the interpreter loop, `run_chunk` |
| `vm_debugger.c` | terminal debugger pauses, structured DAP pauses and debug-control pipe framing |
| `vm_crypto.c` | BCrypt, SecureRandom, SHA-256/SHA-1 digests and HMAC, AES-GCM and Base64 (Gzip stays in `vm.c`) |
| `vm_string.c` | the String methods (`string_invoke_helper`, `String#format`, `String#tr`) |
| `vm_calls.c` | calling closures, functions and methods from the interpreter and JIT (`call_closure_helper`, `jit_call_core`, `diamond_jit_invoke_instance`, `diamond_jit_native_call`), `public_send`, keyword-argument merging, and backtrace capture |
| `vm_types.c` | runtime type checks (`value_matches_type`, structural interface matching against the native member tables, `diamond_jit_check_type`/`is_type`) and generic type binding and inference (`bind_context_set`, `infer_from_value`) |
| `vm_format.c` | value formatting: type names for error messages, `builder_format_value`, `inspect`/`to_s` (`stringify_value`, `inspect_value`), uncaught-exception messages and `Array#join` |
| `vm_values.c` | copying values between VMs (`copy_value_into_vm` and its per-kind steps), equality (`values_equal`), hashing and the Hash table operations (`hash_find`, `hash_set`, `hash_delete`) |
| `vm_collections.c` | the Array/Hash/String and Int/Float method dispatch (`collection_invoke_helper`, `numeric_invoke_helper`), the prelude-bridged collection methods, and `respond_to?` for built-in receivers |
| `vm_gc.c` | the garbage collector: marking (roots, frames, fibers, the remembered set), `sweep_list`, the card-marking write barriers, and `diamond_vm_collect`/`_minor` (allocation accounting and `maybe_collect` stay in `vm.c` with the resource budgets) |
| `vm_operators.c` | user-defined operator dispatch (`invoke_operator_method`), `case`/`when` matching, `method_missing`, the `+` fallbacks and the Int arithmetic and comparison slow paths (`int_arith_slow`, `compare_int_slow`) |
| `vm_exceptions.c` | catching exceptions and runtime errors in a frame's rescue handlers (`catch_exception`, `catch_runtime_error`), arity-error messages, and non-local `break`/`return` arrival (`nonlocal_exit_arrives`) |
| `vm_concurrency.c` | Thread, Channel and Supervisor: thread accounting and creation, program cloning, the thread and supervised-child entry points, and the Fiber, Thread, Channel and Supervisor methods (the fiber switching core, and handle allocation and freeing, stay in `vm.c`) |

Within `vm_program_builder.c`, `program_builder_invoke_helper` only checks whether
the program is still owned by the builder and dispatches by method name. The 35
method implementations are separate static helpers in the same file, each taking
only the state it uses. Arity checks, type validation, mutations and results stay
together in each implementation; unknown-method errors stay in the dispatcher.

The debugger helpers use the existing internal declarations for stringification,
input, buffers and frames; moving them adds no shared VM interfaces. Exception
backtrace capture stays in `vm.c` because it is part of raising an exception,
independent of an attached debugger.

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
