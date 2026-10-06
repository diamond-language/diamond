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

#define DIAMOND_INTERNAL __attribute__((visibility("hidden")))

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

#endif
