#ifndef DIAMOND_VM_INTERNAL_H
#define DIAMOND_VM_INTERNAL_H

/* Declarations shared between src/vm.c and the vm_*.c files split out of it.
 *
 * vm.c grew past 26,000 lines, one function (run_chunk) of them over 8,000, and an edit anywhere
 * recompiled all of it: about six minutes under clang's ASan+UBSan. The subsystems that run_chunk
 * reaches only through out-of-line helpers (ProgramBuilder, the database drivers, network, time,
 * tensors, JSON, regexps, processes and files) move into their own files; what they share with the
 * interpreter is declared here.
 *
 * This is not the embedding API (that is vm.h). Nothing here is a stable interface, and the
 * symbols are hidden, so they do not widen what a shared build of the runtime exports. */

#include "vm.h"
#include "compiler.h"
#include <time.h>

#define DIAMOND_INTERNAL __attribute__((visibility("hidden")))

enum { TIME_BEGINNING_OF_DAY,TIME_END_OF_DAY,TIME_BEGINNING_OF_MONTH,
    TIME_END_OF_MONTH,TIME_BEGINNING_OF_WEEK,TIME_END_OF_WEEK,
    TIME_BEGINNING_OF_YEAR,TIME_END_OF_YEAR,TIME_BEGINNING_OF_QUARTER,
    TIME_END_OF_QUARTER };

enum { TIME_SHIFT_DAYS,TIME_SHIFT_WEEKS,TIME_SHIFT_MONTHS,TIME_SHIFT_YEARS };

enum { DIAMOND_TIME_LOCAL,DIAMOND_TIME_UTC,DIAMOND_TIME_FIXED_OFFSET };

/* Types shared by the files split out of vm.c. */
typedef struct GrowBuffer {
    char *data;
    size_t length;
    size_t capacity;
} GrowBuffer;

/* Each run_chunk activation unconditionally allocates DiamondValue
 * registers[256] (4KB), DiamondTypeBinding bindings[8] (~3.2KB), and
 * UnwindHandler handlers[16] (~0.5KB) as C-stack locals, regardless of the
 * called function's actual complexity. Measured against this machine's
 * default 8MB stack, real (non-instrumented) recursion segfaults around
 * depth ~210-220 in a debug (-O0) build and ~150-160 under AddressSanitizer's
 * redzone-inflated frames -- both well below the depth this counter used to
 * allow, so the guard never had a chance to trip before the native stack
 * actually overflowed.
 *
 * Recalibrated 100 -> 95 when adding native Time support: `-fstack-usage`
 * showed only ~240 bytes of *reported* growth from the new opcodes/
 * arithmetic-and-comparison fallback branches, but that was enough to
 * flip `depth(5000)` (tests/run.sh) from a clean guard trip to a real
 * ASan stack-overflow -- exactly the redzone-per-named-local amplification
 * the READ_SHORT incident just below already documents, not a byte-count
 * story. Empirically, every value from 91 through 99 passed cleanly
 * against the post-Time frame size (100 was the only one that didn't).
 *
 * Recalibrated 95 -> 92 (2026-09-27) when Ubuntu 26.04's own apt-packaged
 * GCC moved to 15.2.0: at 95, a plain `make release` (-O3, no
 * instrumentation at all) segfaulted on `depth(5000)` under that specific
 * compiler -- this project's own GCC (Fedora, 16.2.1) still passed at 95,
 * so this was invisible until CI's `ubuntu:26.04` image actually ran it
 * (found via a local Docker container matching that image and toolchain
 * exactly, not a hunch). The safe window this time was only {92, 93} --
 * confirmed against `main` at the commit before this recalibration too,
 * so this was newly exposed by the toolchain, not by anything this
 * project changed -- much narrower than the 91-99 window above, so
 * there is very little margin left for the next opcode/local addition to
 * this function; a future recalibration may need to shrink run_chunk's
 * own stack footprint directly rather than only retuning this constant
 * again. 92 sits at the low end of that narrow window rather than the
 * middle, since a smaller value only ever makes a real overflow *less*
 * likely to be reached before this guard trips -- the one thing that
 * must still hold is `legacy_0091.di`'s own `depth(90)` succeeding (a
 * hard floor, confirmed at 92). If a future change reopens this margin
 * again, re-run the same empirical sweep -- across every build variant
 * this project ships (debug, release, sanitize, and a container image
 * matching whatever CI target regressed), not just the one you're
 * sitting at -- rebuild at a range of candidate values, check
 * `depth(5000)` at each, rather than guessing.
 *
 * run_chunk's own `registers` array (below) stays a fixed
 * DIAMOND_INLINE_REGISTER_COUNT-wide C-stack array, at the same 256 this
 * comment's own measurement was taken against, rather than a VLA sized to
 * DIAMOND_REGISTER_COUNT (4096, see vm.h) -- a function whose
 * live_register_count exceeds it (rare -- see DIAMOND_REGISTER_COUNT's
 * own comment) heap-allocates instead, which doesn't consume C stack at
 * all and so can't affect this depth calibration regardless of how large
 * it gets. That register-count widening's first pass also nearly broke
 * this guard for a completely different reason, worth remembering: the
 * new READ_SHORT (below) originally declared its own `high_`/`low_`
 * uint8_t locals to assemble each 16-bit operand, and with ~90 opcodes
 * now reading 1-4 such operands apiece, that put several hundred extra
 * named locals into this one function -- at -O0, under ASan, each got
 * its own padded/redzoned stack slot, which dwarfed the cost of the
 * registers array itself (shrinking it 256->64 barely moved the
 * measured crash depth) and pushed the real crash below depth 90,
 * *under* this very call-depth guard, silently defeating it exactly
 * like an oversized array would have. Rewriting READ_SHORT to read
 * straight out of chunk->code[] into `target_` without any named
 * intermediate restored the original margin. */
