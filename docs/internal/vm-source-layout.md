# VM source layout

`src/vm.c` is the interpreter, the garbage collector and the native service layer. It grew to over
26,000 lines; `vm.c` itself is now about 19,000. This note records how it is organised, what has been split out and why, and the
measurements that decide what is worth splitting next.

## Where the compile time is

Measured with clang `-O1 -fsanitize=address,undefined` (what the fuzzers and `CC=clang make
sanitize` use), on one core:

| | lines | compile time |
|---|---|---|
| `vm.c` as it was | 26,544 | 363 s |
| `vm.c` with the body of `run_chunk` replaced by a stub | 16,713 | **3 s** |
| `vm_program_builder.c` | 1,734 | 2 s |

So essentially all of the cost is `run_chunk`, the interpreter loop (8,171 lines, 31% of the file).
The other 16,700 lines compile in seconds. Splitting `vm.c` into files therefore cannot shorten a
full instrumented build, but it does decide how much a change *outside* `run_chunk` costs: an edit
to anything left in the same translation unit still pays for `run_chunk`.

Under gcc at `-O0` the whole file takes about 7 s, and at `-O3` about 34 s, so this is a
sanitizer-build problem.

## What is inside `run_chunk`

148 opcode cases have braced bodies, 7,688 of its 8,171 lines. One of them,
`DIAMOND_OP_INVOKE_TYPED` (method calls), is **3,104 lines**; the next largest is 238. It is a chain of
blocks keyed on the receiver's kind:

| block | lines |
|---|---|
| Array, Hash, String and other built-in natives | 1,115 |
| Int and Float natives | 254 |
| Supervisor, UDP socket, TLS socket, Channel, Listener, Thread, File, Socket, Fiber, Regexp | about 1,350 together |
| user-defined Instance dispatch and the rest | the remainder |

The per-handle blocks have the same shape as `time_dispatch_helper`, `tensor_dispatch_helper` and the
database `*_dispatch_helper` functions, which are already outlined.

## What has been split out

Code moves into its own file only when the interpreter reaches it through out-of-line helpers, so
that the move cannot change what the compiler inlines into `run_chunk`. 123 `*_helper` functions
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

What a file needs from `vm.c` is declared in `src/vm_internal.h`. It is not the embedding API, and its
symbols are hidden. Each cluster needed only 3 to 9 shared primitives (`allocate_string`,
`allocate_array`, `allocate_hash`, `array_push`, `builder_append`, ...).

## How a move is done

A move is mechanical: the function text, with its leading comment, leaves `vm.c` and arrives
unchanged. The only edits are `static` removed from the functions that now cross a file boundary.
That is checked by comparing the multiset of source lines before and after: the only original lines
missing afterwards are the changed signatures.
