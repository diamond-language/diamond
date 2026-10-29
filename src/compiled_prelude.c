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
 * (write_class/write_module/write_interface below): their method, field
 * and class-variable tables are heap pointers, so each is written as a
 * count followed by that many entries (same "count before the
 * variable-length tail" shape as write_function's own dynamic arrays
 * above), and the reader allocates them again. DiamondMethod itself
 * has two pointers (source_chunk/bound_values),
 * but per its own comment (src/vm.h) both are non-null *only* for a
 * method installed at runtime via ClassName.compile_method/
 * .define_method, which the prelude's own source never does to itself
 * -- checked directly below (assert_methods_are_plain), not assumed.
 * DiamondClass.shapes[].class (a self-referential pointer) is never
 * written -- the raw bytes a write/read round-trip would carry over
 * point at the *original* program's own classes[] array, not `out`'s;
 * diamond_program_read_compiled's own tail calls diamond_program_
 * recompute_shapes (src/compiler.h) explicitly instead of relying on a
 * later compile pass to fix it up the way an ordinary diamond_compile(_
 * incremental) call's own run_compile_pass tail already does for a
 * freshly-compiled program (template-seeded classes included) -- a
 * caller that only ever deserializes an already-fully-compiled program,
 * never compiling anything further against it, has no such later pass
 * to rely on. Confirmed directly, not assumed: a user-defined class's
 * own instance-variable reads returned Nil without this. */

/* See bignum.c's own identical comment: needed transitively for vm.h's
 * <ucontext.h> use (via compiler.h), only under musl (docs/portability.md). */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "compiled_prelude.h"
#include "disassemble.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool write_all(FILE *file, const void *data, size_t size) {
    return size == 0 || fwrite(data, 1, size, file) == size;
}

/* A DiamondFunction is ~11 KB of fixed-size tables (scope facts, parameter
 * names, per-register facts) of which a typical function uses a few hundred
 * bytes. Its image is therefore written as records of (zero bytes, literal
 * length, literal bytes) until the whole struct is covered, which stays
 * correct for any field added later and makes a cache file roughly the size
 * of the code it holds. Pointer and JIT fields are zeroed first: the reader
 * rebuilds them and they mean nothing in another process. */
static bool write_function_image(FILE *file, const DiamondFunction *function) {
    DiamondFunction image;
    memcpy(&image, function, sizeof image);
    image.code = nullptr;
    image.lines = nullptr;
    image.columns = nullptr;
    image.constants = nullptr;
    image.strings = nullptr;
    image.type_sets = nullptr;
    image.jit_code = nullptr;
    image.jit_code_size = 0;
    image.jit_call_count = 0;
    const uint8_t *bytes = (const uint8_t *)&image;
    size_t position = 0;
    while (position < sizeof image) {
        size_t zeros = 0;
        while (position + zeros < sizeof image && bytes[position + zeros] == 0) zeros++;
        size_t literal = 0;
        /* A literal ends at a run of 8 or more zeros, so isolated zero bytes
         * inside live data do not each pay for a record header. */
        size_t trailing_zeros = 0;
        while (position + zeros + literal < sizeof image) {
            if (bytes[position + zeros + literal] == 0) {
                if (++trailing_zeros >= 8) break;
            } else {
                trailing_zeros = 0;
            }
            literal++;
        }
        literal -= trailing_zeros < 8 ? 0 : 7;
        const uint32_t zero_count = (uint32_t)zeros;
        const uint32_t literal_count = (uint32_t)literal;
        if (!write_all(file, &zero_count, sizeof zero_count) ||
            !write_all(file, &literal_count, sizeof literal_count) ||
            !write_all(file, bytes + position + zeros, literal))
            return false;
        position += zeros + literal;
    }
    return true;
}

