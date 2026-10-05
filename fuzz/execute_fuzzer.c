/* open_memstream (POSIX.1-2008) is hidden by glibc's stdio.h under a
 * strict -std=c23 with no feature-test macro set. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE

#include "compiler.h"
#include "disassemble.h"
#include "vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* Per-input execution budget.
 *
 * run_chunk has no execution-step budget by default on purpose -- real
 * Diamond programs legitimately run unbounded loops -- but this harness feeds
 * it raw, adversarial bytecode, and libFuzzer treats a genuine hang the same
 * as a crash (a single JUMP to its own offset caught it once).
 *
 * The VM's own DIAMOND_MAX_INSTRUCTIONS / DIAMOND_MAX_WALL_MILLISECONDS
 * budgets (docs/sandbox.md) stop a run cleanly: the run returns an error and
 * unwinds normally. An earlier version used a timer plus siglongjmp out of
 * the run; when the timer fired inside a garbage collection it left the
 * thread-local mark stack mid-drain, and the next input's collection marked
 * stale pointers (a heap-use-after-free found by this fuzzer, in the harness
 * and not in the runtime). Nothing may longjmp out of the VM. */
static bool references_unsafe_opcode(const DiamondChunk *chunk) {
    char *text = nullptr;
    size_t text_size = 0;
    FILE *sink = open_memstream(&text, &text_size);
    if (sink == nullptr) return true; /* fail closed: skip this input */
    diamond_disassemble(sink, chunk->name, chunk);
    fclose(sink);
    static const char *const unsafe_mnemonics[] = {
        "FILE_OPEN", "TCP_CONNECT", "TCP_LISTEN", "TCP_LISTEN_NONBLOCK",
        "IO_POLL", "DNS_RESOLVE", "UDP_BIND", "UDP_OPEN", "SIGNAL_TRAP", "TLS_CONNECT", "TLS_START_HANDSHAKE",
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

/* The input is more than code now. The first harness gave the entry function
 * bytes and nothing else, so every opcode that names a constant, string or
 * function was rejected by the verifier (there were none to name) and never
 * ran -- but ProgramBuilder#add_constant/#add_string/#declare_function hand a
 * script all three. Layout, every field optional (a short input just reads
 * zeros):
 *
 *   byte 0   bits 0-5 entry register_count - 1, bits 6-7 class count
 *   byte 1   bits 0-2 constant count, bits 3-5 string count, bits 6-7 function count
 *   then     each constant: 1 kind byte (Nil/Bool/Int/Float/Class) + 8 payload bytes
 *   then     each string: 1 length byte (mod 16) + that many bytes
 *   then     each function: arity (mod 4), register_count - 1 (mod 16), code length (mod 48), code
 *   then     each class: superclass (a byte past the classes so far means none), field count (mod 5),
 *            method count (mod 3), and per method a function index (mod function count) and arity (mod 4)
 *   rest     the entry function's code
 *
 * Every function, and the entry, gets its own copy of the same constants and
 * strings, so an index that is valid in one is valid in all. Class constants
 * are included on purpose: a script can put one there (`add_constant(f, self)`
 * inside a singleton method), and its index only means something to the chunk
 * it came from. */
typedef struct FuzzReader {
    const uint8_t *data;
    size_t size;
} FuzzReader;

static uint8_t fuzz_byte(FuzzReader *reader) {
    if (reader->size == 0) return 0;
    const uint8_t byte = *reader->data;
    reader->data++;
    reader->size--;
    return byte;
}

static bool fuzz_function_fill(DiamondFunction *function, const char *name, FuzzReader *reader,
        size_t code_length, uint16_t register_count, uint8_t arity,
        const DiamondValue *constants, size_t constant_count,
        const DiamondStringConstant *strings, size_t string_count) {
    if (name != function->name) (void)snprintf(function->name, sizeof function->name, "%s", name);
    function->arity = arity;
    function->required_arity = arity;
    function->register_count = register_count;
    if (code_length > 0) {
        if (!diamond_function_reserve_code(function, code_length)) return false;
        for (size_t index = 0; index < code_length; index++) function->code[index] = fuzz_byte(reader);
        function->code_count = code_length;
    }
    if (constant_count > 0) {
        if (!diamond_function_reserve_constants(function, constant_count)) return false;
        memcpy(function->constants, constants, constant_count * sizeof *constants);
        function->constant_count = constant_count;
    }
    if (string_count > 0) {
        if (!diamond_function_reserve_strings(function, string_count)) return false;
        memcpy(function->strings, strings, string_count * sizeof *strings);
        function->string_count = string_count;
    }
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    /* DiamondProgram is tens of MB (see compile_fuzzer.c's own comment
     * on the same struct) -- heap-allocated once and reused across the
     * whole run rather than malloc'd fresh on every iteration. */
    static DiamondProgram *program = nullptr;
    if (program == nullptr) {
        program = calloc(1, sizeof *program);
        if (program == nullptr) return 0;
    }
    diamond_program_free(program);
    diamond_program_init(program);

    FuzzReader reader = {.data = data, .size = size};
    const uint8_t first = fuzz_byte(&reader);
    const uint16_t register_count = (uint16_t)(1 + (first & 63u));
    const size_t class_total = (first >> 6) & 3u;
    const uint8_t counts = fuzz_byte(&reader);
    const size_t constant_count = counts & 7u;
    const size_t string_count = (counts >> 3) & 7u;
    const size_t function_count = (counts >> 6) & 3u;

    DiamondValue constants[8];
    for (size_t index = 0; index < constant_count; index++) {
        const uint8_t kind = fuzz_byte(&reader) % 5;
        uint8_t payload[8];
        for (size_t byte = 0; byte < sizeof payload; byte++) payload[byte] = fuzz_byte(&reader);
        int64_t integer;
        double real;
        memcpy(&integer, payload, sizeof integer);
        memcpy(&real, payload, sizeof real);
        switch (kind) {
            case 0: constants[index] = DIAMOND_NIL; break;
            case 1: constants[index] = DIAMOND_BOOL(payload[0] & 1u); break;
            case 2: constants[index] = DIAMOND_INT(integer); break;
            case 3: constants[index] = DIAMOND_FLOAT(real); break;
            default: constants[index] = DIAMOND_CLASS(payload[0]); break;
        }
    }
    DiamondStringConstant strings[8];
    for (size_t index = 0; index < string_count; index++) {
        const size_t length = fuzz_byte(&reader) % 16;
        memset(&strings[index], 0, sizeof strings[index]);
        for (size_t byte = 0; byte < length; byte++) strings[index].chars[byte] = (char)fuzz_byte(&reader);
        strings[index].length = length;
    }
    for (size_t index = 0; index < function_count; index++) {
        const uint8_t arity = fuzz_byte(&reader) % 4;
        const uint16_t function_registers = (uint16_t)(1 + fuzz_byte(&reader) % 16);
        const size_t code_length = fuzz_byte(&reader) % 48;
        DiamondFunction *function = diamond_program_add_function(program);
        char name[8];
        (void)snprintf(name, sizeof name, "f%zu", index);
        if (function == nullptr || !fuzz_function_fill(function, name, &reader, code_length,
                function_registers, arity, constants, constant_count, strings, string_count))
            return 0;
    }
    /* Classes, declared the way ProgramBuilder#declare_class / #declare_field /
     * #declare_method do, so NEW, GET_IVAR/SET_IVAR, INVOKE and super calls have
     * something to act on. A method points at one of the functions above; as
     * declare_method does, that function's owner_class is set to the class. */
    for (size_t index = 0; index < class_total; index++) {
        if (program->class_count >= DIAMOND_MAX_CLASSES) break;
        const uint8_t superclass_byte = fuzz_byte(&reader);
        const size_t field_total = fuzz_byte(&reader) % 5;
        const size_t method_total = fuzz_byte(&reader) % 3;
        const size_t class_index = program->class_count++;
        DiamondClass *class = &program->classes[class_index];
        *class = (DiamondClass){};
        (void)snprintf(class->name, sizeof class->name, "K%zu", index);
        class->superclass = UINT8_MAX;
        if (superclass_byte < class_index) {
            const DiamondClass *parent = &program->classes[superclass_byte];
            class->superclass = superclass_byte;
            class->field_count = parent->field_count;
            memcpy(class->fields, parent->fields, parent->field_count * sizeof class->fields[0]);
        }
        for (size_t field = 0; field < field_total && class->field_count < DIAMOND_MAX_FIELDS; field++)
            (void)snprintf(class->fields[class->field_count++], sizeof class->fields[0], "f%zu", field);
        for (size_t method = 0; method < method_total; method++) {
            const uint8_t function_byte = fuzz_byte(&reader);
            const uint8_t arity = fuzz_byte(&reader) % 4;
            if (function_count == 0 || class->method_count >= DIAMOND_MAX_METHODS) continue;
            const size_t function_index = function_byte % function_count;
            DiamondMethod *entry = &class->methods[class->method_count++];
            *entry = (DiamondMethod){};
            (void)snprintf(entry->name, sizeof entry->name, method == 0 ? "initialize" : "m%zu", method);
            entry->function_index = (uint16_t)function_index;
            entry->arity = arity;
            entry->required_arity = arity;
            program->functions[function_index]->owner_class = (uint8_t)class_index;
        }
        for (size_t field_count = 0; field_count <= class->field_count; field_count++)
            class->shapes[field_count] = (DiamondShape){.class = class, .field_count = (uint8_t)field_count};
    }
    /* `entry.code` is `uint8_t *`, not an inline array -- diamond_program_
     * init's memset leaves it null, same as every other DiamondProgram
     * field a real compile pass would malloc into. Never caught until a
     * real Ubuntu 26.04 + Clang test-all run finally got far enough to run
     * this harness (see CHANGELOG.md). */
    size_t entry_length = reader.size;
    if (entry_length > DIAMOND_MAX_CODE) entry_length = DIAMOND_MAX_CODE;
    if (!fuzz_function_fill(&program->entry, program->entry.name, &reader, entry_length,
            register_count, 0, constants, constant_count, strings, string_count))
        return 0;

    const DiamondChunk chunk = diamond_program_chunk(program);
    if (!diamond_verify_bytecode(&chunk)) return 0;
    if (references_unsafe_opcode(&chunk)) return 0;

    /* Read by diamond_vm_init, so set before it. */
    setenv("DIAMOND_MAX_INSTRUCTIONS", "2000000", 1);
    setenv("DIAMOND_MAX_WALL_MILLISECONDS", "2000", 1);
    DiamondVm vm;
    diamond_vm_init(&vm);
    DiamondValue result = DIAMOND_NIL;
    (void)diamond_vm_run(&vm, &chunk, &result);
    diamond_vm_free(&vm);
    return 0;
}
