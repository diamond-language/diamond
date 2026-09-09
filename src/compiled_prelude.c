/* Serializes/deserializes an already-compiled DiamondProgram as a flat,
 * same-build-only binary blob -- not a portable format (no versioning,
 * no endianness handling, struct layouts dumped exactly as this build's
 * own compiler laid them out), the same way an object file isn't
 * portable across a different compiler/architecture either. Generated
 * by tools/gen_compiled_prelude.c (a build-time-only tool, never linked
 * into `diamond` itself) and consumed by src/compiled_prelude_data.c's
 * `#embed`ded bytes, both compiled by the exact same `make` invocation
 * -- there is no cross-version compatibility question to answer here,
 * only "does this build's own writer and this build's own reader agree,"
 * which is automatic by construction.
 *
 * Why a raw dump of DiamondFunction/DiamondClass/DiamondInterface/
 * DiamondModule is safe at all, despite those structs containing
 * pointers: every dynamic array pointer (DiamondFunction.code/lines/
 * columns/constants/strings/type_sets) is written and read back as its
 * own explicit, length-prefixed byte range right after the owning
 * struct -- diamond_program_read_compiled reconstructs each one via
 * diamond_function_copy (the same deep-copy clone_program_from_chunk,
 * src/vm.c, already uses for Thread.new's own cross-heap program clone),
 * never by trusting a raw pointer value read from the file.
 *
 * classes[]/interfaces[]/modules[] are each written field-by-field
 * (write_class/write_module/write_interface below) rather than as a
 * single fixed-size struct dump: DiamondClass/DiamondModule's own
 * methods[DIAMOND_MAX_METHODS=256]/singleton_methods[256] arrays (and
 * DiamondInterface's own methods[256]) are sized for a program far
 * larger than the prelude ever populates, so writing only the actual
 * method_count/singleton_method_count entries (same "count before the
 * variable-length tail" shape as write_function's own dynamic arrays
 * above) is what keeps this format from ballooning back out to
 * something close to raw sizeof(DiamondProgram). DiamondMethod itself
 * has two pointers (source_chunk/bound_values),
 * but per its own comment (src/vm.h) both are non-null *only* for a
 * method installed at runtime via ClassName.compile_method/
 * .define_method, which the prelude's own source never does to itself
 * -- checked directly below (assert_methods_are_plain), not assumed.
 * DiamondClass.shapes[].class (a self-referential pointer) needs no
 * handling at all: run_compile_pass's own tail (src/compiler.c)
 * unconditionally recomputes shapes for *every* class in
 * `program->classes[0..class_count)` at the end of every single
 * diamond_compile/diamond_compile_incremental call, template-seeded
 * classes included, so whatever garbage this write/read round-trip
 * leaves in a deserialized class's own shapes[] is always overwritten
 * before anything ever reads it. */

/* See bignum.c's own identical comment: needed transitively for vm.h's
 * <ucontext.h> use (via compiler.h), only under musl (docs/roadmap.md's
 * "Portability"). */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "compiled_prelude.h"

#include <assert.h>
#include <string.h>

static bool write_all(FILE *file, const void *data, size_t size) {
    return size == 0 || fwrite(data, 1, size, file) == size;
}

static bool write_function(FILE *file, const DiamondFunction *function) {
    if (!write_all(file, function, sizeof *function)) return false;
    if (!write_all(file, function->code, function->code_count * sizeof *function->code)) return false;
    if (!write_all(file, function->lines, function->code_count * sizeof *function->lines)) return false;
    if (!write_all(file, function->columns, function->code_count * sizeof *function->columns)) return false;
    if (!write_all(file, function->constants, function->constant_count * sizeof *function->constants)) return false;
    if (!write_all(file, function->strings, function->string_count * sizeof *function->strings)) return false;
    if (!write_all(file, function->type_sets, function->type_set_count * sizeof *function->type_sets)) return false;
    return true;
}

