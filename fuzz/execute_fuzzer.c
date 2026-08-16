/* open_memstream (POSIX.1-2008) is hidden by glibc's stdio.h under a
 * strict -std=c23 with no feature-test macro set. */
#define _POSIX_C_SOURCE 200809L

#include "compiler.h"
#include "disassemble.h"
#include "vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fuzzes run_chunk (via diamond_vm_run) directly against a synthetic,
 * minimal DiamondChunk built straight from raw fuzzer bytes -- the exact
 * surface ProgramBuilder-constructed bytecode exposes to run_chunk, and
 * a gap compile_fuzzer.c structurally can't cover since it only ever
 * calls diamond_compile, never diamond_vm_run (see docs/fuzzing.md).
 * This input is raw bytecode bytes, not Diamond source text -- a
 * complement to compile_fuzzer, not a replacement.
 *
 * The register-bounds-out-of-range bug the pre-release audit found
 * (ProgramBuilder#run executing a hand-assembled MOVE/CALL/etc with a
 * destination register past the frame's own register_count -- see
 * diamond_verify_bytecode's own doc comment, src/disassemble.h) is
 * exactly the class of bug this harness is built to catch: it exercises
 * the identical trust boundary ProgramBuilder#run does, minus the
 * Diamond-level #emit_byte call overhead of getting there.
 *
 * Still deliberately never touches real I/O, for the same reason
 * compile_fuzzer.c stays compile-only (see its own doc comment and
 * docs/fuzzing.md's "Why compile-only, never execute"): a fuzzer-
 * mutated program that opens/writes/deletes real files, holds open
 * sockets, or spawns threads isn't safe to run unattended without
 * sandboxing/resource limits neither harness attempts. Any chunk whose
 * disassembly mentions an I/O-, thread-, or signal-capable opcode is
 * rejected before diamond_vm_run ever sees it -- reusing
 * diamond_disassemble's own proven-correct per-instruction walk via
 * open_memstream rather than duplicating its opcode/operand-width
 * knowledge in a second decoder here.
 *
 * register_count and code come from the fuzzer's own input bytes (the
 * first byte picks register_count, 1..64; the rest is the code array,
 * truncated to DIAMOND_MAX_CODE) so libFuzzer's coverage-guided mutation
 * can explore both dimensions of what made the original bug reachable:
 * a small declared register_count together with an operand that
 * overruns it. */

static bool references_unsafe_opcode(const DiamondChunk *chunk) {
    char *text = nullptr;
    size_t text_size = 0;
    FILE *sink = open_memstream(&text, &text_size);
    if (sink == nullptr) return true; /* fail closed: skip this input */
    diamond_disassemble(sink, chunk->name, chunk);
    fclose(sink);
    static const char *const unsafe_mnemonics[] = {
        "FILE_OPEN", "TCP_CONNECT", "TCP_LISTEN", "TCP_LISTEN_NONBLOCK",
        "IO_POLL", "UDP_BIND", "UDP_OPEN", "SIGNAL_TRAP", "TLS_CONNECT",
        "TLS_LISTEN", "THREAD_NEW",
    };
    bool found = false;
    for (size_t index = 0;
         index < sizeof(unsafe_mnemonics) / sizeof(unsafe_mnemonics[0]);
         index++) {
        if (strstr(text, unsafe_mnemonics[index]) != nullptr) {
            found = true;
            break;
        }
    }
    free(text);
    return found;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    /* DiamondProgram is tens of MB (see compile_fuzzer.c's own comment
     * on the same struct) -- heap-allocated once and reused across the
     * whole run rather than malloc'd fresh on every iteration. */
    static DiamondProgram *program = nullptr;
    if (program == nullptr) {
        program = malloc(sizeof *program);
        if (program == nullptr) return 0;
    }
    diamond_program_init(program);

    const uint16_t register_count = (uint16_t)(1 + (data[0] % 64));
    data++;
    size--;
    if (size > DIAMOND_MAX_CODE) size = DIAMOND_MAX_CODE;
    memcpy(program->entry.code, data, size);
    program->entry.code_count = size;
    program->entry.register_count = register_count;

    const DiamondChunk chunk = diamond_program_chunk(program);
    if (!diamond_verify_bytecode(&chunk)) return 0;
    if (references_unsafe_opcode(&chunk)) return 0;

    DiamondVm vm;
    diamond_vm_init(&vm);
    DiamondValue result = DIAMOND_NIL;
    (void)diamond_vm_run(&vm, &chunk, &result);
    diamond_vm_free(&vm);
    return 0;
}
