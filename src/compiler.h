#ifndef DIAMOND_COMPILER_H
#define DIAMOND_COMPILER_H

#include "lexer.h"
#include "loader.h"
#include "vm.h"

typedef struct DiamondProgram {
    DiamondFunction entry;
    /* Root chunk display name, kept separate from entry.name: that field
     * shares DiamondFunction's DIAMOND_MAX_FUNCTION_NAME (64-byte) buffer
     * with every ordinary function/class/field name, but a real source
     * path (as set by the ProgramBuilder#expand_source native bridge, see
     * src/vm.c) routinely exceeds that. diamond_program_chunk() prefers
     * this field over entry.name when it's set. Native's own run_source
     * (src/main.c) sidesteps the whole problem by overwriting chunk.name
     * with its own unbounded `name` pointer after compiling; the bridge
     * has no equivalent post-compile hook, so it needs a field to persist
     * the untruncated path across expand_source and the later run. */
    char entry_path[DIAMOND_MAX_SOURCE_PATH];
    DiamondFunction functions[DIAMOND_MAX_FUNCTIONS];
    size_t function_count;
    DiamondClass classes[DIAMOND_MAX_CLASSES];
    size_t class_count;
    DiamondInterface interfaces[DIAMOND_MAX_INTERFACES];
    size_t interface_count;
    DiamondModule modules[DIAMOND_MAX_MODULES];
    size_t module_count;
    char namespace_constants[DIAMOND_MAX_NAMESPACE_CONSTANTS]
                            [DIAMOND_MAX_FUNCTION_NAME];
    size_t namespace_constant_count;
} DiamondProgram;

typedef struct DiamondDiagnostic {
    DiamondSpan span;
    const char *message;
} DiamondDiagnostic;

/* Zeroes *program and populates the fixed built-in exception-class table
 * (Exception/StandardError/TypeError/.../RegexpError) with valid shapes,
 * so the result is immediately safe to run even before any user code is
 * compiled or emitted into it -- shared by diamond_compile and the
 * ProgramBuilder native bridge (src/vm.c), which needs the same baseline
 * without going through the parser at all. See docs/roadmap.md. */
void diamond_program_init(DiamondProgram *program);
bool diamond_compile(const char *source, DiamondProgram *program,
                     DiamondDiagnostic *diagnostic);
DiamondChunk diamond_program_chunk(const DiamondProgram *program);

#endif
