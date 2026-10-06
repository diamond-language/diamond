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

#endif
