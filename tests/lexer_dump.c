/* Reference token dumper for the self-hosting Phase 2 differential
 * harness (tests/lexer_diff.sh): reads a file's raw content and prints
 * one line per token via diamond_lexer_next, in the exact format
 * selfhost/lexer_dump.di's Diamond-language lexer also prints, so a
 * plain `diff` between the two catches any divergence between the C
 * lexer and its Diamond-language port (selfhost/lexer.di). See
 * docs/roadmap.md's self-hosting Phase 2 entry. */
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>

static const char *kind_name(DiamondTokenKind kind) {
    switch (kind) {
        case DIAMOND_TOKEN_EOF: return "eof";
        case DIAMOND_TOKEN_ERROR: return "error";
        case DIAMOND_TOKEN_INTEGER: return "integer";
        case DIAMOND_TOKEN_FLOAT: return "float";
        case DIAMOND_TOKEN_STRING: return "string";
        case DIAMOND_TOKEN_SYMBOL: return "symbol";
        case DIAMOND_TOKEN_IDENTIFIER: return "identifier";
        case DIAMOND_TOKEN_INSTANCE_VARIABLE: return "instance_variable";
        case DIAMOND_TOKEN_CLASS_VARIABLE: return "class_variable";
        case DIAMOND_TOKEN_NEWLINE: return "newline";
        case DIAMOND_TOKEN_LEFT_PAREN: return "left_paren";
        case DIAMOND_TOKEN_RIGHT_PAREN: return "right_paren";
        case DIAMOND_TOKEN_LEFT_BRACKET: return "left_bracket";
        case DIAMOND_TOKEN_RIGHT_BRACKET: return "right_bracket";
        case DIAMOND_TOKEN_LEFT_BRACE: return "left_brace";
        case DIAMOND_TOKEN_RIGHT_BRACE: return "right_brace";
        case DIAMOND_TOKEN_COMMA: return "comma";
        case DIAMOND_TOKEN_DOT: return "dot";
        case DIAMOND_TOKEN_DOT_DOT: return "dot_dot";
        case DIAMOND_TOKEN_DOT_DOT_DOT: return "dot_dot_dot";
        case DIAMOND_TOKEN_COLON: return "colon";
        case DIAMOND_TOKEN_DOUBLE_COLON: return "double_colon";
        case DIAMOND_TOKEN_QUESTION: return "question";
        case DIAMOND_TOKEN_PIPE: return "pipe";
        case DIAMOND_TOKEN_AMPERSAND: return "ampersand";
        case DIAMOND_TOKEN_BANG: return "bang";
        case DIAMOND_TOKEN_NOT: return "not";
        case DIAMOND_TOKEN_AND_AND: return "and_and";
        case DIAMOND_TOKEN_OR_OR: return "or_or";
        case DIAMOND_TOKEN_AND: return "and";
        case DIAMOND_TOKEN_OR: return "or";
        case DIAMOND_TOKEN_PLUS: return "plus";
        case DIAMOND_TOKEN_MINUS: return "minus";
        case DIAMOND_TOKEN_ARROW: return "arrow";
        case DIAMOND_TOKEN_STAR: return "star";
        case DIAMOND_TOKEN_SLASH: return "slash";
        case DIAMOND_TOKEN_PERCENT: return "percent";
        case DIAMOND_TOKEN_CARET: return "caret";
        case DIAMOND_TOKEN_PLUS_EQUAL: return "plus_equal";
        case DIAMOND_TOKEN_MINUS_EQUAL: return "minus_equal";
        case DIAMOND_TOKEN_STAR_EQUAL: return "star_equal";
        case DIAMOND_TOKEN_SLASH_EQUAL: return "slash_equal";
        case DIAMOND_TOKEN_PERCENT_EQUAL: return "percent_equal";
        case DIAMOND_TOKEN_OR_OR_EQUAL: return "or_or_equal";
        case DIAMOND_TOKEN_AND_AND_EQUAL: return "and_and_equal";
        case DIAMOND_TOKEN_EQUAL: return "equal";
        case DIAMOND_TOKEN_EQUAL_EQUAL: return "equal_equal";
        case DIAMOND_TOKEN_BANG_EQUAL: return "bang_equal";
        case DIAMOND_TOKEN_LESS: return "less";
        case DIAMOND_TOKEN_LESS_EQUAL: return "less_equal";
        case DIAMOND_TOKEN_LESS_LESS: return "less_less";
        case DIAMOND_TOKEN_SPACESHIP: return "spaceship";
        case DIAMOND_TOKEN_GREATER: return "greater";
        case DIAMOND_TOKEN_GREATER_EQUAL: return "greater_equal";
        case DIAMOND_TOKEN_GREATER_GREATER: return "greater_greater";
        case DIAMOND_TOKEN_IF: return "if";
        case DIAMOND_TOKEN_UNLESS: return "unless";
        case DIAMOND_TOKEN_THEN: return "then";
        case DIAMOND_TOKEN_ELSE: return "else";
        case DIAMOND_TOKEN_ELSIF: return "elsif";
        case DIAMOND_TOKEN_CASE: return "case";
        case DIAMOND_TOKEN_WHEN: return "when";
        case DIAMOND_TOKEN_END: return "end";
        case DIAMOND_TOKEN_WHILE: return "while";
        case DIAMOND_TOKEN_UNTIL: return "until";
        case DIAMOND_TOKEN_DO: return "do";
        case DIAMOND_TOKEN_LOOP: return "loop";
        case DIAMOND_TOKEN_TRUE: return "true";
        case DIAMOND_TOKEN_FALSE: return "false";
        case DIAMOND_TOKEN_NIL: return "nil";
        case DIAMOND_TOKEN_DEF: return "def";
        case DIAMOND_TOKEN_CLOSURE: return "closure";
        case DIAMOND_TOKEN_CLASS: return "class";
        case DIAMOND_TOKEN_INTERFACE: return "interface";
        case DIAMOND_TOKEN_MODULE: return "module";
        case DIAMOND_TOKEN_INCLUDE: return "include";
        case DIAMOND_TOKEN_PRIVATE: return "private";
        case DIAMOND_TOKEN_PROTECTED: return "protected";
        case DIAMOND_TOKEN_PUBLIC: return "public";
        case DIAMOND_TOKEN_ATTR_READER: return "attr_reader";
        case DIAMOND_TOKEN_ATTR_WRITER: return "attr_writer";
        case DIAMOND_TOKEN_ATTR_ACCESSOR: return "attr_accessor";
        case DIAMOND_TOKEN_ATTR_PREDICATE: return "attr_predicate";
        case DIAMOND_TOKEN_ATTR: return "attr";
        case DIAMOND_TOKEN_MODULE_FUNCTION: return "module_function";
        case DIAMOND_TOKEN_ALIAS_METHOD: return "alias_method";
        case DIAMOND_TOKEN_DELEGATE: return "delegate";
        case DIAMOND_TOKEN_SELF: return "self";
        case DIAMOND_TOKEN_SUPER: return "super";
        case DIAMOND_TOKEN_RETURN: return "return";
        case DIAMOND_TOKEN_BREAK: return "break";
        case DIAMOND_TOKEN_NEXT: return "next";
        case DIAMOND_TOKEN_REDO: return "redo";
        case DIAMOND_TOKEN_RAISE: return "raise";
        case DIAMOND_TOKEN_RETRY: return "retry";
        case DIAMOND_TOKEN_YIELD: return "yield";
        case DIAMOND_TOKEN_BEGIN: return "begin";
        case DIAMOND_TOKEN_RESCUE: return "rescue";
        case DIAMOND_TOKEN_ENSURE: return "ensure";
        case DIAMOND_TOKEN_IS: return "is";
    }
    return "<unknown>";
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: lexer_dump FILE\n");
        return 64;
    }
    FILE *file = fopen(argv[1], "rb");
    if (file == nullptr) {
        fprintf(stderr, "lexer_dump: cannot open '%s'\n", argv[1]);
        return 74;
    }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 74; }
    const long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 74; }
    char *source = malloc((size_t)size + 1);
    if (source == nullptr) { fclose(file); return 74; }
    const size_t read_count = fread(source, 1, (size_t)size, file);
    fclose(file);
    source[read_count] = '\0';

    DiamondLexer lexer;
    diamond_lexer_init(&lexer, source);
    for (;;) {
        const DiamondToken token = diamond_lexer_next(&lexer);
        printf("%s %zu %zu %zu %zu\n", kind_name(token.kind), token.span.start,
               token.span.length, token.span.line, token.span.column);
        if (token.kind == DIAMOND_TOKEN_EOF) break;
    }
    free(source);
    return 0;
}
