#ifndef DIAMOND_VALUE_H
#define DIAMOND_VALUE_H

#include <stdint.h>
#include <stdio.h>

typedef enum DiamondValueKind : uint8_t {
    DIAMOND_VALUE_NIL,
    DIAMOND_VALUE_BOOL,
    DIAMOND_VALUE_INT,
    DIAMOND_VALUE_FLOAT,
    DIAMOND_VALUE_OBJECT,
    /* Internal sparse-call sentinel; consumed at frame entry. */
    DIAMOND_VALUE_UNDEFINED,
    /* A compile-time class index (into the ambient chunk's `classes[]`),
     * not a heap object -- carries no pointer, so GC's mark_value already
     * skips it exactly like INT/FLOAT/BOOL/NIL, and it costs no struct
     * growth (fits in the union alongside `bool`). Deliberately narrow:
     * this can only appear in register 0 (`self`) inside a class-owned
     * singleton method and as the receiver of `self.foo(...)` dispatch
     * there -- see docs/design.md's "classes are not first-class runtime
     * values" section for the full scope of what this does and doesn't
     * enable. class_index is only meaningful relative to the chunk it
     * came from, the same caveat NEW/REDEFINE_METHOD/DEFINE_METHOD's own
     * class_index bytecode operand already has. */
    DIAMOND_VALUE_CLASS,
} DiamondValueKind;

typedef struct DiamondObject DiamondObject;

typedef struct DiamondValue {
    DiamondValueKind kind;
    union {
        bool boolean;
        int64_t integer;
        double real;
        DiamondObject *object;
        uint8_t class_index;
    } as;
} DiamondValue;

#define DIAMOND_NIL ((DiamondValue){.kind = DIAMOND_VALUE_NIL})
#define DIAMOND_UNDEFINED ((DiamondValue){.kind = DIAMOND_VALUE_UNDEFINED})
#define DIAMOND_BOOL(value_) \
    ((DiamondValue){.kind = DIAMOND_VALUE_BOOL, .as.boolean = (value_)})
#define DIAMOND_INT(value_) \
    ((DiamondValue){.kind = DIAMOND_VALUE_INT, .as.integer = (value_)})
#define DIAMOND_FLOAT(value_) \
    ((DiamondValue){.kind = DIAMOND_VALUE_FLOAT, .as.real = (value_)})
#define DIAMOND_OBJECT(value_) \
    ((DiamondValue){.kind = DIAMOND_VALUE_OBJECT, .as.object = (DiamondObject *)(value_)})
#define DIAMOND_CLASS(value_) \
    ((DiamondValue){.kind = DIAMOND_VALUE_CLASS, .as.class_index = (uint8_t)(value_)})

void diamond_value_print(DiamondValue value);
void diamond_value_fprint(FILE *stream, DiamondValue value);

#endif
