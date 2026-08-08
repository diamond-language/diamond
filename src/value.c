#include "value.h"

#include "vm.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

/* Pragmatic %.15g-based format, not a shortest-round-trip algorithm
 * (Grisu/Ryu-quality) - a documented simplification. Forces a trailing
 * .0 for whole-number results so a Float never prints indistinguishably
 * from an Int. */
static void fprint_float(FILE *stream, double real) {
    if (isnan(real)) { fputs("NaN", stream); return; }
    if (isinf(real)) { fputs(real < 0 ? "-Infinity" : "Infinity", stream); return; }
    char buffer[32];
    const int length = snprintf(buffer, sizeof buffer, "%.15g", real);
    bool has_marker = false;
    for (int index = 0; index < length; index++) {
        if (buffer[index] == '.' || buffer[index] == 'e' || buffer[index] == 'E') {
            has_marker = true;
            break;
        }
    }
    fputs(buffer, stream);
    if (!has_marker) fputs(".0", stream);
}

void diamond_value_fprint(FILE *stream, DiamondValue value) {
    switch (value.kind) {
        case DIAMOND_VALUE_NIL:
            fputs("nil", stream);
            break;
        case DIAMOND_VALUE_BOOL:
            fputs(value.as.boolean ? "true" : "false", stream);
            break;
        case DIAMOND_VALUE_INT:
            fprintf(stream, "%" PRId64, value.as.integer);
            break;
        case DIAMOND_VALUE_FLOAT:
            fprint_float(stream, value.as.real);
            break;
        case DIAMOND_VALUE_OBJECT: {
            const DiamondString *string = (const DiamondString *)value.as.object;
            if (value.as.object->kind == DIAMOND_OBJECT_STRING) {
                fprintf(stream, "%.*s", (int)string->length, string->chars);
            } else if(value.as.object->kind==DIAMOND_OBJECT_ARRAY) {
                const DiamondArray *array=(const DiamondArray *)value.as.object;
                fputc('[',stream);
                for(size_t index=0;index<array->count;index++) {
                    if(index>0) fputs(", ",stream);
                    diamond_value_fprint(stream,array->values[index]);
                }
                fputc(']',stream);
            } else if(value.as.object->kind==DIAMOND_OBJECT_HASH) {
                const DiamondHash *hash=(const DiamondHash *)value.as.object;
                fputc('{',stream);
                for(size_t index=0;index<hash->count;index++) {
                    if(index>0) fputs(", ",stream);
                    diamond_value_fprint(stream,hash->entries[index].key);
                    fputs(": ",stream);
                    diamond_value_fprint(stream,hash->entries[index].value);
                }
                fputc('}',stream);
            } else if(value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
                fprintf(stream,"#<%s>",instance->class->name);
            } else {
                fputs("#<Closure>",stream);
            }
            break;
        }
    }
}

void diamond_value_print(DiamondValue value) {
    diamond_value_fprint(stdout, value);
}
