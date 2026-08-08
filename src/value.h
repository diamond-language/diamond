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
} DiamondValueKind;

typedef struct DiamondObject DiamondObject;

typedef struct DiamondValue {
    DiamondValueKind kind;
    union {
        bool boolean;
        int64_t integer;
        double real;
        DiamondObject *object;
    } as;
} DiamondValue;

#define DIAMOND_NIL ((DiamondValue){.kind = DIAMOND_VALUE_NIL})
#define DIAMOND_BOOL(value_) \
    ((DiamondValue){.kind = DIAMOND_VALUE_BOOL, .as.boolean = (value_)})
#define DIAMOND_INT(value_) \
    ((DiamondValue){.kind = DIAMOND_VALUE_INT, .as.integer = (value_)})
#define DIAMOND_FLOAT(value_) \
    ((DiamondValue){.kind = DIAMOND_VALUE_FLOAT, .as.real = (value_)})
#define DIAMOND_OBJECT(value_) \
    ((DiamondValue){.kind = DIAMOND_VALUE_OBJECT, .as.object = (DiamondObject *)(value_)})

void diamond_value_print(DiamondValue value);
void diamond_value_fprint(FILE *stream, DiamondValue value);

#endif
