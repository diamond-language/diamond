#ifndef DIAMOND_LSP_JSON_H
#define DIAMOND_LSP_JSON_H

#include <stdbool.h>
#include <stddef.h>

/* A deliberately minimal JSON implementation, scoped to exactly what LSP
 * messages need (objects, arrays, strings, numbers, booleans, null) --
 * not a general-purpose library. Every JsonValue is individually
 * heap-allocated and owns its children; json_free walks the tree.
 * Numbers are stored as double: every integer an LSP message actually
 * carries (request ids, line/character positions) fits exactly, and a
 * double avoids needing a separate integer/float distinction the
 * protocol itself doesn't make. */

typedef enum JsonKind {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonKind;

typedef struct JsonValue JsonValue;

typedef struct JsonMember {
    char *key;
    JsonValue *value;
} JsonMember;

struct JsonValue {
    JsonKind kind;
    union {
        bool boolean;
        double number;
        struct {
            char *chars;
            size_t length;
        } string;
        struct {
            JsonValue **items;
            size_t count;
            size_t capacity;
        } array;
        struct {
            JsonMember *members;
            size_t count;
            size_t capacity;
        } object;
    } as;
};

/* Constructors. Each returns nullptr only on allocation failure -- every
 * call site should check before use, the same discipline src/vm.c's own
 * allocate_array/allocate_string follow. */
JsonValue *json_null(void);
JsonValue *json_bool(bool value);
JsonValue *json_number(double value);
JsonValue *json_string(const char *chars, size_t length);
JsonValue *json_string_z(const char *chars);
JsonValue *json_array(void);
JsonValue *json_object(void);

/* Takes ownership of `item`/`value` on success; on failure (allocation
 * failure growing the backing array) the caller still owns it and must
 * free it. */
bool json_array_push(JsonValue *array, JsonValue *item);
bool json_object_set(JsonValue *object, const char *key, JsonValue *value);

/* Lookups. Return nullptr/false when absent or of the wrong kind --
 * callers treat a missing optional field the same as an explicit null. */
const JsonValue *json_object_get(const JsonValue *object, const char *key);
bool json_as_string(const JsonValue *value, const char **chars, size_t *length);
bool json_as_number(const JsonValue *value, double *out);
bool json_as_bool(const JsonValue *value, bool *out);

/* Parses exactly `length` bytes starting at `source` (no implicit
 * null-termination requirement, since RPC bodies are read by exact byte
 * count). Returns nullptr on malformed input; if `error` is non-null,
 * *error is set to a static description. */
JsonValue *json_parse(const char *source, size_t length, const char **error);

void json_free(JsonValue *value);

/* Serializes into a freshly malloc'd, null-terminated buffer the caller
 * frees. *out_length is the byte length excluding the trailing '\0' --
 * exactly what rpc.c needs for the Content-Length header, so callers
 * never have to strlen() the result back out. */
char *json_serialize(const JsonValue *value, size_t *out_length);

#endif