static bool write_function(FILE *file, const DiamondFunction *function) {
    if (!write_function_image(file, function)) return false;
    if (!write_all(file, function->code, function->code_count * sizeof *function->code)) return false;
    if (!write_all(file, function->lines, function->code_count * sizeof *function->lines)) return false;
    if (!write_all(file, function->columns, function->code_count * sizeof *function->columns)) return false;
    if (!write_all(file, function->constants, function->constant_count * sizeof *function->constants)) return false;
    /* A DiamondStringConstant is a fixed DIAMOND_MAX_STRING_LENGTH buffer in
     * memory, almost all padding; on disk each is just its length and bytes.
     * Writing whole structs made a small program's cache tens of megabytes. */
    for (size_t index = 0; index < function->string_count; index++) {
        const uint64_t length = (uint64_t)function->strings[index].length;
        if (!write_all(file, &length, sizeof length)) return false;
        if (!write_all(file, function->strings[index].chars, (size_t)length)) return false;
    }
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

/* Writes the used entries of a class's own methods[]/singleton_methods[]/
 * fields[]/class_variables[] tables -- see this file's own top comment. shapes[]
 * is never written: always safely recomputed on the read side before
 * anything reads it. */
static bool write_class(FILE *file, const DiamondClass *class) {
    if (!write_all(file, class->name, sizeof class->name)) return false;
    if (!write_all(file, class->type_parameter, sizeof class->type_parameter)) return false;
    if (!write_all(file, class->second_type_parameter, sizeof class->second_type_parameter)) return false;
    const uint8_t generated_struct=class->generated_struct?1:0;
    if(!write_all(file,&generated_struct,sizeof generated_struct))return false;
    const uint8_t fixed=class->fixed_type_arguments?1:0;
    if(!write_all(file,&fixed,sizeof fixed))return false;
    const uint8_t swapped=class->swaps_parent_arguments?1:0;
    if(!write_all(file,&swapped,sizeof swapped))return false;
    const uint8_t transformed=class->transforms_parent_arguments?1:0;
    if(!write_all(file,&transformed,sizeof transformed))return false;
    if(transformed&&!write_all(file,class->parent_type_bindings,sizeof class->parent_type_bindings))return false;
    if(fixed&&!write_all(file,class->fixed_type_bindings,sizeof class->fixed_type_bindings))return false;
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

/* Writes `program`, leaving out its first `skip` functions. The embedded
 * prelude blob skips none; a bytecode cache file skips the prelude's own
 * functions when the reader can rebuild them from the binary's embedded
 * copy (see diamond_program_write_cache_file). */
static bool write_program_body(const DiamondProgram *program, size_t skip, FILE *file) {
    if (!write_function(file, &program->entry)) return false;

    const uint64_t function_count = (uint64_t)(program->function_count - skip);
    if (!write_all(file, &function_count, sizeof function_count)) return false;
    for (size_t index = skip; index < program->function_count; index++)
        if (!write_function(file, program->functions[index])) return false;

    for (size_t index = 0; index < program->class_count; index++) {
        assert_methods_are_plain(program->classes[index]->methods,
            program->classes[index]->method_count);
        assert_methods_are_plain(program->classes[index]->singleton_methods,
            program->classes[index]->singleton_method_count);
    }
    for (size_t index = 0; index < program->module_count; index++) {
        assert_methods_are_plain(program->modules[index]->methods,
            program->modules[index]->method_count);
        assert_methods_are_plain(program->modules[index]->singleton_methods,
            program->modules[index]->singleton_method_count);
    }

    /* Counts before arrays, not after: only the *used* prefix of each
     * fixed-size table is written (class_count entries out of
     * DIAMOND_MAX_CLASSES=1024, not all 1024 -- the prelude only ever
     * populates ~23), so the reader needs each count in hand before it
     * knows how many bytes to expect. The unused suffix doesn't need
     * writing at all: diamond_program_init/calloc already zero it on
     * the reading side, exactly matching what an unused slot already
     * looks like in a freshly compiled program. Shrinks the serialized
     * form from a full ~27MB DiamondProgram-shaped dump (dominated by
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
        if (!write_class(file, program->classes[index])) return false;
    for (size_t index = 0; index < program->interface_count; index++)
        if (!write_interface(file, program->interfaces[index])) return false;
    for (size_t index = 0; index < program->module_count; index++)
        if (!write_module(file, program->modules[index])) return false;
    if (program->namespace_constant_count > 0 && !write_all(file, program->namespace_constants,
            program->namespace_constant_count * sizeof program->namespace_constants[0])) return false;
    if (!write_all(file, &program->range_class_index, sizeof program->range_class_index)) return false;

    return true;
}

bool diamond_program_write_compiled(const DiamondProgram *program, FILE *file) {
    return write_program_body(program, 0, file);
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
    DiamondFunction view;
    memset(&view, 0, sizeof view);
    size_t position = 0;
    while (position < sizeof view) {
        const uint8_t *header = take(cursor, end, 2 * sizeof(uint32_t));
        if (header == nullptr) return false;
        uint32_t zero_count, literal_count;
        memcpy(&zero_count, header, sizeof zero_count);
        memcpy(&literal_count, header + sizeof zero_count, sizeof literal_count);
        if (zero_count > sizeof view - position ||
            literal_count > sizeof view - position - zero_count)
            return false;
        const uint8_t *literal = take(cursor, end, literal_count);
        if (literal == nullptr && literal_count > 0) return false;
        if (literal_count > 0)
            memcpy((uint8_t *)&view + position + zero_count, literal, literal_count);
        position += (size_t)zero_count + literal_count;
        /* A record that covers nothing would loop forever on a hostile file. */
        if (zero_count == 0 && literal_count == 0) return false;
    }
    /* Every count below comes straight off disk: bound it by the bytes that
     * remain before multiplying, so a corrupted count can't wrap a size_t
     * product into a small, in-bounds-looking take(). */
    if(view.type_variable_count>8||view.class_type_variable>2||
       view.class_type_variable>view.type_variable_count)return false;
    const size_t remaining = (size_t)(end - *cursor);
    if (view.code_count > remaining / sizeof *view.code ||
        view.constant_count > remaining / sizeof *view.constants ||
        view.type_set_count > remaining / sizeof *view.type_sets)
        return false;
    const uint8_t *code = take(cursor, end, view.code_count * sizeof *view.code);
    const uint8_t *lines_bytes = take(cursor, end, view.code_count * sizeof *view.lines);
    const uint8_t *columns_bytes = take(cursor, end, view.code_count * sizeof *view.columns);
    const uint8_t *constants_bytes = take(cursor, end, view.constant_count * sizeof *view.constants);
    if ((view.code_count > 0 && (code == nullptr || lines_bytes == nullptr || columns_bytes == nullptr)) ||
        (view.constant_count > 0 && constants_bytes == nullptr))
        return false;
    /* Expand the length-prefixed strings back into fixed in-memory records. */
    if (view.string_count > (size_t)(end - *cursor) / sizeof(uint64_t)) return false;
    DiamondStringConstant *strings = nullptr;
    if (view.string_count > 0) {
        strings = calloc(view.string_count, sizeof *strings);
        if (strings == nullptr) return false;
    }
    for (size_t index = 0; index < view.string_count; index++) {
        uint64_t length = 0;
        const uint8_t *length_bytes = take(cursor, end, sizeof length);
        if (length_bytes != nullptr) memcpy(&length, length_bytes, sizeof length);
        const uint8_t *chars = length_bytes != nullptr && length <= DIAMOND_MAX_STRING_LENGTH ?
            take(cursor, end, (size_t)length) : nullptr;
        if (chars == nullptr && !(length_bytes != nullptr && length == 0)) {
            free(strings);
            return false;
        }
        if (length > 0) memcpy(strings[index].chars, chars, (size_t)length);
        strings[index].length = (size_t)length;
    }
    const uint8_t *type_sets_bytes = take(cursor, end, view.type_set_count * sizeof *view.type_sets);
    if (view.type_set_count > 0 && type_sets_bytes == nullptr) {
        free(strings);
        return false;
    }
    view.code = (uint8_t *)code;
    view.lines = (uint32_t *)(const void *)lines_bytes;
    view.columns = (uint32_t *)(const void *)columns_bytes;
    view.constants = (DiamondValue *)(const void *)constants_bytes;
    view.strings = strings;
    view.type_sets = (DiamondTypeSet *)(const void *)type_sets_bytes;
    const bool copied = diamond_function_copy(destination, &view);
    free(strings);
    return copied;
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

/* A DiamondMethod is raw-dumped, pointer fields included. The writer only
 * ever sees plain methods (assert_methods_are_plain), so on the way back in
 * those fields are always null; forcing that here means a hand-edited file
 * can't smuggle in a chunk or bound-value pointer of its own choosing. */
static void clear_method_pointers(DiamondMethod *methods, size_t count) {
    for (size_t index = 0; index < count; index++) {
        methods[index].source_chunk = nullptr;
        methods[index].bound_values = nullptr;
        methods[index].bound_value_count = 0;
    }
}

/* Inverse of write_class: rejects an oversized method_count/field_count/
 * class_variable_count up front (this build's own DIAMOND_MAX_METHODS/
 * DIAMOND_MAX_FIELDS budget) rather than allocating whatever a corrupt
 * count asks for -- same defensive posture as
 * diamond_program_read_compiled's own class_count/interface_count/
 * module_count checks below. */
static bool read_class(const uint8_t **cursor, const uint8_t *end, DiamondClass *out) {
    diamond_class_free_tables(out);
    memset(out, 0, sizeof *out);
    if (!read_bytes(cursor, end, out->name, sizeof out->name)) return false;
    if (!read_bytes(cursor, end, out->type_parameter, sizeof out->type_parameter)) return false;
    if(memchr(out->type_parameter,'\0',sizeof out->type_parameter)==nullptr)return false;
    if (!read_bytes(cursor, end, out->second_type_parameter, sizeof out->second_type_parameter)) return false;
    if(memchr(out->second_type_parameter,'\0',sizeof out->second_type_parameter)==nullptr)return false;
    if(out->second_type_parameter[0]!='\0'&&out->type_parameter[0]=='\0')return false;
    uint8_t generated_struct=0;
    if(!read_bytes(cursor,end,&generated_struct,sizeof generated_struct)||generated_struct>1)return false;
    out->generated_struct=generated_struct!=0;
    uint8_t fixed=0;
    if(!read_bytes(cursor,end,&fixed,sizeof fixed)||fixed>1)return false;
    out->fixed_type_arguments=fixed!=0;
    uint8_t swapped=0;
    if(!read_bytes(cursor,end,&swapped,sizeof swapped)||swapped>1)return false;
    out->swaps_parent_arguments=swapped!=0;
    uint8_t transformed=0;
    if(!read_bytes(cursor,end,&transformed,sizeof transformed)||transformed>1)return false;
    out->transforms_parent_arguments=transformed!=0;
    if(transformed&&!read_bytes(cursor,end,out->parent_type_bindings,sizeof out->parent_type_bindings))return false;
    if(fixed&&(!read_bytes(cursor,end,out->fixed_type_bindings,sizeof out->fixed_type_bindings)||
       DIAMOND_CLASS_TYPE_PARAMETER_COUNT(out)==0))return false;
    if (!read_bytes(cursor, end, &out->declaration_line, sizeof out->declaration_line)) return false;
    if (!read_bytes(cursor, end, &out->declaration_column, sizeof out->declaration_column)) return false;
    if (!read_bytes(cursor, end, &out->declaration_start, sizeof out->declaration_start)) return false;
    if (!read_bytes(cursor, end, &out->superclass, sizeof out->superclass)) return false;

    uint64_t method_count = 0, singleton_method_count = 0;
    if (!read_u64(cursor, end, &method_count)) return false;
    if (!read_u64(cursor, end, &singleton_method_count)) return false;
    if (method_count > DIAMOND_MAX_METHODS || singleton_method_count > DIAMOND_MAX_METHODS) return false;
    if (!diamond_reserve_methods(&out->methods, &out->method_capacity, (size_t)method_count) ||
        !diamond_reserve_methods(&out->singleton_methods, &out->singleton_method_capacity,
            (size_t)singleton_method_count)) return false;
    out->method_count = (size_t)method_count;
    out->singleton_method_count = (size_t)singleton_method_count;
    if (!read_bytes(cursor, end, out->methods, (size_t)method_count * sizeof out->methods[0])) return false;
    if (!read_bytes(cursor, end, out->singleton_methods,
            (size_t)singleton_method_count * sizeof out->singleton_methods[0])) return false;
    clear_method_pointers(out->methods, out->method_count);
    clear_method_pointers(out->singleton_methods, out->singleton_method_count);

    uint64_t field_count = 0;
    if (!read_u64(cursor, end, &field_count)) return false;
    if (field_count > DIAMOND_MAX_FIELDS) return false;
    if (!diamond_class_reserve_fields(out, (size_t)field_count)) return false;
    out->field_count = (size_t)field_count;
    if (!read_bytes(cursor, end, out->fields, (size_t)field_count * sizeof out->fields[0])) return false;
    if (!read_bytes(cursor, end, out->field_type_status, (size_t)field_count * sizeof out->field_type_status[0])) return false;
    if (!read_bytes(cursor, end, out->field_known_class, (size_t)field_count * sizeof out->field_known_class[0])) return false;
    /* At this boundary all methods are compiled; final field facts also
     * supply receiver-dependent reader inference in the restored program. */
    if (field_count > 0) {
        memcpy(out->discovered_field_type_status,out->field_type_status,
            (size_t)field_count*sizeof out->field_type_status[0]);
        memcpy(out->discovered_field_known_class,out->field_known_class,
            (size_t)field_count*sizeof out->field_known_class[0]);
    }

    uint64_t class_variable_count = 0;
    if (!read_u64(cursor, end, &class_variable_count)) return false;
    if (class_variable_count > DIAMOND_MAX_CLASS_VARIABLES) return false;
    if (!diamond_class_reserve_class_variables(out, (size_t)class_variable_count)) return false;
    out->class_variable_count = (size_t)class_variable_count;
    if (!read_bytes(cursor, end, out->class_variables,
            (size_t)class_variable_count * sizeof out->class_variables[0])) return false;

    if (!read_bytes(cursor, end, &out->declared_by_discovery, sizeof out->declared_by_discovery)) return false;
    return true;
}

static bool read_module(const uint8_t **cursor, const uint8_t *end, DiamondModule *out) {
    diamond_module_free_tables(out);
    memset(out, 0, sizeof *out);
    if (!read_bytes(cursor, end, out->name, sizeof out->name)) return false;
    if (!read_bytes(cursor, end, &out->declaration_line, sizeof out->declaration_line)) return false;
    if (!read_bytes(cursor, end, &out->declaration_column, sizeof out->declaration_column)) return false;
    if (!read_bytes(cursor, end, &out->declaration_start, sizeof out->declaration_start)) return false;

    uint64_t method_count = 0, singleton_method_count = 0;
    if (!read_u64(cursor, end, &method_count)) return false;
    if (!read_u64(cursor, end, &singleton_method_count)) return false;
    if (method_count > DIAMOND_MAX_METHODS || singleton_method_count > DIAMOND_MAX_METHODS) return false;
    if (!diamond_reserve_methods(&out->methods, &out->method_capacity, (size_t)method_count) ||
        !diamond_reserve_methods(&out->singleton_methods, &out->singleton_method_capacity,
            (size_t)singleton_method_count)) return false;
    out->method_count = (size_t)method_count;
    out->singleton_method_count = (size_t)singleton_method_count;
    if (!read_bytes(cursor, end, out->methods, (size_t)method_count * sizeof out->methods[0])) return false;
    if (!read_bytes(cursor, end, out->singleton_methods,
            (size_t)singleton_method_count * sizeof out->singleton_methods[0])) return false;
    clear_method_pointers(out->methods, out->method_count);
    clear_method_pointers(out->singleton_methods, out->singleton_method_count);

    if (!read_bytes(cursor, end, &out->next_singleton_claim, sizeof out->next_singleton_claim)) return false;

    uint64_t field_count = 0;
    if (!read_u64(cursor, end, &field_count)) return false;
    if (field_count > DIAMOND_MAX_FIELDS) return false;
    if (!diamond_module_reserve_fields(out, (size_t)field_count)) return false;
    out->field_count = (size_t)field_count;
    if (!read_bytes(cursor, end, out->fields, (size_t)field_count * sizeof out->fields[0])) return false;

    if (!read_bytes(cursor, end, &out->declared_by_discovery, sizeof out->declared_by_discovery)) return false;
    return true;
}

/* type_sets is left nullptr here -- diamond_program_read_compiled's own
 * tail fixes it up for every interface afterward, same as it always has. */
static bool read_interface(const uint8_t **cursor, const uint8_t *end, DiamondInterface *out) {
    diamond_interface_free_tables(out);
    memset(out, 0, sizeof *out);
    if (!read_bytes(cursor, end, out->name, sizeof out->name)) return false;
    if (!read_bytes(cursor, end, &out->declaration_line, sizeof out->declaration_line)) return false;
    if (!read_bytes(cursor, end, &out->declaration_column, sizeof out->declaration_column)) return false;
    if (!read_bytes(cursor, end, &out->declaration_start, sizeof out->declaration_start)) return false;

    uint64_t method_count = 0;
    if (!read_u64(cursor, end, &method_count)) return false;
    if (method_count > DIAMOND_MAX_METHODS) return false;
    if (!diamond_reserve_interface_methods(&out->methods, &out->method_capacity,
            (size_t)method_count)) return false;
    out->method_count = (size_t)method_count;
    if (!read_bytes(cursor, end, out->methods, (size_t)method_count * sizeof out->methods[0])) return false;

    if (!read_bytes(cursor, end, &out->declared_by_discovery, sizeof out->declared_by_discovery)) return false;
    return true;
}

/* Reads a program whose first `skip` functions were left out, taking them
 * from `template` (which must hold exactly `skip` functions) instead. */
static bool read_program_body(const uint8_t *data, size_t size, DiamondProgram *out,
        const DiamondProgram *template, size_t skip) {
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;

    if (!read_function(&cursor, end, &out->entry)) return false;

    for (size_t index = 0; index < skip; index++) {
        DiamondFunction *slot = diamond_program_add_function(out);
        if (slot == nullptr || !diamond_function_copy(slot, template->functions[index]))
            return false;
    }

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

    for (uint64_t index = 0; index < class_count; index++) {
        DiamondClass *class = diamond_program_class_slot(out, (size_t)index);
        if (class == nullptr || !read_class(&cursor, end, class)) return false;
        class->index = (uint16_t)index;
    }
    for (uint64_t index = 0; index < interface_count; index++) {
        DiamondInterface *interface = diamond_program_interface_slot(out, (size_t)index);
        if (interface == nullptr || !read_interface(&cursor, end, interface)) return false;
    }
    for (uint64_t index = 0; index < module_count; index++) {
        DiamondModule *module = diamond_program_module_slot(out, (size_t)index);
        if (module == nullptr || !read_module(&cursor, end, module)) return false;
    }

    if (!diamond_program_reserve_namespace_constants(out, (size_t)namespace_constant_count))
        return false;
    if (namespace_constant_count > 0 && !read_bytes(&cursor, end, out->namespace_constants,
            (size_t)namespace_constant_count * sizeof out->namespace_constants[0])) return false;
    if (!read_bytes(&cursor, end, &out->range_class_index, sizeof out->range_class_index)) return false;

    /* Every class's own shapes[] (self-referential `shape->class`
     * back-pointers) must be recomputed here, not left as whatever the
     * raw classes[] dump carried over: those pointers were computed
     * against the *original* program's own classes[] array, at a
     * different memory address than `out`'s -- confirmed directly, not
     * assumed (a user-defined class's own instance-variable reads
     * returned Nil without this, since field access resolves through a
     * shape whose `class` pointer no longer matched the class actually
     * being read). run_compile_pass's own tail (src/compiler.c) recomputes
     * this for free after an ordinary compile, but nothing runs a compile
     * pass over an already-fully-compiled deserialized program the way
     * diamond_compile_incremental against a template does for the
     * prelude's own template-seeded classes -- see diamond_program_
     * recompute_shapes' own comment (src/compiler.h). interfaces[].
     * type_sets is fixed up the same way diamond_compile's own tail
     * already does after an ordinary compile, for the same reason: it's
     * read before any compile pass could otherwise fix it up (a lookup
     * could reference an interface by name without redeclaring it). */
    diamond_program_recompute_shapes(out);
    for (size_t index = 0; index < out->interface_count; index++)
        out->interfaces[index]->type_sets = out->entry.type_sets;

    return true;
}

static bool fixed_class_bindings_valid(const DiamondProgram *program,const DiamondClass *class);

bool diamond_program_read_compiled(const uint8_t *data, size_t size, DiamondProgram *out) {
    if(!read_program_body(data,size,out,nullptr,0))return false;
    for(size_t i=0;i<out->class_count;i++)
        if(!fixed_class_bindings_valid(out,out->classes[i]))return false;
    return true;
}

/* Semantic checks over a program read back from disk, run before anything
 * executes it. The reader above only guarantees the bytes fit the structs;
 * this guarantees the structs don't point outside the program. Anything
 * that fails is treated by callers exactly like a missing file. */
static bool method_table_valid(const DiamondMethod *methods, size_t count, size_t function_count) {
    for (size_t index = 0; index < count; index++)
        if (methods[index].function_index >= function_count) return false;
    return true;
}

static bool constants_are_scalar(const DiamondFunction *function) {
    for (size_t index = 0; index < function->constant_count; index++) {
        const DiamondValueKind kind = function->constants[index].kind;
        if (kind != DIAMOND_VALUE_NIL && kind != DIAMOND_VALUE_BOOL &&
            kind != DIAMOND_VALUE_INT && kind != DIAMOND_VALUE_FLOAT)
            return false;
    }
    return true;
}

static bool fixed_binding_node_valid(const DiamondProgram *program,const DiamondTypeBinding *binding,
        uint8_t node,uint16_t active,uint16_t *reachable,size_t variables) {
    if(node==UINT8_MAX)return true;
    if(node>=binding->node_count||(active&(uint16_t)(1u<<node))!=0)return false;
    if(variables>0&&(*reachable&(uint16_t)(1u<<node))!=0)return false;
    active|=(uint16_t)(1u<<node);
    *reachable|=(uint16_t)(1u<<node);
    const DiamondBoundTypeNode *set=&binding->nodes[node];
    if(set->count==0||set->count>DIAMOND_BOUND_TYPE_MEMBERS)return false;
    for(size_t i=0;i<set->count;i++) {
        const DiamondBoundTypeMember member=set->members[i];
        const bool class_type=member.id>=DIAMOND_TYPE_CLASS_BASE&&member.id<DIAMOND_TYPE_CLASS_LIMIT;
        if(member.id>=DIAMOND_TYPE_CLASS_LIMIT&&member.id<DIAMOND_TYPE_INTERFACE_BASE&&
           member.id!=DIAMOND_TYPE_NATIVE&&
           !(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&member.id<DIAMOND_TYPE_VARIABLE_BASE+variables))return false;
        if(member.argument_node!=UINT8_MAX&&!class_type&&member.id!=DIAMOND_TYPE_ARRAY&&
           member.id!=DIAMOND_TYPE_HASH)return false;
        if(member.second_argument_node!=UINT8_MAX&&!class_type&&member.id!=DIAMOND_TYPE_HASH)return false;
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&member.id<DIAMOND_TYPE_INTERFACE_BASE&&
           (member.id>=DIAMOND_TYPE_VARIABLE_BASE+variables||set->count!=1||
            member.argument_node!=UINT8_MAX||member.second_argument_node!=UINT8_MAX||member.signature!=UINT8_MAX))return false;
        if(member.id>=DIAMOND_TYPE_INTERFACE_BASE&&
           (size_t)(member.id-DIAMOND_TYPE_INTERFACE_BASE)>=program->interface_count)return false;
        if(member.id>=DIAMOND_TYPE_CLASS_BASE&&member.id<DIAMOND_TYPE_CLASS_LIMIT&&
           (size_t)(member.id-DIAMOND_TYPE_CLASS_BASE)>=program->class_count)return false;
        if(member.id==DIAMOND_TYPE_NATIVE&&
           strcmp(diamond_native_type_name(member.native_kind),"<native>")==0)return false;
        if(!fixed_binding_node_valid(program,binding,member.argument_node,active,reachable,variables)||
           !fixed_binding_node_valid(program,binding,member.second_argument_node,active,reachable,variables))return false;
        if(member.id==DIAMOND_TYPE_CALLABLE&&member.signature!=UINT8_MAX) {
            if(member.signature>=binding->signature_count)return false;
            const DiamondBoundSignature *signature=&binding->signatures[member.signature];
            const bool zero=false,one=true;
            if((memcmp(&signature->parameters_typed,&zero,sizeof zero)!=0&&
                memcmp(&signature->parameters_typed,&one,sizeof one)!=0)||
               memcmp(&signature->variadic,&zero,sizeof zero)!=0||
               signature->required_arity!=signature->arity||
               (signature->parameters_typed&&signature->arity>16))return false;
            if(signature->parameters_typed)
                for(size_t p=0;p<signature->arity;p++)
                    if(!fixed_binding_node_valid(program,binding,signature->parameter_nodes[p],active,reachable,variables))return false;
            if(!fixed_binding_node_valid(program,binding,signature->return_node,active,reachable,variables))return false;
        }
    }
    return true;
}

static bool fixed_class_bindings_valid(const DiamondProgram *program,const DiamondClass *class) {
    if(class->generated_struct&&class->superclass!=DIAMOND_NO_CLASS&&
       (class->superclass>=program->class_count||class->superclass==class->index||
        !program->classes[class->superclass]->generated_struct))return false;
    if(class->transforms_parent_arguments) {
        const size_t variables=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class);
        if(class->fixed_type_arguments||class->swaps_parent_arguments||variables==0||
           class->superclass>=program->class_count||class->superclass==class->index)return false;
        unsigned occurrences[2]={};
        const size_t parent_count=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(program->classes[class->superclass]);
        if(parent_count==0)return false;
        for(size_t i=0;i<parent_count;i++) {
            const DiamondTypeBinding *binding=&class->parent_type_bindings[i];
            uint16_t reachable=0;const bool zero=false;
            if(memcmp(&binding->invalid,&zero,sizeof zero)!=0||binding->node_count==0||
               binding->node_count>DIAMOND_BOUND_TYPE_NODES||binding->signature_count>DIAMOND_BOUND_SIGNATURES||
               !fixed_binding_node_valid(program,binding,0,0,&reachable,variables)||
               reachable!=(uint16_t)((1u<<binding->node_count)-1u))return false;
            for(size_t n=0;n<binding->node_count;n++)
                for(size_t m=0;m<binding->nodes[n].count;m++) {
                    const uint16_t id=binding->nodes[n].members[m].id;
                    if(id>=DIAMOND_TYPE_VARIABLE_BASE&&id<DIAMOND_TYPE_VARIABLE_BASE+variables)
                        occurrences[id-DIAMOND_TYPE_VARIABLE_BASE]++;
                }
        }
        for(size_t i=0;i<variables;i++)
            if(occurrences[i]>1||(!class->generated_struct&&occurrences[i]!=1))return false;
    }
    if(class->swaps_parent_arguments&&(class->fixed_type_arguments||
       DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class)!=2||class->superclass>=program->class_count||
       program->classes[class->superclass]->fixed_type_arguments||
       DIAMOND_CLASS_TYPE_PARAMETER_COUNT(program->classes[class->superclass])!=2))return false;
    if(!class->fixed_type_arguments) {
        if(DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class)==0||class->superclass==DIAMOND_NO_CLASS)return true;
        if(class->superclass>=program->class_count||class->superclass==class->index)return false;
        const DiamondClass *parent=program->classes[class->superclass];
        return (!parent->fixed_type_arguments||(class->generated_struct&&class->transforms_parent_arguments))&&
            (class->transforms_parent_arguments||
             (class->generated_struct&&DIAMOND_CLASS_TYPE_PARAMETER_COUNT(parent)==0)||
             DIAMOND_CLASS_TYPE_PARAMETER_COUNT(parent)==DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class));
    }
    if(class->superclass==DIAMOND_NO_CLASS||class->superclass>=program->class_count||
       class->superclass==class->index)return false;
    const DiamondClass *parent=program->classes[class->superclass];
    const size_t count=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class);
    if(count==0||count!=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(parent))return false;
    if(parent->fixed_type_arguments&&memcmp(class->fixed_type_bindings,parent->fixed_type_bindings,
        sizeof class->fixed_type_bindings)!=0)return false;
    const bool zero=false;
    for(size_t i=0;i<count;i++) {
        const DiamondTypeBinding *binding=&class->fixed_type_bindings[i];
        uint16_t reachable=0;
        if(memcmp(&binding->invalid,&zero,sizeof zero)!=0||binding->node_count==0||
           binding->node_count>DIAMOND_BOUND_TYPE_NODES||binding->signature_count>DIAMOND_BOUND_SIGNATURES||
           !fixed_binding_node_valid(program,binding,0,0,&reachable,0)||
           reachable!=(uint16_t)((1u<<binding->node_count)-1u))return false;
    }
    return true;
}