enum { DIAMOND_MAX_CALL_DEPTH = 92 };

/* Types shared by the files split out of vm.c. */
/* A growable byte buffer for regexp_replace_helper's own output, the only
 * place in this file that needs to build a string of unknown final length
 * incrementally rather than in one allocate_string call. */
typedef struct ByteBuffer {
    char *data;
    size_t length;
    size_t capacity;
} ByteBuffer;

/* Set on a StringBuilder by stringify_value so builder_format_value can
 * call a user-defined to_s on instances nested in an Array or Hash. Without
 * one (every other caller), nested instances print as #<ClassName>. */
typedef struct FormatContext {
    DiamondVm *vm;
    const DiamondChunk *chunk;
    size_t depth;
    /* Why formatting stopped, when builder_format_value returns false. */
    DiamondVmStatus status;
} FormatContext;

typedef struct StringBuilder {
    char *chars;
    size_t length;
    size_t capacity;
    const DiamondObject *active[32];
    size_t active_count;
    FormatContext *format_context;
    /* inspect() rather than to_s(): Strings are quoted and escaped, Symbols
     * keep their colon, and an Instance without its own inspect shows its
     * fields, so `["a", "b"]` and `["a, b"]` no longer print alike. */
    bool inspect;
} StringBuilder;

/* Allocation and shared primitives that stay in vm.c (the garbage collector lives there). */
DIAMOND_INTERNAL DiamondString *allocate_string(DiamondVm *vm, const char *chars, size_t length);
DIAMOND_INTERNAL bool copy_value_into_vm(DiamondVm *dest_vm, DiamondValue value,
                                         DiamondProgram *source_program,
                                         const DiamondClass *rebase_source_classes,
                                         const DiamondClass *rebase_dest_classes,
                                         const DiamondChunk **adopted_owner,
                                         DiamondValue *out);
DIAMOND_INTERNAL bool methods_have_own_named(const DiamondMethod *methods, size_t count,
                                             const char *name);
