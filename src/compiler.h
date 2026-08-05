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
} DiamondProgram;

typedef struct DiamondDiagnostic {
    DiamondSpan span;
    const char *message;
} DiamondDiagnostic;

bool diamond_compile(const char *source, DiamondProgram *program,
                     DiamondDiagnostic *diagnostic);
DiamondChunk diamond_program_chunk(const DiamondProgram *program);

#endif
