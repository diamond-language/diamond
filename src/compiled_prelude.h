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
 * embedded prelude on every single invocation -- see CHANGELOG.md's
 * "Performance".
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

/* Captures exactly what diamond_program_write_compiled/_read_compiled's
 * own raw-struct-dump format depends on being identical between the
 * build that wrote a blob and the build reading it back -- see
 * diamond_cache_fingerprint's own comment below. Every field is a plain
 * uint32_t (no padding ambiguity), so the two functions below compare
 * one of these wholesale via memcmp rather than field-by-field. This is
 * new territory those two functions above never needed: their own
 * writer and reader are always the exact same `make` invocation
 * (tools/gen_compiled_prelude.c generating what src/compiled_prelude_
 * data.c's own #embed immediately consumes), but a cache file written
 * by diamond_program_write_cache_file below can persist on disk across
 * separate `diamond` invocations, potentially spanning a rebuilt or
 * upgraded binary -- see docs/caching.md. */
typedef struct DiamondCacheFingerprint {
    uint32_t format_version;
    uint32_t function_size;
    uint32_t class_size;
    uint32_t interface_size;
    uint32_t module_size;
    uint32_t opcode_count;
    uint32_t builtin_class_count;
    uint32_t max_classes;
    uint32_t max_methods;
} DiamondCacheFingerprint;

/* Computes this build's own fingerprint fresh, from sizeof()/enum-count
 * constants -- there is no "known good" table to keep in sync by hand,
 * except format_version itself (bump it whenever this cache *file*
 * format changes in some way none of the other fields would happen to
 * catch, e.g. reordering fields in this very function). Deliberately
 * biased toward over-invalidating: any mismatch anywhere is treated as
 * a plain cache miss (one extra recompile), never specially diagnosed,
 * because the alternative -- accepting a blob this build didn't
 * actually produce -- risks feeding run_chunk raw bytecode bytes it
 * never validated, the same unsafe-raw-bytecode category diamond_
 * verify_bytecode (src/disassemble.h) exists to guard against
 * elsewhere. */
DiamondCacheFingerprint diamond_cache_fingerprint(void);

/* Reads a cache file previously written by diamond_program_write_cache_
 * file at `path`. Returns false -- a plain cache miss, never an error
 * the caller needs to report -- if the file doesn't exist, its magic/
 * fingerprint/source_hash don't match exactly, or its contents are
 * truncated/malformed in any way; `program` is left completely untouched
 * on any false return. Bounds-checked throughout, the same discipline
 * diamond_program_read_compiled already has -- never crashes on a
 * corrupted or hand-edited file. `source_hash` is the caller's own
 * SHA-256 of the exact source this program would be compiled from (see
 * src/run_source.c) -- this function never hashes anything itself, only
 * compares the 32 bytes given against what the file itself claims. */
bool diamond_program_read_cache_file(const char *path,
    const uint8_t source_hash[32], DiamondProgram *program);

/* Best-effort: writes `program` to a fresh cache file at `path`, tagged
 * with `source_hash` and this build's own diamond_cache_fingerprint(),
 * via a same-directory temporary file plus an atomic rename -- safe
 * against a second `diamond` process concurrently reading or writing the
 * same path (an in-progress writer's temp file is invisible at `path`
 * until the rename lands; a reader that already opened the old `path`
 * keeps reading that same, still-complete inode even if a rename
 * replaces the path underneath it). Any failure (read-only directory,
 * disk full, a concurrent writer losing the rename race, ...) is
 * silent: caching is a pure optimization a caller's own already-
 * successful compile/run must never be affected by. */
void diamond_program_write_cache_file(const char *path,
    const uint8_t source_hash[32], const DiamondProgram *program);

#endif