static bool program_is_valid(const DiamondProgram *program) {
    if (!constants_are_scalar(&program->entry)) return false;
    for (size_t index = 0; index < program->function_count; index++)
        if (!constants_are_scalar(program->functions[index])) return false;
    for (size_t index = 0; index < program->class_count; index++) {
        const DiamondClass *class = program->classes[index];
        if (class->superclass != DIAMOND_NO_CLASS && class->superclass >= program->class_count) return false;
        if(!fixed_class_bindings_valid(program,class))return false;
        if (!method_table_valid(class->methods, class->method_count, program->function_count) ||
            !method_table_valid(class->singleton_methods, class->singleton_method_count,
                program->function_count))
            return false;
    }
    for (size_t index = 0; index < program->module_count; index++) {
        const DiamondModule *module = program->modules[index];
        if (!method_table_valid(module->methods, module->method_count, program->function_count) ||
            !method_table_valid(module->singleton_methods, module->singleton_method_count,
                program->function_count))
            return false;
    }
    const DiamondChunk chunk = diamond_program_chunk(program);
    return diamond_verify_bytecode(&chunk);
}

/* Not cryptographic: it detects truncation, bit rot, partial overwrites and
 * hand edits, the ways a file next to a source file really goes wrong. A
 * file someone deliberately forges with a matching checksum still has to get
 * past program_is_valid. Eight bytes per step keeps this to a few
 * milliseconds over a multi-megabyte cache. */
