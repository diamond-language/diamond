#ifndef DIAMOND_OBJECT_H
#define DIAMOND_OBJECT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <reginold.h>

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
    DIAMOND_OBJECT_BIGNUM,
    DIAMOND_OBJECT_SYMBOL,
    DIAMOND_OBJECT_REGEXP,
    DIAMOND_OBJECT_PROGRAM_BUILDER,
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

/* An interned-free, content-compared name: equality and hashing are by
 * byte content (exactly like DiamondString), and lifetime is ordinary GC,
 * not permanent -- see docs/roadmap.md for why Symbol was scoped this way
 * rather than as Ruby-style pointer-equal singletons. Deliberately its own
 * struct (not just DiamondString reused under a different kind) purely for
 * type clarity, even though the layout is identical. */
typedef struct DiamondSymbol {
    DiamondObject object;
    size_t length;
    char chars[];
} DiamondSymbol;

/* An Int that overflowed int64_t. Sign + magnitude, base 10^9 limbs
 * (each 0..999999999), little-endian (limbs[0] is least significant) --
 * chosen over a binary base specifically to make decimal stringification
 * (the operation a bignum actually exercises on every puts/interpolation/
 * error message) close to trivial, at an acceptable cost to raw
 * arithmetic throughput that doesn't matter on this cold path. Never
 * represents a value that fits in int64_t -- every operation that
 * produces a bignum result canonicalizes back to a plain DIAMOND_VALUE_INT
 * when it fits, so downstream code never has two representations of the
 * same value to worry about. */
typedef struct DiamondBignum {
    DiamondObject object;
    bool negative;
    size_t limb_count;
    uint32_t limbs[];
} DiamondBignum;

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
    uint64_t hash;
} DiamondHashEntry;

typedef struct DiamondHash {
    DiamondObject object;
    size_t count;
    size_t capacity;
    DiamondHashEntry *entries;
    size_t *buckets;
    size_t bucket_capacity;
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

/* A compiled reginold pattern. Unlike DiamondFileHandle/DiamondListenerHandle,
 * this owns no OS resource (fd/socket) -- just heap memory reginold itself
 * allocated -- so there's no explicit .close() method; GC-time
 * reginold_regex_free (see diamond_vm_collect's sweep loop) is sufficient. */
typedef struct DiamondRegexp {
    DiamondObject object;
    reginold_regex *handle;
} DiamondRegexp;

/* Forward-declared, not included: DiamondProgram is defined in compiler.h,
 * which itself includes vm.h (and so, transitively, this file) -- a
 * pointer to the incomplete type is all this struct needs. See
 * docs/roadmap.md's self-hosting Phase 1 entry for the full design: this
 * is the ProgramBuilder native bridge letting Diamond code construct and
 * run a DiamondProgram at runtime. Like DiamondFileHandle/
 * DiamondListenerHandle, no mark_object branch is needed -- the wrapped
 * DiamondProgram's own constants are restricted to scalar DiamondValues
 * (see ProgramBuilder#add_constant in vm.c), so nothing inside one ever
 * references another Diamond value. */
typedef struct DiamondProgram DiamondProgram;
typedef struct DiamondProgramBuilder {
    DiamondObject object;
    DiamondProgram *program;
} DiamondProgramBuilder;

#endif
