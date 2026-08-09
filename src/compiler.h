#ifndef DIAMOND_COMPILER_H
#define DIAMOND_COMPILER_H

#include "lexer.h"
#include "vm.h"

typedef struct DiamondProgram {
    DiamondFunction entry;
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