static uint64_t body_checksum(const uint8_t *data, size_t size) {
    uint64_t hash = 0x9e3779b97f4a7c15ull ^ (uint64_t)size;
    size_t index = 0;
    for (; index + 8 <= size; index += 8) {
        uint64_t word;
        memcpy(&word, data + index, sizeof word);
        hash = (hash ^ word) * 0xff51afd7ed558ccdull;
        hash ^= hash >> 32;
    }
    uint64_t tail = 0;
    memcpy(&tail, data + index, size - index);
    hash = (hash ^ tail) * 0xff51afd7ed558ccdull;
    hash ^= hash >> 29;
    return hash;
}

/* Deliberately not char[8] (no room for a null terminator, only every
 * on-disk-magic-length reference below actually needs) -- newer GCC's
 * -Wunterminated-string-initialization flags that even though it's
 * valid ISO C, so this stays a plain, ordinarily-terminated string
 * constant and DIAMOND_CACHE_MAGIC_LENGTH is the one place the "how many
 * bytes actually go on disk" fact lives. */
static constexpr char DIAMOND_CACHE_MAGIC[] = "DIACACHE";
enum { DIAMOND_CACHE_MAGIC_LENGTH = 8 };
/* 2: string constants are length-prefixed instead of fixed 4KB records.
 * 3: the fingerprint carries build_id.
 * 4: a body checksum follows the source hash.
 * 5: the body starts with a count of leading functions left out because they
 *    are the embedded prelude's own (0 when none were).
 * 6: each function's struct image is stored as zero-run records instead of
 *    its full ~11 KB.
 * 7: the fingerprint carries the writer's byte order, so an image built for
 *    an opposite-endian target is rejected instead of misread.
 * 8: class operands are 16 bits wide and the native/variable/interface
 *    type-id bases moved, so every instruction that names a class is a byte
 *    longer and every stored type id has a different value.
 * 9: classes store a type parameter; methods distinguish its instance binding.
 * 10: NEW_TYPED constructs explicitly specialized generic instances.
 * 11: generic class methods bind method variables after the receiver slot. */