/* See this file's own top comment on exactly why this must always hold
 * for a program compiled from plain source (never through
 * ClassName.compile_method/.define_method) -- an assertion, not
 * something the format works around, since a program where it doesn't
 * hold isn't safe to dump this way at all. */
static void assert_methods_are_plain(const DiamondMethod *methods, size_t count) {
    for (size_t index = 0; index < count; index++) {
        assert(methods[index].source_chunk == nullptr);
        assert(methods[index].bound_values == nullptr);
        assert(methods[index].bound_value_count == 0);
    }
    (void)methods; (void)count; /* silence unused-parameter in an NDEBUG build */
}

/* Writes only the *used* prefix of a class's own methods[]/
 * singleton_methods[]/fields[]/class_variables[] (each up to
 * DIAMOND_MAX_METHODS=256 or DIAMOND_MAX_FIELDS=64 slots, almost all
 * unused for any one class) -- see this file's own top comment. shapes[]
 * is never written: always safely recomputed on the read side before
 * anything reads it. */
static bool write_class(FILE *file, const DiamondClass *class) {
    if (!write_all(file, class->name, sizeof class->name)) return false;
    if (!write_all(file, &class->declaration_line, sizeof class->declaration_line)) return false;
    if (!write_all(file, &class->declaration_column, sizeof class->declaration_column)) return false;
    if (!write_all(file, &class->declaration_start, sizeof class->declaration_start)) return false;
    if (!write_all(file, &class->superclass, sizeof class->superclass)) return false;

    const uint64_t method_count = (uint64_t)class->method_count;
    const uint64_t singleton_method_count = (uint64_t)class->singleton_method_count;
    if (!write_all(file, &method_count, sizeof method_count)) return false;
    if (!write_all(file, &singleton_method_count, sizeof singleton_method_count)) return false;
    if (!write_all(file, class->methods, (size_t)method_count * sizeof class->methods[0])) return false;
    if (!write_all(file, class->singleton_methods,
            (size_t)singleton_method_count * sizeof class->singleton_methods[0])) return false;

    const uint64_t field_count = (uint64_t)class->field_count;
    if (!write_all(file, &field_count, sizeof field_count)) return false;
    if (!write_all(file, class->fields, (size_t)field_count * sizeof class->fields[0])) return false;
    if (!write_all(file, class->field_type_status, (size_t)field_count * sizeof class->field_type_status[0])) return false;
    if (!write_all(file, class->field_known_class, (size_t)field_count * sizeof class->field_known_class[0])) return false;

    const uint64_t class_variable_count = (uint64_t)class->class_variable_count;
    if (!write_all(file, &class_variable_count, sizeof class_variable_count)) return false;
    if (!write_all(file, class->class_variables,
            (size_t)class_variable_count * sizeof class->class_variables[0])) return false;

    if (!write_all(file, &class->declared_by_discovery, sizeof class->declared_by_discovery)) return false;
    return true;
}

/* Same trimming as write_class, for DiamondModule's own methods[]/
 * singleton_methods[]/fields[]. */
static bool write_module(FILE *file, const DiamondModule *module) {
    if (!write_all(file, module->name, sizeof module->name)) return false;
    if (!write_all(file, &module->declaration_line, sizeof module->declaration_line)) return false;
    if (!write_all(file, &module->declaration_column, sizeof module->declaration_column)) return false;
    if (!write_all(file, &module->declaration_start, sizeof module->declaration_start)) return false;

    const uint64_t method_count = (uint64_t)module->method_count;
    const uint64_t singleton_method_count = (uint64_t)module->singleton_method_count;
    if (!write_all(file, &method_count, sizeof method_count)) return false;
    if (!write_all(file, &singleton_method_count, sizeof singleton_method_count)) return false;
    if (!write_all(file, module->methods, (size_t)method_count * sizeof module->methods[0])) return false;
    if (!write_all(file, module->singleton_methods,
            (size_t)singleton_method_count * sizeof module->singleton_methods[0])) return false;

    if (!write_all(file, &module->next_singleton_claim, sizeof module->next_singleton_claim)) return false;

    const uint64_t field_count = (uint64_t)module->field_count;
    if (!write_all(file, &field_count, sizeof field_count)) return false;
    if (!write_all(file, module->fields, (size_t)field_count * sizeof module->fields[0])) return false;

    if (!write_all(file, &module->declared_by_discovery, sizeof module->declared_by_discovery)) return false;
    return true;
}

