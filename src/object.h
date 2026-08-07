#ifndef DIAMOND_OBJECT_H
#define DIAMOND_OBJECT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "value.h"

typedef enum DiamondObjectKind : uint8_t {
    DIAMOND_OBJECT_STRING,
    DIAMOND_OBJECT_INSTANCE,
    DIAMOND_OBJECT_ARRAY,
    DIAMOND_OBJECT_HASH,
    DIAMOND_OBJECT_CLOSURE,
    DIAMOND_OBJECT_CELL,
    DIAMOND_OBJECT_FIBER,
    DIAMOND_OBJECT_FILE,
    DIAMOND_OBJECT_LISTENER,
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
typedef struct DiamondShape DiamondShape;
typedef struct DiamondTypeSet DiamondTypeSet;
typedef struct DiamondInterface DiamondInterface;

enum { DIAMOND_BOUND_TYPE_NODES=16,DIAMOND_BOUND_TYPE_MEMBERS=8 };
typedef struct DiamondBoundTypeMember {
    uint8_t id;
    uint8_t argument_node;
    uint8_t second_argument_node;
} DiamondBoundTypeMember;
typedef struct DiamondBoundTypeNode {
    DiamondBoundTypeMember members[DIAMOND_BOUND_TYPE_MEMBERS];
    uint8_t count;
} DiamondBoundTypeNode;
typedef struct DiamondTypeBinding {
    DiamondBoundTypeNode nodes[DIAMOND_BOUND_TYPE_NODES];
    uint8_t node_count;
} DiamondTypeBinding;

typedef struct DiamondInstance {
    DiamondObject object;
    const DiamondClass *class;
    const DiamondShape *shape;
    size_t field_count;
    DiamondValue fields[];
} DiamondInstance;

typedef struct DiamondArray {
    DiamondObject object;
    size_t count;
    size_t capacity;
    struct {
        const DiamondTypeSet *type_sets;
        size_t type_set_count;
        uint8_t set_index;
        const DiamondClass *classes;
        size_t class_count;
        const DiamondInterface *interfaces;
        size_t interface_count;
        DiamondTypeBinding *type_variable_bindings;
        uint8_t type_variable_count;
    } constraints[4];
    uint8_t constraint_count;
    DiamondValue *values;
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
    struct {
        const DiamondTypeSet *type_sets;
        size_t type_set_count;
        uint8_t key_set;
        uint8_t value_set;
        const DiamondClass *classes;
        size_t class_count;
        const DiamondInterface *interfaces;
        size_t interface_count;
        DiamondTypeBinding *type_variable_bindings;
        uint8_t type_variable_count;
    } constraints[4];
    uint8_t constraint_count;
} DiamondHash;

typedef struct DiamondClosure {
    DiamondObject object;
    uint8_t function_index;
    uint8_t capture_count;
    DiamondValue captures[16];
} DiamondClosure;

typedef struct DiamondCell {
    DiamondObject object;
    DiamondValue value;
} DiamondCell;

typedef struct DiamondFiber DiamondFiber;

typedef struct DiamondFiberHandle {
    DiamondObject object;
    DiamondFiber *fiber;
} DiamondFiberHandle;

typedef struct DiamondFileHandle {
    DiamondObject object;
    FILE *stream;
} DiamondFileHandle;

typedef struct DiamondListenerHandle {
    DiamondObject object;
    int fd;
} DiamondListenerHandle;

#endif