enum { DIAMOND_CACHE_FORMAT_VERSION = 17 };

/* A checksum of the compiler's own sources and prelude (every .c and .h
 * file under src/, plus lib/core.d6 and lib/core/), passed in by the
 * Makefile. The
 * layout fields above only notice a change to the file format; a compiler
 * fix that changes the bytecode generated for the same source leaves them
 * all identical, and without this a rebuilt or upgraded `diamond` would
 * keep running bytecode its predecessor compiled. A build that doesn't
 * define it (a test harness compiling the sources directly) uses 0. */
#ifndef DIAMOND_BUILD_ID
#define DIAMOND_BUILD_ID 0u
#endif

DiamondCacheFingerprint diamond_cache_fingerprint(void) {
    return (DiamondCacheFingerprint){
        .format_version=DIAMOND_CACHE_FORMAT_VERSION,
        .function_size=(uint32_t)sizeof(DiamondFunction),
        .class_size=(uint32_t)sizeof(DiamondClass),
        .interface_size=(uint32_t)sizeof(DiamondInterface),
        .module_size=(uint32_t)sizeof(DiamondModule),
        .opcode_count=(uint32_t)DIAMOND_OP_COUNT,
        .builtin_class_count=(uint32_t)DIAMOND_BUILTIN_CLASS_COUNT,
        .max_classes=(uint32_t)DIAMOND_MAX_CLASSES,
        .max_methods=(uint32_t)DIAMOND_MAX_METHODS,
        .build_id=(uint32_t)DIAMOND_BUILD_ID,
        .byte_order=0x01020304u,
    };
}

