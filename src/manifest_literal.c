#include "manifest_literal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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
    (void)snprintf(parser->error, parser->error_size,
                   "line %zu: %s", line, reason);
    return false;
}

static void skip_space(LiteralParser *parser) {
    while (isspace((unsigned char)parser->source[parser->position]))
        parser->position++;
}

static bool parse_string(LiteralParser *parser) {
    if (parser->source[parser->position++] != '"') return fail(parser, "expected String");
    for (;;) {
        unsigned char c = (unsigned char)parser->source[parser->position++];
        if (c == '"') return true;
        if (c == '\0' || c < 0x20) {
            parser->position--;
            return fail(parser, "unterminated or invalid String");
        }
        if (c == '#' && parser->source[parser->position] == '{')
            return fail(parser, "String interpolation is not allowed");
        if (c != '\\') continue;
        c = (unsigned char)parser->source[parser->position];
        if (c == '\0') return fail(parser, "unterminated String escape");
        parser->position++;
        if (c == '"' || c == '\\' || c == '/' || c == 'n' || c == 'r' ||
            c == 't' || c == 'b' || c == 'f') continue;
        if (c == 'u') {
            for (unsigned i = 0; i < 4; i++) {
                if (!isxdigit((unsigned char)parser->source[parser->position]))
                    return fail(parser, "invalid Unicode escape");
                parser->position++;
            }
            continue;
        }
        return fail(parser, "invalid String escape");
    }
}

static bool parse_value(LiteralParser *parser);

static bool parse_collection(LiteralParser *parser, bool hash) {
    if (++parser->depth > 32) return fail(parser, "literal nesting exceeds 32 levels");
    parser->position++;
    skip_space(parser);
    if (parser->source[parser->position] == (hash ? '}' : ']')) {
        parser->position++;
        parser->depth--;
        return true;
    }
    for (;;) {
        if (hash) {
            if (parser->source[parser->position] != '"' || !parse_string(parser))
                return fail(parser, "Hash keys must be Strings");
            skip_space(parser);
            if (parser->source[parser->position] != ':')
                return fail(parser, "expected ':' after Hash key");
            parser->position++;
            skip_space(parser);
        }
        if (!parse_value(parser)) return false;
        skip_space(parser);
        char c = parser->source[parser->position];
        if (c == (hash ? '}' : ']')) {
            parser->position++;
            parser->depth--;
            return true;
        }
        if (c != ',') return fail(parser, "expected ',' or closing delimiter");
        parser->position++;
        skip_space(parser);
        if (parser->source[parser->position] == (hash ? '}' : ']')) {
            parser->position++;
            parser->depth--;
            return true;
        }
    }
}

static bool parse_value(LiteralParser *parser) {
    skip_space(parser);
    char c = parser->source[parser->position];
    if (c == '{') return parse_collection(parser, true);
    if (c == '[') return parse_collection(parser, false);
    if (c == '"') return parse_string(parser);
    if (c == '-' || isdigit((unsigned char)c)) {
        if (c == '-') parser->position++;
        size_t start = parser->position;
        while (isdigit((unsigned char)parser->source[parser->position]))
            parser->position++;
        if (start == parser->position) return fail(parser, "expected integer");
        return true;
    }
    const char *keywords[] = {"true", "false", "nil"};
    for (size_t i = 0; i < 3; i++) {
        size_t length = strlen(keywords[i]);
        if (strlen(parser->source + parser->position) >= length &&
            strncmp(parser->source + parser->position, keywords[i], length) == 0) {
            parser->position += length;
            return true;
        }
    }
    return fail(parser, "expected data literal");
}

bool diamond_manifest_literal_validate(const char *source, char *error,
                                       size_t error_size) {
    if (source == NULL || error == NULL || error_size == 0) return false;
    LiteralParser parser = {.source = source, .error = error,
                            .error_size = error_size};
    skip_space(&parser);
    if (source[parser.position] != '{') return fail(&parser, "expected Hash literal");
    if (!parse_collection(&parser, true)) return false;
    skip_space(&parser);
    if (source[parser.position] != '\0')
        return fail(&parser, "unexpected content after Hash literal");
    error[0] = '\0';
    return true;
}
