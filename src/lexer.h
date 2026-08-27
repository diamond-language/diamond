#ifndef DIAMOND_LEXER_H
#define DIAMOND_LEXER_H

#include <stddef.h>

typedef struct DiamondSpan {
    size_t start;
    size_t length;
    size_t line;
    size_t column;
} DiamondSpan;

typedef enum DiamondTokenKind {
    DIAMOND_TOKEN_EOF,
    DIAMOND_TOKEN_ERROR,
    DIAMOND_TOKEN_INTEGER,
    DIAMOND_TOKEN_FLOAT,
    DIAMOND_TOKEN_STRING,
    DIAMOND_TOKEN_SYMBOL,
    DIAMOND_TOKEN_IDENTIFIER,
    DIAMOND_TOKEN_INSTANCE_VARIABLE,
    DIAMOND_TOKEN_CLASS_VARIABLE,
    DIAMOND_TOKEN_NEWLINE,
    DIAMOND_TOKEN_LEFT_PAREN,
    DIAMOND_TOKEN_RIGHT_PAREN,
    DIAMOND_TOKEN_LEFT_BRACKET,
    DIAMOND_TOKEN_RIGHT_BRACKET,
    DIAMOND_TOKEN_LEFT_BRACE,
    DIAMOND_TOKEN_RIGHT_BRACE,
    DIAMOND_TOKEN_COMMA,
    DIAMOND_TOKEN_DOT,
    /* Range operators -- `1..5` (inclusive), `1...5` (exclusive). Lexed
     * here, not folded into DIAMOND_TOKEN_DOT with a "how many dots"
     * count, so the parser can dispatch on token kind the same way it
     * already does for every other operator, rather than re-inspecting
     * the source text. See compiler.c's range desugaring for why no new
     * DiamondOpCode was needed for this. */
    DIAMOND_TOKEN_DOT_DOT,
    DIAMOND_TOKEN_DOT_DOT_DOT,
    DIAMOND_TOKEN_COLON,
    DIAMOND_TOKEN_DOUBLE_COLON,
    /* Ternary `cond ? a : b` -- QUESTION is its own token only when not
     * glued onto a preceding identifier (`respond_to?`, `exclusive?` stay
     * one predicate-method IDENTIFIER token, handled already by the
     * existing identifier scan). See compiler.c's parse_ternary for why
     * no new DiamondOpCode is needed: pure sugar over JUMP_IF_FALSE/MOVE/
     * JUMP, the same shape parse_if's own then/else already uses. */
    DIAMOND_TOKEN_QUESTION,
    DIAMOND_TOKEN_PIPE,
    DIAMOND_TOKEN_AMPERSAND,
    DIAMOND_TOKEN_BANG,
    DIAMOND_TOKEN_NOT,
    DIAMOND_TOKEN_AND_AND,
    DIAMOND_TOKEN_OR_OR,
    DIAMOND_TOKEN_AND,
    DIAMOND_TOKEN_OR,
    DIAMOND_TOKEN_PLUS,
    DIAMOND_TOKEN_MINUS,
    DIAMOND_TOKEN_ARROW,
    DIAMOND_TOKEN_STAR,
    DIAMOND_TOKEN_SLASH,
    DIAMOND_TOKEN_PERCENT,
    /* Pattern pin (`^name`), accepted only inside case Array patterns. */
    DIAMOND_TOKEN_CARET,
    /* Compound assignment (`x += 1`, ...) -- pure syntax sugar, expanded
     * entirely at parse time into the same opcode sequence the plain
     * `x = x + 1` spelling would already produce (see compile_compound_
     * assignment, compiler.c), so no new DiamondOpCode is needed for
     * these. DIAMOND_TOKEN_OR_OR_EQUAL/AND_AND_EQUAL live here too,
     * next to their arithmetic siblings, rather than next to AND_AND/
     * OR_OR above -- keeping every compound-assignment token together
     * matches how compound_assignment_ahead (compiler.c) checks them
     * as one group. */
    DIAMOND_TOKEN_PLUS_EQUAL,
    DIAMOND_TOKEN_MINUS_EQUAL,
    DIAMOND_TOKEN_STAR_EQUAL,
    DIAMOND_TOKEN_SLASH_EQUAL,
    DIAMOND_TOKEN_PERCENT_EQUAL,
    DIAMOND_TOKEN_OR_OR_EQUAL,
    DIAMOND_TOKEN_AND_AND_EQUAL,
    DIAMOND_TOKEN_EQUAL,
    DIAMOND_TOKEN_EQUAL_EQUAL,
    DIAMOND_TOKEN_BANG_EQUAL,
    DIAMOND_TOKEN_LESS,
    DIAMOND_TOKEN_LESS_EQUAL,
    DIAMOND_TOKEN_LESS_LESS,
    /* `<=>` -- Ruby's Comparable "spaceship" operator, a single method
     * deriving `<`/`<=`/`>`/`>=`/`==`. Lexed as its own token (not
     * inferred from LESS_EQUAL followed by a separate GREATER) so the
     * parser can dispatch on token kind the same way it already does for
     * every other operator. */
    DIAMOND_TOKEN_SPACESHIP,
    DIAMOND_TOKEN_GREATER,
    DIAMOND_TOKEN_GREATER_EQUAL,
    DIAMOND_TOKEN_IF,
    DIAMOND_TOKEN_UNLESS,
    DIAMOND_TOKEN_THEN,
    DIAMOND_TOKEN_ELSE,
    DIAMOND_TOKEN_ELSIF,
    DIAMOND_TOKEN_CASE,
    DIAMOND_TOKEN_WHEN,
    DIAMOND_TOKEN_END,
    DIAMOND_TOKEN_WHILE,
    DIAMOND_TOKEN_UNTIL,
    DIAMOND_TOKEN_DO,
    DIAMOND_TOKEN_LOOP,
    DIAMOND_TOKEN_TRUE,
    DIAMOND_TOKEN_FALSE,
    DIAMOND_TOKEN_NIL,
    DIAMOND_TOKEN_DEF,
    DIAMOND_TOKEN_CLOSURE,
    DIAMOND_TOKEN_CLASS,
    DIAMOND_TOKEN_INTERFACE,
    DIAMOND_TOKEN_MODULE,
    DIAMOND_TOKEN_INCLUDE,
    DIAMOND_TOKEN_PRIVATE,
    DIAMOND_TOKEN_PROTECTED,
    DIAMOND_TOKEN_PUBLIC,
    DIAMOND_TOKEN_ATTR_READER,
    DIAMOND_TOKEN_ATTR_WRITER,
    DIAMOND_TOKEN_ATTR_ACCESSOR,
    DIAMOND_TOKEN_ATTR_PREDICATE,
    DIAMOND_TOKEN_ATTR,
    DIAMOND_TOKEN_MODULE_FUNCTION,
    DIAMOND_TOKEN_ALIAS_METHOD,
    DIAMOND_TOKEN_DELEGATE,
    DIAMOND_TOKEN_SELF,
    DIAMOND_TOKEN_SUPER,
    DIAMOND_TOKEN_RETURN,
    DIAMOND_TOKEN_BREAK,
    DIAMOND_TOKEN_NEXT,
    DIAMOND_TOKEN_REDO,
    DIAMOND_TOKEN_RAISE,
    DIAMOND_TOKEN_RETRY,
    DIAMOND_TOKEN_YIELD,
    DIAMOND_TOKEN_BEGIN,
    DIAMOND_TOKEN_RESCUE,
    DIAMOND_TOKEN_ENSURE,
    DIAMOND_TOKEN_IS,
} DiamondTokenKind;

typedef struct DiamondToken {
    DiamondTokenKind kind;
    DiamondSpan span;
} DiamondToken;

typedef struct DiamondLexer {
    const char *source;
    size_t start;
    size_t current;
    size_t line;
    size_t column;
    size_t token_line;
    size_t token_column;
} DiamondLexer;

void diamond_lexer_init(DiamondLexer *lexer, const char *source);
DiamondToken diamond_lexer_next(DiamondLexer *lexer);

#endif
