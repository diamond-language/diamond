/* Build-time-only generator: compiles the prelude the ordinary way (with
 * JSON, so the embedded template below can serve every program except
 * the rare one that itself needs the discovery pass to see `JSON`/
 * `JSONCodec`/`JSONError` as *undeclared* names -- diamond_run_source
 * (src/run_source.c) still falls back to a live diamond_compile of a
 * fresh, JSON-aware-or-not prelude for that one case, exactly as it
 * always has), then writes diamond_program_write_compiled's own binary
 * form to the path given as argv[1]. Never linked into the `diamond`
 * binary itself -- see src/compiled_prelude.h's own top comment. Run
 * once per `make` invocation (see Makefile's own $(BUILD_DIR)/
 * compiled_prelude.bin rule); its output is #embed'd by
 * src/compiled_prelude_data.c. */

/* See src/bignum.c's own identical comment: needed transitively for
 * vm.h's <ucontext.h> use (via compiler.h), only under musl
 * (docs/roadmap.md's "Portability"). */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "compiled_prelude.h"
#include "compiler.h"
#include "prelude.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT_PATH\n", argv[0]);
        return 1;
    }

    const size_t prelude_length = diamond_prelude_length(true);
    char *prelude_source = malloc(prelude_length + 1);
    if (prelude_source == nullptr) {
        fprintf(stderr, "gen_compiled_prelude: out of memory\n");
        return 1;
    }
    diamond_prelude_write(prelude_source, true);
    prelude_source[prelude_length] = '\0';

    DiamondProgram *program = calloc(1, sizeof *program);
    if (program == nullptr) {
        fprintf(stderr, "gen_compiled_prelude: out of memory\n");
        return 1;
    }
    DiamondDiagnostic diagnostic;
    if (!diamond_compile(prelude_source, program, &diagnostic)) {
        fprintf(stderr, "gen_compiled_prelude: prelude compile failed: %s\n",
            diagnostic.message);
        return 1;
    }
    free(prelude_source);

    FILE *file = fopen(argv[1], "wb");
    if (file == nullptr) {
        fprintf(stderr, "gen_compiled_prelude: cannot open '%s' for writing\n", argv[1]);
        return 1;
    }
    if (!diamond_program_write_compiled(program, file)) {
        fprintf(stderr, "gen_compiled_prelude: write_compiled failed\n");
        fclose(file);
        return 1;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "gen_compiled_prelude: error closing '%s'\n", argv[1]);
        return 1;
    }

    diamond_program_free(program);
    free(program);
    return 0;
}