/* Same trimming as write_class, for DiamondInterface's own methods[].
 * type_sets is a pointer -- never written, fixed up on the read side
 * exactly like diamond_program_read_compiled's own tail already does
 * for a whole-array dump. */
static bool write_interface(FILE *file, const DiamondInterface *interface) {
    if (!write_all(file, interface->name, sizeof interface->name)) return false;
    if (!write_all(file, &interface->declaration_line, sizeof interface->declaration_line)) return false;
    if (!write_all(file, &interface->declaration_column, sizeof interface->declaration_column)) return false;
    if (!write_all(file, &interface->declaration_start, sizeof interface->declaration_start)) return false;

    const uint64_t method_count = (uint64_t)interface->method_count;
    if (!write_all(file, &method_count, sizeof method_count)) return false;
    if (!write_all(file, interface->methods, (size_t)method_count * sizeof interface->methods[0])) return false;

    if (!write_all(file, &interface->declared_by_discovery, sizeof interface->declared_by_discovery)) return false;
    return true;
}

bool diamond_program_write_compiled(const DiamondProgram *program, FILE *file) {
    if (!write_function(file, &program->entry)) return false;

    const uint64_t function_count = (uint64_t)program->function_count;
    if (!write_all(file, &function_count, sizeof function_count)) return false;
    for (size_t index = 0; index < program->function_count; index++)
        if (!write_function(file, program->functions[index])) return false;

    for (size_t index = 0; index < program->class_count; index++) {
        assert_methods_are_plain(program->classes[index].methods,
            program->classes[index].method_count);
        assert_methods_are_plain(program->classes[index].singleton_methods,
            program->classes[index].singleton_method_count);
    }
    for (size_t index = 0; index < program->module_count; index++) {
        assert_methods_are_plain(program->modules[index].methods,
            program->modules[index].method_count);
        assert_methods_are_plain(program->modules[index].singleton_methods,
            program->modules[index].singleton_method_count);
    }

    /* Counts before arrays, not after: only the *used* prefix of each
     * fixed-size table is written (class_count entries out of
     * DIAMOND_MAX_CLASSES=180, not all 180 -- the prelude only ever
     * populates ~23), so the reader needs each count in hand before it
     * knows how many bytes to expect. The unused suffix doesn't need
     * writing at all: diamond_program_init/calloc already zero it on
     * the reading side, exactly matching what an unused slot already
     * looks like in a freshly compiled program. Shrinks the serialized
     * form from a full ~14MB DiamondProgram-shaped dump (dominated by
     * 180-slot method tables sized for a program far larger than the
     * prelude) down to roughly what's actually declared. */
    const uint64_t class_count = (uint64_t)program->class_count;
    const uint64_t interface_count = (uint64_t)program->interface_count;
    const uint64_t module_count = (uint64_t)program->module_count;
    const uint64_t namespace_constant_count = (uint64_t)program->namespace_constant_count;
    if (!write_all(file, &class_count, sizeof class_count)) return false;
    if (!write_all(file, &interface_count, sizeof interface_count)) return false;
    if (!write_all(file, &module_count, sizeof module_count)) return false;
    if (!write_all(file, &namespace_constant_count, sizeof namespace_constant_count)) return false;

    for (size_t index = 0; index < program->class_count; index++)
        if (!write_class(file, &program->classes[index])) return false;
    for (size_t index = 0; index < program->interface_count; index++)
        if (!write_interface(file, &program->interfaces[index])) return false;
    for (size_t index = 0; index < program->module_count; index++)
        if (!write_module(file, &program->modules[index])) return false;
    if (!write_all(file, program->namespace_constants,
            program->namespace_constant_count * sizeof program->namespace_constants[0])) return false;
    if (!write_all(file, &program->range_class_index, sizeof program->range_class_index)) return false;

    return true;
}

