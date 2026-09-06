#ifndef DIAMOND_COMPILED_PRELUDE_H
#define DIAMOND_COMPILED_PRELUDE_H

#include "compiler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* A same-build-only serialization of an already-compiled DiamondProgram
 * (see compiled_prelude.c's own top comment for the full design and why
 * it's safe): not a portable file format, not versioned, not meant to
 * survive a different compiler/architecture/struct-layout than the one
 * that wrote it -- generated and consumed by the exact same `make`
 * invocation, the same way an object file is. Used to let the `diamond`
 * CLI itself (one program per process, so it has no earlier in-process
 * compile to amortize against, unlike tests/run_cases.c's own
 * diamond_compile_incremental use) skip lexing/parsing the ~24-36KB
 * embedded prelude on every single invocation -- see docs/roadmap.md's
 * "Make programs start faster".
 *
 * Writes to `file` (already open for binary writing); returns false on
 * any I/O failure, leaving `file`'s contents unspecified. Used only by
 * tools/gen_compiled_prelude.c, a build-time-only generator -- never
 * linked into the `diamond` binary itself. */
bool diamond_program_write_compiled(const DiamondProgram *program, FILE *file);

/* The inverse: reads a buffer produced by diamond_program_write_compiled
 * (typically a `#embed`ded, process-lifetime-static array -- see
 * src/compiled_prelude_data.c) into `out`, which must already be
 * diamond_program_init'd (or freshly calloc'd; either way, in the same
 * state diamond_compile itself expects to receive `program` in).
 * `out`'s own dynamic arrays end up genuinely, independently heap-
 * allocated (via the same diamond_function_copy every other program-
 * cloning path already uses -- see clone_program_from_chunk, src/vm.c),
 * so `out` is a perfectly ordinary DiamondProgram afterward: safe to
 * pass to diamond_compile_incremental as a template, and safe to
 * diamond_program_free like any other, unlike a hypothetical zero-copy
 * scheme that aliased the embedded buffer directly. Returns false if
 * `data` is malformed or too short for its own embedded counts (should
 * never happen for a buffer this project's own build just generated,
 * but never trusted blindly -- see the function's own comment). */
bool diamond_program_read_compiled(const uint8_t *data, size_t size, DiamondProgram *out);

#endif