/* Field-by-field, not a raw struct write -- same reason write_class/
 * write_module/write_interface above already avoid that (no padding-
 * layout ambiguity to worry about, but consistent with this file's own
 * established style throughout). */
static bool write_cache_fingerprint(FILE *file, const DiamondCacheFingerprint *fingerprint) {
    if (!write_all(file, &fingerprint->format_version, sizeof fingerprint->format_version)) return false;
    if (!write_all(file, &fingerprint->function_size, sizeof fingerprint->function_size)) return false;
    if (!write_all(file, &fingerprint->class_size, sizeof fingerprint->class_size)) return false;
    if (!write_all(file, &fingerprint->interface_size, sizeof fingerprint->interface_size)) return false;
    if (!write_all(file, &fingerprint->module_size, sizeof fingerprint->module_size)) return false;
    if (!write_all(file, &fingerprint->opcode_count, sizeof fingerprint->opcode_count)) return false;
    if (!write_all(file, &fingerprint->builtin_class_count, sizeof fingerprint->builtin_class_count)) return false;
    if (!write_all(file, &fingerprint->max_classes, sizeof fingerprint->max_classes)) return false;
    if (!write_all(file, &fingerprint->max_methods, sizeof fingerprint->max_methods)) return false;
    if (!write_all(file, &fingerprint->build_id, sizeof fingerprint->build_id)) return false;
    if (!write_all(file, &fingerprint->byte_order, sizeof fingerprint->byte_order)) return false;
    return true;
}

