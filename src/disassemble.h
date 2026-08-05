#ifndef DIAMOND_DISASSEMBLE_H
#define DIAMOND_DISASSEMBLE_H

#include "vm.h"

#include <stdio.h>

bool diamond_disassemble(FILE *stream, const char *name,
                         const DiamondChunk *chunk);

#endif