/* Advances *cursor past `size` bytes and returns a pointer to where they
 * started, or nullptr if `size` bytes aren't actually left in [*cursor,
 * end) -- every call site below treats that as a malformed-buffer
 * failure, never a crash, even though a buffer this project's own build
 * just generated should never actually be short. */
static const uint8_t *take(const uint8_t **cursor, const uint8_t *end, size_t size) {
    if (size > (size_t)(end - *cursor)) return nullptr;
    const uint8_t *start = *cursor;
    *cursor += size;
    return start;
}

/* Reconstructs one real, independently heap-owned DiamondFunction into
 * `destination` by building a temporary, non-owning "view"
 * DiamondFunction whose pointer fields point directly into `*cursor`
 * (valid only for the duration of this call), then deep-copying it via
 * diamond_function_copy -- the exact same clone clone_program_from_chunk
 * (src/vm.c) already trusts for this. */
static bool read_function(const uint8_t **cursor, const uint8_t *end,
        DiamondFunction *destination) {
    const uint8_t *raw = take(cursor, end, sizeof(DiamondFunction));
    if (raw == nullptr) return false;
    DiamondFunction view;
    memcpy(&view, raw, sizeof view);
    const uint8_t *code = take(cursor, end, view.code_count * sizeof *view.code);
    const uint8_t *lines_bytes = take(cursor, end, view.code_count * sizeof *view.lines);
    const uint8_t *columns_bytes = take(cursor, end, view.code_count * sizeof *view.columns);
    const uint8_t *constants_bytes = take(cursor, end, view.constant_count * sizeof *view.constants);
    const uint8_t *strings_bytes = take(cursor, end, view.string_count * sizeof *view.strings);
    const uint8_t *type_sets_bytes = take(cursor, end, view.type_set_count * sizeof *view.type_sets);
    if ((view.code_count > 0 && (code == nullptr || lines_bytes == nullptr || columns_bytes == nullptr)) ||
        (view.constant_count > 0 && constants_bytes == nullptr) ||
        (view.string_count > 0 && strings_bytes == nullptr) ||
        (view.type_set_count > 0 && type_sets_bytes == nullptr))
        return false;
    view.code = (uint8_t *)code;
    view.lines = (uint32_t *)(const void *)lines_bytes;
    view.columns = (uint32_t *)(const void *)columns_bytes;
    view.constants = (DiamondValue *)(const void *)constants_bytes;
    view.strings = (DiamondStringConstant *)(const void *)strings_bytes;
    view.type_sets = (DiamondTypeSet *)(const void *)type_sets_bytes;
    return diamond_function_copy(destination, &view);
}

static bool read_u64(const uint8_t **cursor, const uint8_t *end, uint64_t *out) {
    const uint8_t *raw = take(cursor, end, sizeof *out);
    if (raw == nullptr) return false;
    memcpy(out, raw, sizeof *out);
    return true;
}

/* Bounds-checked cursor read of exactly `size` bytes into `dest` --
 * every read_class/read_module/read_interface field below goes through
 * this instead of repeating take()+memcpy() by hand. */
static bool read_bytes(const uint8_t **cursor, const uint8_t *end, void *dest, size_t size) {
    const uint8_t *raw = take(cursor, end, size);
    if (raw == nullptr) return false;
    if (size > 0) memcpy(dest, raw, size);
    return true;
}

/* Inverse of write_class: rejects an oversized method_count/field_count/
 * class_variable_count up front (this build's own DIAMOND_MAX_METHODS/
 * DIAMOND_MAX_FIELDS budget) rather than overflowing `out`'s own
 * fixed-size arrays -- same defensive posture as
 * diamond_program_read_compiled's own class_count/interface_count/
 * module_count checks below. */
