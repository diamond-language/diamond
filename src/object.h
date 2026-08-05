#ifndef DIAMOND_OBJECT_H
#define DIAMOND_OBJECT_H

#include <stddef.h>
#include <stdint.h>

#include "value.h"

typedef enum DiamondObjectKind : uint8_t {
    DIAMOND_OBJECT_STRING,
    DIAMOND_OBJECT_INSTANCE,
    DIAMOND_OBJECT_ARRAY,
    DIAMOND_OBJECT_HASH,
    DIAMOND_OBJECT_CLOSURE,
} DiamondObjectKind;

typedef struct DiamondObject {
    struct DiamondObject *next;
    DiamondObjectKind kind;
    bool marked;
} DiamondObject;

typedef struct DiamondString {
    DiamondObject object;
    size_t length;
    char chars[];
} DiamondString;

typedef struct DiamondClass DiamondClass;

typedef struct DiamondInstance {
    DiamondObject object;
    const DiamondClass *class;
    size_t field_count;
    DiamondValue fields[];
} DiamondInstance;

typedef struct DiamondArray {
    DiamondObject object;
    size_t count;
    DiamondValue values[];
} DiamondArray;

typedef struct DiamondHashEntry {
    DiamondValue key;
    DiamondValue value;
} DiamondHashEntry;

typedef struct DiamondHash {
    DiamondObject object;
    size_t count;
    size_t capacity;
    DiamondHashEntry *entries;
} DiamondHash;

typedef struct DiamondClosure {
    DiamondObject object;
    uint8_t function_index;
    uint8_t capture_count;
    DiamondValue captures[16];
} DiamondClosure;

#endif
