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

#endif
