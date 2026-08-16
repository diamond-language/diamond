#ifndef DIAMOND_DISASSEMBLE_H
#define DIAMOND_DISASSEMBLE_H

#include "vm.h"

#include <stdio.h>

bool diamond_disassemble(FILE *stream, const char *name,
                         const DiamondChunk *chunk);

/* Structurally validates every function in `chunk` (the top-level chunk
 * and every entry in chunk->functions) the same way diamond_disassemble
 * does -- register operands in range, jump/PUSH_RESCUE/PUSH_ENSURE/
 * RUN_ENSURE targets landing on a real instruction boundary, constant/
 * string/function/type-set indices in range -- without printing
 * anything. Ordinary compiler-emitted bytecode always passes this by
 * construction; it exists for ProgramBuilder#run (src/vm.c), which lets
 * Diamond code hand-assemble raw bytecode via #emit_byte with no
 * understanding of what instruction it's building, bypassing every
 * guarantee the compiler's own register allocator normally provides. */
bool diamond_verify_bytecode(const DiamondChunk *chunk);

/* Writes a type set's display form (`Int`, `String | Nil`, a class/
 * interface name, or a generic/`Callable` shape with its own nested
 * type sets in brackets) to `stream` -- the exact formatting this
 * disassembler already uses inline for CHECK_TYPE-style operands,
 * exposed here so a second consumer (lsp/hover.c, reconstructing a
 * function's declared signature) doesn't need its own copy of type-set
 * formatting logic. Returns false (having still written a best-effort
 * `<invalid ...>` placeholder) for a set index or member this chunk
 * doesn't actually have, same as every other disassembly helper here. */
bool diamond_print_type_set(FILE *stream, const DiamondChunk *chunk, uint8_t set_index);

#endif
