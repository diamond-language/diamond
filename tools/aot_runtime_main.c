/* The `main()` linked into every `diamond build`-produced standalone
 * binary (see src/main.c's own "build" subcommand). Deliberately not
 * under src/: like tools/gen_compiled_prelude.c, this is a build-time-
 * only artifact, never part of the ordinary `diamond` binary itself.
 *
 * Reads the already-compiled DiamondProgram a generated embed-data file
 * provides (diamond_aot_program_data/_size, following src/compiled_
 * prelude_data.c's own #embed-plus-accessor-function shape exactly --
 * see main.c's own "build" handler for how that file is generated per
 * invocation) via diamond_program_read_compiled, then runs it the same
 * way run_source.c's own run_compiled_chunk does for the ordinary CLI:
 * diamond_vm_set_argv for ARGV, diamond_vm_run, diamond_value_print the
 * top-level result on success (confirmed directly against the real CLI:
 * `echo '"hello"' > f.di; diamond f.di` prints "hello" with no explicit
 * puts -- diamond_value_print is what does that). No DIAMOND_TRACE_
 * (name) or DIAMOND_STRESS_GC-style dev knobs here: those are for
 * developing Diamond itself, not for a program someone else's
 * `diamond build` shipped as a finished binary. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE

#include "compiled_prelude.h"
#include "compiler.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>

const uint8_t *diamond_aot_program_data(void);
size_t diamond_aot_program_size(void);

int main(int argc, char **argv) {
    DiamondProgram *program = calloc(1, sizeof *program);
    if (program == nullptr) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 74;
    }
    diamond_program_init_fresh(program);
    if (!diamond_program_read_compiled(diamond_aot_program_data(),
            diamond_aot_program_size(), program)) {
        fprintf(stderr, "%s: internal error: corrupt embedded program\n", argv[0]);
        return 74;
    }

    DiamondVm vm;
    diamond_vm_init(&vm);
    diamond_vm_set_argv(&vm, argc > 1 ? argc - 1 : 0, argc > 1 ? argv + 1 : nullptr);
    const DiamondChunk chunk = diamond_program_chunk(program);
    DiamondValue result = DIAMOND_NIL;
    const DiamondVmStatus status = diamond_vm_run(&vm, &chunk, &result);
    if (status != DIAMOND_VM_OK) {
        fprintf(stderr, "%s: %s\n", argv[0],
            vm.error[0] != '\0' ? vm.error : diamond_vm_status_name(status));
        diamond_vm_free(&vm);
        diamond_program_free(program);
        free(program);
        return 70;
    }
    diamond_value_print(result);
    putchar('\n');
    diamond_vm_free(&vm);
    diamond_program_free(program);
    free(program);
    return 0;
}