static bool read_class(const uint8_t **cursor, const uint8_t *end, DiamondClass *out) {
    memset(out, 0, sizeof *out);
    if (!read_bytes(cursor, end, out->name, sizeof out->name)) return false;
    if (!read_bytes(cursor, end, &out->declaration_line, sizeof out->declaration_line)) return false;
    if (!read_bytes(cursor, end, &out->declaration_column, sizeof out->declaration_column)) return false;
    if (!read_bytes(cursor, end, &out->declaration_start, sizeof out->declaration_start)) return false;
    if (!read_bytes(cursor, end, &out->superclass, sizeof out->superclass)) return false;

    uint64_t method_count = 0, singleton_method_count = 0;
    if (!read_u64(cursor, end, &method_count)) return false;
    if (!read_u64(cursor, end, &singleton_method_count)) return false;
    if (method_count > DIAMOND_MAX_METHODS || singleton_method_count > DIAMOND_MAX_METHODS) return false;
    out->method_count = (size_t)method_count;
    out->singleton_method_count = (size_t)singleton_method_count;
    if (!read_bytes(cursor, end, out->methods, (size_t)method_count * sizeof out->methods[0])) return false;
    if (!read_bytes(cursor, end, out->singleton_methods,
            (size_t)singleton_method_count * sizeof out->singleton_methods[0])) return false;

    uint64_t field_count = 0;
    if (!read_u64(cursor, end, &field_count)) return false;
    if (field_count > DIAMOND_MAX_FIELDS) return false;
    out->field_count = (size_t)field_count;
    if (!read_bytes(cursor, end, out->fields, (size_t)field_count * sizeof out->fields[0])) return false;
    if (!read_bytes(cursor, end, out->field_type_status, (size_t)field_count * sizeof out->field_type_status[0])) return false;
    if (!read_bytes(cursor, end, out->field_known_class, (size_t)field_count * sizeof out->field_known_class[0])) return false;

    uint64_t class_variable_count = 0;
    if (!read_u64(cursor, end, &class_variable_count)) return false;
    if (class_variable_count > DIAMOND_MAX_FIELDS) return false;
    out->class_variable_count = (size_t)class_variable_count;
    if (!read_bytes(cursor, end, out->class_variables,
            (size_t)class_variable_count * sizeof out->class_variables[0])) return false;

    if (!read_bytes(cursor, end, &out->declared_by_discovery, sizeof out->declared_by_discovery)) return false;
    return true;
}

static bool read_module(const uint8_t **cursor, const uint8_t *end, DiamondModule *out) {
    memset(out, 0, sizeof *out);
    if (!read_bytes(cursor, end, out->name, sizeof out->name)) return false;
    if (!read_bytes(cursor, end, &out->declaration_line, sizeof out->declaration_line)) return false;
    if (!read_bytes(cursor, end, &out->declaration_column, sizeof out->declaration_column)) return false;
    if (!read_bytes(cursor, end, &out->declaration_start, sizeof out->declaration_start)) return false;

    uint64_t method_count = 0, singleton_method_count = 0;
    if (!read_u64(cursor, end, &method_count)) return false;
    if (!read_u64(cursor, end, &singleton_method_count)) return false;
    if (method_count > DIAMOND_MAX_METHODS || singleton_method_count > DIAMOND_MAX_METHODS) return false;
    out->method_count = (size_t)method_count;
    out->singleton_method_count = (size_t)singleton_method_count;
    if (!read_bytes(cursor, end, out->methods, (size_t)method_count * sizeof out->methods[0])) return false;
    if (!read_bytes(cursor, end, out->singleton_methods,
            (size_t)singleton_method_count * sizeof out->singleton_methods[0])) return false;

    if (!read_bytes(cursor, end, &out->next_singleton_claim, sizeof out->next_singleton_claim)) return false;

    uint64_t field_count = 0;
    if (!read_u64(cursor, end, &field_count)) return false;
    if (field_count > DIAMOND_MAX_FIELDS) return false;
    out->field_count = (size_t)field_count;
    if (!read_bytes(cursor, end, out->fields, (size_t)field_count * sizeof out->fields[0])) return false;

    if (!read_bytes(cursor, end, &out->declared_by_discovery, sizeof out->declared_by_discovery)) return false;
    return true;
}

