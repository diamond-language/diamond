#include "lexer.h"

static bool at_end(const DiamondLexer *lexer) {
    return lexer->source[lexer->current] == '\0';
}

static char advance(DiamondLexer *lexer) {
    const char character = lexer->source[lexer->current++];
    if (character == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return character;
}

static bool match(DiamondLexer *lexer, char expected) {
    if (lexer->source[lexer->current] != expected) {
        return false;
    }
    advance(lexer);
    return true;
}

static bool identifier_start(char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') || character == '_';
}

static bool identifier_part(char character) {
    return identifier_start(character) ||
           (character >= '0' && character <= '9');
}

static bool text_equals(const DiamondLexer *lexer, const char *text,
                        size_t length) {
    if (lexer->current - lexer->start != length) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        if (lexer->source[lexer->start + index] != text[index]) {
            return false;
        }
    }
    return true;
}

static DiamondTokenKind identifier_kind(const DiamondLexer *lexer) {
    if (text_equals(lexer, "if", 2)) return DIAMOND_TOKEN_IF;
    if (text_equals(lexer, "else", 4)) return DIAMOND_TOKEN_ELSE;
    if (text_equals(lexer, "end", 3)) return DIAMOND_TOKEN_END;
    if (text_equals(lexer, "while", 5)) return DIAMOND_TOKEN_WHILE;
    if (text_equals(lexer, "true", 4)) return DIAMOND_TOKEN_TRUE;
    if (text_equals(lexer, "false", 5)) return DIAMOND_TOKEN_FALSE;
    if (text_equals(lexer, "nil", 3)) return DIAMOND_TOKEN_NIL;
    if (text_equals(lexer, "def", 3)) return DIAMOND_TOKEN_DEF;
    if (text_equals(lexer, "class", 5)) return DIAMOND_TOKEN_CLASS;
    if (text_equals(lexer, "interface", 9)) return DIAMOND_TOKEN_INTERFACE;
    if (text_equals(lexer, "module", 6)) return DIAMOND_TOKEN_MODULE;
    if (text_equals(lexer, "include", 7)) return DIAMOND_TOKEN_INCLUDE;
    if (text_equals(lexer, "self", 4)) return DIAMOND_TOKEN_SELF;
    if (text_equals(lexer, "super", 5)) return DIAMOND_TOKEN_SUPER;
    if (text_equals(lexer, "return", 6)) return DIAMOND_TOKEN_RETURN;
    if (text_equals(lexer, "break", 5)) return DIAMOND_TOKEN_BREAK;
    if (text_equals(lexer, "next", 4)) return DIAMOND_TOKEN_NEXT;
    if (text_equals(lexer, "raise", 5)) return DIAMOND_TOKEN_RAISE;
    if (text_equals(lexer, "begin", 5)) return DIAMOND_TOKEN_BEGIN;
    if (text_equals(lexer, "rescue", 6)) return DIAMOND_TOKEN_RESCUE;
    if (text_equals(lexer, "ensure", 6)) return DIAMOND_TOKEN_ENSURE;
    if (text_equals(lexer, "is", 2)) return DIAMOND_TOKEN_IS;
    return DIAMOND_TOKEN_IDENTIFIER;
}

static DiamondToken token(const DiamondLexer *lexer, DiamondTokenKind kind) {
    return (DiamondToken){
        .kind = kind,
        .span = {
            .start = lexer->start,
            .length = lexer->current - lexer->start,
            .line = lexer->token_line,
            .column = lexer->token_column,
        },
    };
}

void diamond_lexer_init(DiamondLexer *lexer, const char *source) {
    *lexer = (DiamondLexer){
        .source = source,
        .line = 1,
        .column = 1,
        .token_line = 1,
        .token_column = 1,
    };
}

