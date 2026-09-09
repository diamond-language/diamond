/* See bignum.c's own identical comment: needed transitively for vm.h's
 * <ucontext.h> use, only under musl (docs/roadmap.md's "Portability").
 * Must precede value.h itself, not just vm.h below -- value.h's own
 * <stdint.h> is this file's first libc header either way. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#include "value.h"

#include "vm.h"
#include "bignum.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shortest decimal that round-trips back to the exact same bit pattern:
 * try increasing precision until re-parsing matches (17 significant
 * digits is provably always sufficient for any IEEE-754 double, so
 * this always terminates) - not a full Grisu/Ryu digit-generation
 * algorithm, but the same observable guarantee for a cold path where
 * an O(17) snprintf+strtod search is an acceptable tradeoff. Compares
 * bit patterns rather than `==` so -0.0 and 0.0 are told apart. Forces
 * a trailing .0 for whole-number results so a Float never prints
 * indistinguishably from an Int.
 *
 * %g switches to scientific notation whenever its precision is <= the
 * value's own decimal exponent - starting the search at precision 1
 * unconditionally means a round value like 10.0 hits that threshold
 * immediately ("%.1g" -> "1e+01", which round-trips exactly and so
 * would otherwise end the search right there). Probe the exponent
 * first via "%.0e" (not log10 - log10(10.0) can land a hair under 1.0
 * and round the wrong way) and start the search at a precision that
 * keeps %g in fixed-point mode for that magnitude, so the search only
 * has to decide how many digits are needed, never fight %g over
 * notation. Values with large enough exponents still fall through to
 * scientific notation once the (capped) starting precision itself
 * can't outrun the exponent, same as every other language's Float
 * formatting. */
static void fprint_float(FILE *stream, double real) {
    if (isnan(real)) { fputs("NaN", stream); return; }
    if (isinf(real)) { fputs(real < 0 ? "-Infinity" : "Infinity", stream); return; }
    char buffer[32];
    int length = 0;
    uint64_t real_bits;
    memcpy(&real_bits, &real, sizeof real_bits);
    char probe[32];
    snprintf(probe, sizeof probe, "%.0e", real);
    const char *exponent_marker = strchr(probe, 'e');
    const int exponent = exponent_marker ? atoi(exponent_marker + 1) : 0;
    /* Only bump the starting precision when it can actually keep %g in
     * fixed-point mode (exponent 1..16): past that, %g would use
     * scientific notation at every precision from 1 to 17 anyway (the
     * exponent alone already exceeds the max precision), so starting
     * at 1 costs nothing and lets the search find a genuinely shorter
     * scientific form when one exists (e.g. exactly 1e+70, rather than
     * needlessly settling for a longer 17-digit mantissa). */
    const int start_precision = (exponent >= 1 && exponent <= 16) ? exponent + 1 : 1;
    for (int precision = start_precision; precision <= 17; precision++) {
        length = snprintf(buffer, sizeof buffer, "%.*g", precision, real);
        if (length < 0 || (size_t)length >= sizeof buffer) continue;
        char *end = nullptr;
        const double parsed = strtod(buffer, &end);
        uint64_t parsed_bits;
        memcpy(&parsed_bits, &parsed, sizeof parsed_bits);
        if (end != buffer && parsed_bits == real_bits) break;
    }
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
        case DIAMOND_VALUE_UNDEFINED:
            fputs("<undefined>",stream);
            break;
        case DIAMOND_VALUE_CLASS:
            /* No chunk/program context reaches this low-level, value-only
             * print path, so this can't resolve class_index back to the
             * class's real name (unlike DIAMOND_OBJECT_INSTANCE, a Class
             * value stores no self-contained data of its own to print
             * from) -- printing a Class value is a diagnostic nicety, not
             * something normal program behavior depends on, so the index
             * alone is an acceptable, deliberately narrow scope cut. */
            fprintf(stream, "#<Class:%u>", value.as.class_index);
            break;
        case DIAMOND_VALUE_OBJECT: {
            const DiamondString *string = (const DiamondString *)value.as.object;
            if (value.as.object->kind == DIAMOND_OBJECT_STRING) {
                fprintf(stream, "%.*s", (int)string->length, string->chars);
            } else if(value.as.object->kind==DIAMOND_OBJECT_SYMBOL) {
                const DiamondSymbol *symbol=(const DiamondSymbol *)value.as.object;
                fprintf(stream,"%.*s",(int)symbol->length,symbol->chars);
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
            } else if(value.as.object->kind==DIAMOND_OBJECT_BIGNUM) {
                const DiamondBignum *bignum=(const DiamondBignum *)value.as.object;
                char *digits=malloc(diamond_bignum_string_length(bignum));
                if(digits!=nullptr) {
                    const size_t length=diamond_bignum_to_string(bignum,digits,
                        diamond_bignum_string_length(bignum));
                    fwrite(digits,1,length,stream);
                    free(digits);
                }
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