/* type_sets is left nullptr here -- diamond_program_read_compiled's own
 * tail fixes it up for every interface afterward, same as it always has. */
static bool read_interface(const uint8_t **cursor, const uint8_t *end, DiamondInterface *out) {
    memset(out, 0, sizeof *out);
    if (!read_bytes(cursor, end, out->name, sizeof out->name)) return false;
    if (!read_bytes(cursor, end, &out->declaration_line, sizeof out->declaration_line)) return false;
    if (!read_bytes(cursor, end, &out->declaration_column, sizeof out->declaration_column)) return false;
    if (!read_bytes(cursor, end, &out->declaration_start, sizeof out->declaration_start)) return false;

    uint64_t method_count = 0;
    if (!read_u64(cursor, end, &method_count)) return false;
    if (method_count > DIAMOND_MAX_METHODS) return false;
    out->method_count = (size_t)method_count;
    if (!read_bytes(cursor, end, out->methods, (size_t)method_count * sizeof out->methods[0])) return false;

    if (!read_bytes(cursor, end, &out->declared_by_discovery, sizeof out->declared_by_discovery)) return false;
    return true;
}

bool diamond_program_read_compiled(const uint8_t *data, size_t size, DiamondProgram *out) {
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;

    if (!read_function(&cursor, end, &out->entry)) return false;

    uint64_t function_count = 0;
    if (!read_u64(&cursor, end, &function_count)) return false;
    for (uint64_t index = 0; index < function_count; index++) {
        DiamondFunction *slot = diamond_program_add_function(out);
        if (slot == nullptr) return false;
        if (!read_function(&cursor, end, slot)) return false;
    }

    /* Counts before arrays -- see diamond_program_write_compiled's own
     * comment: only the used prefix of each fixed-size table was
     * written, so the exact byte count to read next isn't known until
     * these are. */
    uint64_t class_count = 0, interface_count = 0, module_count = 0, namespace_constant_count = 0;
    if (!read_u64(&cursor, end, &class_count)) return false;
    if (!read_u64(&cursor, end, &interface_count)) return false;
    if (!read_u64(&cursor, end, &module_count)) return false;
    if (!read_u64(&cursor, end, &namespace_constant_count)) return false;
    if (class_count > DIAMOND_MAX_CLASSES || interface_count > DIAMOND_MAX_INTERFACES ||
        module_count > DIAMOND_MAX_MODULES || namespace_constant_count > DIAMOND_MAX_NAMESPACE_CONSTANTS)
        return false;

    out->class_count = (size_t)class_count;
    out->interface_count = (size_t)interface_count;
    out->module_count = (size_t)module_count;
    out->namespace_constant_count = (size_t)namespace_constant_count;

    for (uint64_t index = 0; index < class_count; index++)
        if (!read_class(&cursor, end, &out->classes[index])) return false;
    for (uint64_t index = 0; index < interface_count; index++)
        if (!read_interface(&cursor, end, &out->interfaces[index])) return false;
    for (uint64_t index = 0; index < module_count; index++)
        if (!read_module(&cursor, end, &out->modules[index])) return false;

    if (!read_bytes(&cursor, end, out->namespace_constants,
            (size_t)namespace_constant_count * sizeof out->namespace_constants[0])) return false;
    if (!read_bytes(&cursor, end, &out->range_class_index, sizeof out->range_class_index)) return false;

    /* See this file's own top comment: safe to leave every class's own
     * shapes[] exactly as the raw classes[] dump above left it (garbage
     * self-pointers included) -- run_compile_pass's own tail
     * unconditionally recomputes it for every class before anything
     * ever reads it. interfaces[].type_sets is the one field that *is*
     * read before any compile pass runs again (a lookup could reference
     * an interface by name without redeclaring it) -- fixed up here the
     * same way diamond_compile's own tail already does after an ordinary
     * compile. */
    for (size_t index = 0; index < out->interface_count; index++)
        out->interfaces[index].type_sets = out->entry.type_sets;

    return true;
}