DIAMOND_INTERNAL bool sandbox_category_allowed(const char *category);

/* vm_program_builder.c: the ProgramBuilder native bridge. */
DIAMOND_INTERNAL DiamondProgramBuilder *allocate_program_builder(DiamondVm *vm);
DIAMOND_INTERNAL DiamondVmStatus program_builder_run_helper(DiamondVm *vm,
        DiamondProgramBuilder *builder, DiamondValue *result);
DIAMOND_INTERNAL DiamondVmStatus program_builder_invoke_helper(DiamondVm *vm,
        DiamondProgramBuilder *builder, const DiamondStringConstant *method_name,
        DiamondValue *registers, uint16_t base, uint8_t argc, size_t depth,
        DiamondValue *result);

/* vm_tensor.c: entry points called from run_chunk */
DIAMOND_INTERNAL DiamondTensor *allocate_tensor(DiamondVm *vm,size_t rows,size_t cols);
DIAMOND_INTERNAL DiamondVmStatus tensor_dispatch_helper(DiamondVm *vm,DiamondTensor *tensor, const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base, uint8_t argc,uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus tensor_from_array_helper(DiamondVm *vm,const DiamondArray *outer, DiamondValue *out);
DIAMOND_INTERNAL void tensor_random_helper(DiamondTensor *tensor,int64_t seed);

/* Shared primitives that stay in vm.c (needed by tensor code) */
DIAMOND_INTERNAL DiamondArray *allocate_array(DiamondVm *vm,const DiamondValue *values, size_t count);
DIAMOND_INTERNAL bool array_push(DiamondVm *vm,DiamondArray *array,DiamondValue value);
DIAMOND_INTERNAL bool numeric_as_double(DiamondValue value, double *out);

/* vm_regexp.c: entry points called from run_chunk and the rest of vm.c */
DIAMOND_INTERNAL DiamondVmStatus regexp_match_helper(DiamondVm *vm, const DiamondRegexp *regexp, const DiamondString *subject, bool test_only, DiamondValue *registers, uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus regexp_new_helper(DiamondVm *vm, const DiamondString *pattern, int64_t options, DiamondValue *result);
DIAMOND_INTERNAL DiamondVmStatus regexp_replace_block_helper(DiamondVm *vm, const DiamondChunk *chunk,size_t depth,const DiamondRegexp *regexp, const DiamondString *subject,const DiamondClosure *block, bool replace_all,DiamondValue *result);
DIAMOND_INTERNAL DiamondVmStatus regexp_replace_helper(DiamondVm *vm,const DiamondRegexp *regexp, const DiamondString *subject,const DiamondString *replacement, bool replace_all,DiamondValue *result);
DIAMOND_INTERNAL DiamondVmStatus regexp_scan_helper(DiamondVm *vm,const DiamondRegexp *regexp, const DiamondString *subject,DiamondValue *registers,uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus regexp_split_helper(DiamondVm *vm,const DiamondRegexp *regexp, const DiamondString *subject,DiamondValue *registers,uint16_t dest);

/* Shared primitives that stay in vm.c (needed by vm_regexp.c) */
DIAMOND_INTERNAL bool byte_buffer_append(ByteBuffer *buffer,const char *bytes,size_t count);
DIAMOND_INTERNAL DiamondVmStatus call_closure_helper(DiamondVm *vm,const DiamondChunk *chunk, const DiamondFunction *fn,const DiamondClosure *called, const DiamondValue *registers,uint16_t base,uint8_t argc,size_t depth, DiamondValue *result);
DIAMOND_INTERNAL bool gc_protect(DiamondVm *vm, DiamondValue value);
DIAMOND_INTERNAL void gc_unprotect(DiamondVm *vm, size_t saved_count);
DIAMOND_INTERNAL DiamondVmStatus stringify_value(DiamondVm *vm,const DiamondChunk *chunk, size_t depth,DiamondValue value, DiamondValue *out);

/* vm_json.c: entry points called from run_chunk and the rest of vm.c */
DIAMOND_INTERNAL bool debug_json_append_escaped_string(GrowBuffer *buffer, const char *chars,size_t length);
DIAMOND_INTERNAL DiamondVmStatus json_parse_document(DiamondVm *vm,const char *source, size_t length,DiamondValue *out);
DIAMOND_INTERNAL DiamondVmStatus json_stringify_document(DiamondVm *vm,const DiamondChunk *chunk, size_t depth,DiamondValue value,DiamondValue *result);

/* Shared primitives that stay in vm.c (needed by vm_json.c) */
DIAMOND_INTERNAL DiamondHash *allocate_hash(DiamondVm *vm);
DIAMOND_INTERNAL bool builder_append(StringBuilder *builder,const char *chars,size_t length);
DIAMOND_INTERNAL bool builder_format_value(StringBuilder *builder,DiamondValue value);
DIAMOND_INTERNAL bool grow_buffer_append(GrowBuffer *buffer,const char *text,size_t text_length);
#define GROW_BUFFER_APPEND_LITERAL(buffer_,literal_) \
    grow_buffer_append((buffer_),(literal_),sizeof(literal_)-1)
DIAMOND_INTERNAL bool hash_set(DiamondVm *vm,DiamondHash *hash,DiamondValue key, DiamondValue value);
DIAMOND_INTERNAL bool value_is_bignum(DiamondValue value);

/* vm_time.c: entry points called from run_chunk and the rest of vm.c */
DIAMOND_INTERNAL DiamondVmStatus time_at_helper(DiamondVm *vm,DiamondValue epoch_value, DiamondValue *out_result);
DIAMOND_INTERNAL DiamondVmStatus time_build_helper(DiamondVm *vm,const DiamondValue *arguments, uint8_t mode,DiamondValue *out_result);
DIAMOND_INTERNAL bool time_comparison_fallback(DiamondValue left_value,DiamondValue right_value, DiamondOpCode opcode,DiamondValue *out_result);
DIAMOND_INTERNAL DiamondVmStatus time_dispatch_helper(DiamondVm *vm,DiamondTime *target, const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base, uint8_t argc,uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus time_now_helper(DiamondVm *vm,bool utc,DiamondValue *out_result);
DIAMOND_INTERNAL DiamondVmStatus time_parse_helper(DiamondVm *vm,DiamondValue input, DiamondValue *out_result);
DIAMOND_INTERNAL DiamondVmStatus time_relative_now_helper(DiamondVm *vm,DiamondValue duration_value, bool future,DiamondValue *out_result);
DIAMOND_INTERNAL bool time_struct_tm(const DiamondTime *target,struct tm *out);
DIAMOND_INTERNAL DiamondVmStatus time_subtract_fallback(DiamondVm *vm, DiamondValue left_value,DiamondValue right_value,DiamondValue *out_result);

/* Shared primitives that stay in vm.c (needed by vm_time.c) */
DIAMOND_INTERNAL DiamondTime *allocate_time(DiamondVm *vm,double epoch,uint8_t zone_mode, int32_t utc_offset);
DIAMOND_INTERNAL int days_in_calendar_month(int year,int month);
DIAMOND_INTERNAL bool format_time_default(const DiamondTime *target,StringBuilder *builder);
DIAMOND_INTERNAL bool format_time_iso8601(const DiamondTime *target,int precision, StringBuilder *builder);
DIAMOND_INTERNAL int parse_decimal_digits(const char *chars,size_t start,size_t count);
DIAMOND_INTERNAL bool parse_time_utc_offset(const DiamondString *string,int32_t *out_offset);
DIAMOND_INTERNAL bool parse_time_utc_offset_chars(const char *chars,size_t length, int32_t *out_offset);
DIAMOND_INTERNAL char *substitute_fixed_offset_z(const char *format,int64_t offset);
DIAMOND_INTERNAL int64_t weekday_calendar_distance(int wday,int64_t count,bool future);

#endif