static bool read_cache_fingerprint(const uint8_t **cursor, const uint8_t *end,
        DiamondCacheFingerprint *out) {
    if (!read_bytes(cursor, end, &out->format_version, sizeof out->format_version)) return false;
    if (!read_bytes(cursor, end, &out->function_size, sizeof out->function_size)) return false;
    if (!read_bytes(cursor, end, &out->class_size, sizeof out->class_size)) return false;
    if (!read_bytes(cursor, end, &out->interface_size, sizeof out->interface_size)) return false;
    if (!read_bytes(cursor, end, &out->module_size, sizeof out->module_size)) return false;
    if (!read_bytes(cursor, end, &out->opcode_count, sizeof out->opcode_count)) return false;
    if (!read_bytes(cursor, end, &out->builtin_class_count, sizeof out->builtin_class_count)) return false;
    if (!read_bytes(cursor, end, &out->max_classes, sizeof out->max_classes)) return false;
    if (!read_bytes(cursor, end, &out->max_methods, sizeof out->max_methods)) return false;
    if (!read_bytes(cursor, end, &out->build_id, sizeof out->build_id)) return false;
    if (!read_bytes(cursor, end, &out->byte_order, sizeof out->byte_order)) return false;
    return true;
}

bool diamond_cache_file_is_cache(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return false;
    char magic[DIAMOND_CACHE_MAGIC_LENGTH];
    const bool is_cache = fread(magic, 1, sizeof magic, file) == sizeof magic &&
        memcmp(magic, DIAMOND_CACHE_MAGIC, DIAMOND_CACHE_MAGIC_LENGTH) == 0;
    fclose(file);
    return is_cache;
}

bool diamond_program_read_cache_file(const char *path,
        const uint8_t source_hash[32], DiamondProgram *program,
        const DiamondProgram *prelude_template) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    const long file_size = ftell(file);
    if (file_size < 0) { fclose(file); return false; }
    if (fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    uint8_t *buffer = malloc((size_t)file_size);
    if (buffer == nullptr) { fclose(file); return false; }
    const size_t read_count = fread(buffer, 1, (size_t)file_size, file);
    fclose(file);
    if (read_count != (size_t)file_size) { free(buffer); return false; }

    const uint8_t *cursor = buffer;
    const uint8_t *end = buffer + (size_t)file_size;
    const uint8_t *magic = take(&cursor, end, DIAMOND_CACHE_MAGIC_LENGTH);
    if (magic == nullptr || memcmp(magic, DIAMOND_CACHE_MAGIC, DIAMOND_CACHE_MAGIC_LENGTH) != 0) {
        free(buffer);
        return false;
    }
    DiamondCacheFingerprint stored_fingerprint;
    if (!read_cache_fingerprint(&cursor, end, &stored_fingerprint)) {
        free(buffer);
        return false;
    }
    const DiamondCacheFingerprint current_fingerprint = diamond_cache_fingerprint();
    if (memcmp(&stored_fingerprint, &current_fingerprint, sizeof stored_fingerprint) != 0) {
        free(buffer);
        return false;
    }
    const uint8_t *stored_hash = take(&cursor, end, 32);
    if (stored_hash == nullptr || memcmp(stored_hash, source_hash, 32) != 0) {
        free(buffer);
        return false;
    }
    const uint8_t *stored_checksum_bytes = take(&cursor, end, sizeof(uint64_t));
    if (stored_checksum_bytes == nullptr) {
        free(buffer);
        return false;
    }
    uint64_t stored_checksum;
    memcpy(&stored_checksum, stored_checksum_bytes, sizeof stored_checksum);
    if (stored_checksum != body_checksum(cursor, (size_t)(end - cursor))) {
        free(buffer);
        return false;
    }
    bool ok = false;
    uint64_t skip = 0;
    const uint8_t *skip_bytes = take(&cursor, end, sizeof skip);
    if (skip_bytes != nullptr) {
        memcpy(&skip, skip_bytes, sizeof skip);
        /* Functions were left out only because they are the template's own,
         * so the reader must hold exactly that template. */
        if (skip == 0 || (prelude_template != nullptr &&
                          skip == (uint64_t)prelude_template->function_count))
            ok = read_program_body(cursor, (size_t)(end - cursor), program,
                prelude_template, (size_t)skip);
    }
    free(buffer);
    if (ok) ok = program_is_valid(program);
    if (!ok) {
        /* A failed or partial read leaves `program` half-populated; hand
         * the caller back the fresh, empty program it passed in. */
        diamond_program_free(program);
        diamond_program_init(program);
    }
    return ok;
}

