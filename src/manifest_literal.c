#include "manifest_literal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct LiteralParser {
    const char *source;
    size_t position;
    unsigned depth;
    char *error;
    size_t error_size;
} LiteralParser;

static bool fail(LiteralParser *parser, const char *reason) {
    size_t line = 1;
    for (size_t i = 0; i < parser->position; i++)
        if (parser->source[i] == '\n') line++;
    (void)snprintf(parser->error, parser->error_size, "line %zu: %s", line, reason);
    return false;
}

static void skip_space(LiteralParser *parser) {
    while (isspace((unsigned char)parser->source[parser->position])) parser->position++;
}

static bool parse_hex_quad(LiteralParser *parser, unsigned *out) {
    unsigned code = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)parser->source[parser->position];
        if (!isxdigit(c)) return fail(parser, "invalid Unicode escape");
        code = code * 16 + (unsigned)(isdigit(c) ? c - '0' :
            (unsigned char)tolower(c) - 'a' + 10);
        parser->position++;
    }
    *out = code;
    return true;
}

static size_t encode_utf8(unsigned code, char *out) {
    if (code < 0x80) { out[0] = (char)code; return 1; }
    if (code < 0x800) {
        out[0] = (char)(0xc0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3f));
        return 2;
    }
    if (code < 0x10000) {
        out[0] = (char)(0xe0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (code >> 18));
    out[1] = (char)(0x80 | ((code >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((code >> 6) & 0x3f));
    out[3] = (char)(0x80 | (code & 0x3f));
    return 4;
}

void diamond_manifest_free(DiamondManifestValue *value) {
    while (value != NULL) {
        DiamondManifestValue *next = value->next;
        diamond_manifest_free(value->children);
        free(value->string);
        free(value->key);
        free(value);
        value = next;
    }
}

static char *parse_string(LiteralParser *parser) {
    size_t start = ++parser->position;
    size_t capacity = strlen(parser->source + start) + 1;
    char *result = malloc(capacity);
    if (result == NULL) { fail(parser, "out of memory"); return NULL; }
    size_t used = 0;
    for (;;) {
        unsigned char c = (unsigned char)parser->source[parser->position];
        if (c == '\0' || c < 0x20) {
            fail(parser, "unterminated or invalid String"); break;
        }
        parser->position++;
        if (c == '"') { result[used] = '\0'; return result; }
        if (c == '#' && parser->source[parser->position] == '{') {
            fail(parser, "String interpolation is not allowed"); break;
        }
        if (c == '\\') {
            c = (unsigned char)parser->source[parser->position];
            if (c == '\0') { fail(parser, "unterminated String escape"); break; }
            parser->position++;
            switch (c) {
                case '"': case '\\': case '/': break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    unsigned code;
                    if (!parse_hex_quad(parser, &code)) { free(result); return NULL; }
                    if (code >= 0xd800 && code <= 0xdbff) {
                        if (parser->source[parser->position] != '\\' ||
                            parser->source[parser->position + 1] != 'u') {
                            fail(parser, "missing low Unicode surrogate");
                            free(result); return NULL;
                        }
                        parser->position += 2;
                        unsigned low;
                        if (!parse_hex_quad(parser, &low)) { free(result); return NULL; }
                        if (low < 0xdc00 || low > 0xdfff) {
                            fail(parser, "invalid low Unicode surrogate");
                            free(result); return NULL;
                        }
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    } else if (code >= 0xdc00 && code <= 0xdfff) {
                        fail(parser, "unexpected low Unicode surrogate");
                        free(result); return NULL;
                    }
                    if (code == 0) {
                        fail(parser, "NUL in String");
                        free(result); return NULL;
                    }
                    used += encode_utf8(code, result + used);
                    continue;
                }
                default: fail(parser, "invalid String escape"); free(result); return NULL;
            }
        }
        if (c == '\0') { fail(parser, "NUL in String"); break; }
        result[used++] = (char)c;
    }
    free(result);
    return NULL;
}

static DiamondManifestValue *parse_value(LiteralParser *parser);

static DiamondManifestValue *parse_collection(LiteralParser *parser, bool hash) {
    if (++parser->depth > 32) { fail(parser, "literal nesting exceeds 32 levels"); return NULL; }
    DiamondManifestValue *value = calloc(1, sizeof *value);
    if (value == NULL) { fail(parser, "out of memory"); return NULL; }
    value->kind = hash ? DIAMOND_MANIFEST_HASH : DIAMOND_MANIFEST_ARRAY;
    parser->position++;
    skip_space(parser);
    DiamondManifestValue **tail = &value->children;
    if (parser->source[parser->position] == (hash ? '}' : ']')) {
        parser->position++;
        parser->depth--;
        return value;
    }
    for (;;) {
        char *key = NULL;
        if (hash) {
            if (parser->source[parser->position] != '"') {
                fail(parser, "Hash keys must be Strings"); goto error;
            }
            key = parse_string(parser);
            if (key == NULL) goto error;
            for (DiamondManifestValue *item = value->children; item != NULL; item = item->next) {
                if (strcmp(item->key, key) == 0) {
                    free(key);
                    fail(parser, "duplicate Hash key"); goto error;
                }
            }
            skip_space(parser);
            if (parser->source[parser->position] != ':') {
                free(key);
                fail(parser, "expected ':' after Hash key"); goto error;
            }
            parser->position++;
        }
        DiamondManifestValue *child = parse_value(parser);
        if (child == NULL) { free(key); goto error; }
        child->key = key;
        *tail = child;
        tail = &child->next;
        skip_space(parser);
        char c = parser->source[parser->position];
        if (c == (hash ? '}' : ']')) {
            parser->position++;
            parser->depth--;
            return value;
        }
        if (c != ',') { fail(parser, "expected ',' or closing delimiter"); goto error; }
        parser->position++;
        skip_space(parser);
        if (parser->source[parser->position] == (hash ? '}' : ']')) {
            parser->position++;
            parser->depth--;
            return value;
        }
    }
error:
    diamond_manifest_free(value);
    return NULL;
}

static DiamondManifestValue *parse_value(LiteralParser *parser) {
    skip_space(parser);
    char c = parser->source[parser->position];
    if (c == '{') return parse_collection(parser, true);
    if (c == '[') return parse_collection(parser, false);
    DiamondManifestValue *value = calloc(1, sizeof *value);
    if (value == NULL) { fail(parser, "out of memory"); return NULL; }
    if (c == '"') {
        value->kind = DIAMOND_MANIFEST_STRING;
        value->string = parse_string(parser);
        if (value->string != NULL) return value;
    } else if (c == '-' || isdigit((unsigned char)c)) {
        size_t start = parser->position;
        if (c == '-') parser->position++;
        size_t digits = parser->position;
        while (isdigit((unsigned char)parser->source[parser->position])) parser->position++;
        if (digits != parser->position) {
            size_t length = parser->position - start;
            value->string = malloc(length + 1);
            if (value->string == NULL) {
                fail(parser, "out of memory");
                free(value);
                return NULL;
            }
            memcpy(value->string, parser->source + start, length);
            value->string[length] = '\0';
            value->kind = DIAMOND_MANIFEST_INTEGER;
            return value;
        }
        fail(parser, "expected integer");
    } else {
        const char *keywords[] = {"true", "false", "nil"};
        for (size_t i = 0; i < 3; i++) {
            size_t length = strlen(keywords[i]);
            if (strlen(parser->source + parser->position) >= length &&
                strncmp(parser->source + parser->position, keywords[i], length) == 0) {
                parser->position += length;
                if (i < 2) {
                    value->kind = DIAMOND_MANIFEST_BOOLEAN;
                    value->string = malloc(length + 1);
                    if (value->string == NULL) {
                        fail(parser, "out of memory");
                        free(value);
                        return NULL;
                    }
                    memcpy(value->string, keywords[i], length + 1);
                }
                return value;
            }
        }
        fail(parser, "expected data literal");
    }
    free(value);
    return NULL;
}

DiamondManifestValue *diamond_manifest_parse(const char *source, char *error,
                                              size_t error_size) {
    if (source == NULL || error == NULL || error_size == 0) return NULL;
    LiteralParser parser = {.source = source, .error = error, .error_size = error_size};
    if (strlen(source) > 1024 * 1024) {
        fail(&parser, "metadata exceeds 1 MiB");
        return NULL;
    }
    skip_space(&parser);
    if (source[parser.position] != '{') { fail(&parser, "expected Hash literal"); return NULL; }
    DiamondManifestValue *root = parse_collection(&parser, true);
    if (root == NULL) return NULL;
    skip_space(&parser);
    if (source[parser.position] != '\0') {
        fail(&parser, "unexpected content after Hash literal");
        diamond_manifest_free(root);
        return NULL;
    }
    error[0] = '\0';
    return root;
}

const DiamondManifestValue *diamond_manifest_get(const DiamondManifestValue *hash,
                                                 const char *key) {
    if (hash == NULL || hash->kind != DIAMOND_MANIFEST_HASH) return NULL;
    for (const DiamondManifestValue *item = hash->children; item != NULL; item = item->next)
        if (strcmp(item->key, key) == 0) return item;
    return NULL;
}

bool diamond_manifest_get_string(const DiamondManifestValue *hash, const char *key,
                                 char *out, size_t out_size) {
    const DiamondManifestValue *value = diamond_manifest_get(hash, key);
    if (value == NULL || value->kind != DIAMOND_MANIFEST_STRING) return false;
    size_t length = strlen(value->string);
    if (length >= out_size) return false;
    memcpy(out, value->string, length + 1);
    return true;
}

bool diamond_manifest_get_u64(const DiamondManifestValue *hash, const char *key,
                              uint64_t *out) {
    const DiamondManifestValue *value = diamond_manifest_get(hash, key);
    if (value == NULL || value->kind != DIAMOND_MANIFEST_INTEGER ||
        value->string[0] == '-') return false;
    uint64_t result = 0;
    for (const char *cursor = value->string; *cursor != '\0'; cursor++) {
        unsigned digit = (unsigned)(*cursor - '0');
        if (result > (UINT64_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    *out = result;
    return true;
}

bool diamond_manifest_get_bool(const DiamondManifestValue *hash, const char *key,
                               bool *out) {
    const DiamondManifestValue *value = diamond_manifest_get(hash, key);
    if (value == NULL || value->kind != DIAMOND_MANIFEST_BOOLEAN) return false;
    *out = strcmp(value->string, "true") == 0;
    return true;
}