DiamondToken diamond_lexer_next(DiamondLexer *lexer) {
    while (!at_end(lexer)) {
        const char character = lexer->source[lexer->current];
        if (character == ' ' || character == '\t' || character == '\r') {
            advance(lexer);
            continue;
        }
        if (character == '#') {
            static constexpr char line_reset[]="#line 1";
            bool reset=true;
            for(size_t index=0;index<sizeof(line_reset)-1;index++)
                if(lexer->source[lexer->current+index]!=line_reset[index]) {
                    reset=false;break;
                }
            if(reset) {
                const char after=lexer->source[lexer->current+sizeof(line_reset)-1];
                if(after=='\n'||after=='\0')lexer->line=0;
            }
            while (!at_end(lexer) && lexer->source[lexer->current] != '\n') {
                advance(lexer);
            }
            continue;
        }
        break;
    }

    lexer->start = lexer->current;
    lexer->token_line = lexer->line;
    lexer->token_column = lexer->column;

    if (at_end(lexer)) {
        return token(lexer, DIAMOND_TOKEN_EOF);
    }

    const char character = advance(lexer);
    if (character >= '0' && character <= '9') {
        while (true) {
            const char next=lexer->source[lexer->current];
            if(next>='0' && next<='9') {
                advance(lexer);
                continue;
            }
            if(next=='_') {
                const char after=lexer->source[lexer->current+1];
                if(after<'0' || after>'9') {
                    advance(lexer);
                    return token(lexer,DIAMOND_TOKEN_ERROR);
                }
                advance(lexer);
                continue;
            }
            break;
        }
        return token(lexer, DIAMOND_TOKEN_INTEGER);
    }
    if (character == '"') {
        while (!at_end(lexer) && lexer->source[lexer->current] != '"') {
            if(lexer->source[lexer->current]=='#'&&
               lexer->source[lexer->current+1]=='{') {
                advance(lexer);advance(lexer);size_t depth=1;
                while(!at_end(lexer)&&depth>0) {
                    const char embedded=advance(lexer);
                    if(embedded=='"') {
                        while(!at_end(lexer)&&lexer->source[lexer->current]!='"') {
                            if(lexer->source[lexer->current]=='\\'&&
                               lexer->source[lexer->current+1]!='\0')advance(lexer);
                            advance(lexer);
                        }
                        if(!at_end(lexer))advance(lexer);
                    } else if(embedded=='{')depth++;
                    else if(embedded=='}')depth--;
                }
                if(depth!=0)return token(lexer,DIAMOND_TOKEN_ERROR);
                continue;
            }
            if (lexer->source[lexer->current] == '\\' &&
                lexer->source[lexer->current + 1] != '\0') {
                advance(lexer);
            }
            advance(lexer);
        }
        if (at_end(lexer)) return token(lexer, DIAMOND_TOKEN_ERROR);
        advance(lexer);
        return token(lexer, DIAMOND_TOKEN_STRING);
    }
    if (identifier_start(character)) {
        while (identifier_part(lexer->source[lexer->current])) {
            advance(lexer);
        }
        return token(lexer, identifier_kind(lexer));
    }
    if (character == '@' && identifier_start(lexer->source[lexer->current])) {
        while (identifier_part(lexer->source[lexer->current])) advance(lexer);
        return token(lexer, DIAMOND_TOKEN_INSTANCE_VARIABLE);
    }

    switch (character) {
        case '(':
            return token(lexer, DIAMOND_TOKEN_LEFT_PAREN);
        case ')':
            return token(lexer, DIAMOND_TOKEN_RIGHT_PAREN);
        case '[':
            return token(lexer, DIAMOND_TOKEN_LEFT_BRACKET);
        case ']':
            return token(lexer, DIAMOND_TOKEN_RIGHT_BRACKET);
        case '{':
            return token(lexer, DIAMOND_TOKEN_LEFT_BRACE);
        case '}':
            return token(lexer, DIAMOND_TOKEN_RIGHT_BRACE);
        case ',':
            return token(lexer, DIAMOND_TOKEN_COMMA);
        case '.':
            return token(lexer, DIAMOND_TOKEN_DOT);
        case ':':
            return token(lexer,match(lexer,':')?DIAMOND_TOKEN_DOUBLE_COLON:
                         DIAMOND_TOKEN_COLON);
        case '|':
            return token(lexer, match(lexer, '|') ? DIAMOND_TOKEN_OR_OR
                                                   : DIAMOND_TOKEN_PIPE);
        case '&':
            return token(lexer, match(lexer, '&') ? DIAMOND_TOKEN_AND_AND
                                                   : DIAMOND_TOKEN_ERROR);
        case '+':
            return token(lexer, DIAMOND_TOKEN_PLUS);
        case '-':
            return token(lexer, match(lexer, '>') ? DIAMOND_TOKEN_ARROW
                                                   : DIAMOND_TOKEN_MINUS);
        case '*':
            return token(lexer, DIAMOND_TOKEN_STAR);
        case '/':
            return token(lexer, DIAMOND_TOKEN_SLASH);
        case '\n':
            return token(lexer, DIAMOND_TOKEN_NEWLINE);
        case ';':
            return token(lexer, DIAMOND_TOKEN_NEWLINE);
        case '=':
            return token(lexer, match(lexer, '=') ? DIAMOND_TOKEN_EQUAL_EQUAL
                                                   : DIAMOND_TOKEN_EQUAL);
        case '!':
            return token(lexer, match(lexer, '=') ? DIAMOND_TOKEN_BANG_EQUAL
                                                   : DIAMOND_TOKEN_BANG);
        case '<':
            return token(lexer, match(lexer, '=') ? DIAMOND_TOKEN_LESS_EQUAL
                                                   : DIAMOND_TOKEN_LESS);
        case '>':
            return token(lexer, match(lexer, '=') ? DIAMOND_TOKEN_GREATER_EQUAL
                                                   : DIAMOND_TOKEN_GREATER);
        default:
            return token(lexer, DIAMOND_TOKEN_ERROR);
    }
}