/* True when `program` carries `template`'s functions unchanged as its first
 * template->function_count entries -- the precondition for leaving them out
 * of a cache file and rebuilding them from the template on read. Anything
 * that differs (a post-compile pass touching a prelude function, a program
 * compiled without the template) just means the full program is written. */
static bool functions_identical(const DiamondFunction *a, const DiamondFunction *b) {
    if (a->code_count != b->code_count || a->constant_count != b->constant_count ||
        a->string_count != b->string_count || a->type_set_count != b->type_set_count)
        return false;
    DiamondFunction left = *a, right = *b;
    left.code = right.code = nullptr;
    left.lines = right.lines = nullptr;
    left.columns = right.columns = nullptr;
    left.constants = right.constants = nullptr;
    left.strings = right.strings = nullptr;
    left.type_sets = right.type_sets = nullptr;
    left.jit_code = right.jit_code = nullptr;
    left.code_capacity = right.code_capacity = 0;
    left.constant_capacity = right.constant_capacity = 0;
    left.string_capacity = right.string_capacity = 0;
    left.type_set_capacity = right.type_set_capacity = 0;
    left.jit_code_size = right.jit_code_size = 0;
    left.jit_call_count = right.jit_call_count = 0;
    left.jit_ineligible = right.jit_ineligible = false;
    if (memcmp(&left, &right, sizeof left) != 0) return false;
    if (a->code_count > 0 &&
        (memcmp(a->code, b->code, a->code_count * sizeof *a->code) != 0 ||
         memcmp(a->lines, b->lines, a->code_count * sizeof *a->lines) != 0 ||
         memcmp(a->columns, b->columns, a->code_count * sizeof *a->columns) != 0))
        return false;
    if (a->constant_count > 0 &&
        memcmp(a->constants, b->constants, a->constant_count * sizeof *a->constants) != 0)
        return false;
    for (size_t index = 0; index < a->string_count; index++)
        if (a->strings[index].length != b->strings[index].length ||
            memcmp(a->strings[index].chars, b->strings[index].chars, a->strings[index].length) != 0)
            return false;
    if (a->type_set_count > 0 &&
        memcmp(a->type_sets, b->type_sets, a->type_set_count * sizeof *a->type_sets) != 0)
        return false;
    return true;
}

static size_t shared_prelude_function_count(const DiamondProgram *program,
        const DiamondProgram *template) {
    if (template == nullptr || template->function_count == 0 ||
        program->function_count < template->function_count)
        return 0;
    for (size_t index = 0; index < template->function_count; index++)
        if (!functions_identical(program->functions[index], template->functions[index]))
            return 0;
    return template->function_count;
}

void diamond_program_write_cache_file(const char *path,
        const uint8_t source_hash[32], const DiamondProgram *program,
        const DiamondProgram *prelude_template) {
    const size_t path_length = strlen(path);
    /* mkstemp needs its own writable buffer ending in a literal
     * "XXXXXX" (it overwrites those six characters in place), in the
     * same directory as `path` -- rename() is only atomic within one
     * directory/filesystem. Mirrors src/main.c's own write_compiled_
     * program_to_temp (diamond build) doing the identical "unique temp
     * file next to the real destination" thing for the identical
     * concurrent-writer-safety reason. */
    char *temp_path = malloc(path_length + 8);
    if (temp_path == nullptr) return;
    memcpy(temp_path, path, path_length);
    memcpy(temp_path + path_length, ".XXXXXX", 8);
    const int fd = mkstemp(temp_path);
    if (fd < 0) {
        free(temp_path);
        return;
    }
    FILE *file = fdopen(fd, "wb");
    if (file == nullptr) {
        close(fd);
        unlink(temp_path);
        free(temp_path);
        return;
    }
    const DiamondCacheFingerprint fingerprint = diamond_cache_fingerprint();
    char *body = nullptr;
    size_t body_size = 0;
    FILE *body_stream = open_memstream(&body, &body_size);
    const uint64_t skip = (uint64_t)shared_prelude_function_count(program, prelude_template);
    bool ok = body_stream != nullptr && write_all(body_stream, &skip, sizeof skip) &&
        write_program_body(program, (size_t)skip, body_stream);
    if (body_stream != nullptr && fclose(body_stream) != 0) ok = false;
    const uint64_t checksum = ok ? body_checksum((const uint8_t *)body, body_size) : 0;
    ok = ok && write_all(file, DIAMOND_CACHE_MAGIC, DIAMOND_CACHE_MAGIC_LENGTH) &&
        write_cache_fingerprint(file, &fingerprint) &&
        write_all(file, source_hash, 32) &&
        write_all(file, &checksum, sizeof checksum) &&
        write_all(file, body, body_size);
    free(body);
    if (fclose(file) != 0) ok = false;
    if (ok) {
        if (rename(temp_path, path) != 0) unlink(temp_path);
    } else {
        unlink(temp_path);
    }
    free(temp_path);
}
