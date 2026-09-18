/* See lsp/completion.c's own identical comment. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "formatting.h"

#include "lexer.h"

#include <stdlib.h>
#include <string.h>

/* One physical line of the document -- byte offsets into the null-
 * terminated source copy formatting_compute works from, excluding the
 * line terminator (`\r\n` and `\n` both handled). */
typedef struct FormatLine {
    size_t start;
    size_t length;
} FormatLine;

static bool split_lines(const char *text,size_t length,FormatLine **out_lines,
        size_t *out_count) {
    size_t capacity=64,count=0;
    FormatLine *lines=malloc(capacity*sizeof *lines);
    if(lines==nullptr)return false;
    size_t line_start=0;
    for(size_t index=0;index<=length;index++) {
        if(index==length||text[index]=='\n') {
            size_t line_end=index;
            if(line_end>line_start&&text[line_end-1]=='\r')line_end--;
            if(count==capacity) {
                capacity*=2;
                FormatLine *grown=realloc(lines,capacity*sizeof *lines);
                if(grown==nullptr) {free(lines);return false;}
                lines=grown;
            }
            lines[count++]=(FormatLine){.start=line_start,.length=line_end-line_start};
            line_start=index+1;
        }
    }
    *out_lines=lines;*out_count=count;
    return true;
}

/* NEWLINE tokens (real, emitted ones -- the parser needs them for
 * statement boundaries, unlike `#` comments, which are pure lexer
 * trivia and never become a token at all) are filtered out here, the
 * same way lsp/references.c's own tokenize loop already does: this
 * pass groups tokens by DiamondSpan.line directly, so it never needs
 * them, and leaving them in would corrupt "is this the first real
 * token on its own physical line" tracking (compute_line_depths'
 * own if/unless/while/until block-form test) the moment a NEWLINE
 * token's own reported line confused that check. */
static bool tokenize(const char *text,DiamondToken **out_tokens,size_t *out_count) {
    DiamondLexer lexer;
    diamond_lexer_init(&lexer,text);
    size_t capacity=256,count=0;
    DiamondToken *tokens=malloc(capacity*sizeof *tokens);
    if(tokens==nullptr)return false;
    for(;;) {
        const DiamondToken token=diamond_lexer_next(&lexer);
        if(token.kind==DIAMOND_TOKEN_NEWLINE)continue;
        if(count==capacity) {
            capacity*=2;
            DiamondToken *grown=realloc(tokens,capacity*sizeof *tokens);
            if(grown==nullptr) {free(tokens);return false;}
            tokens=grown;
        }
        tokens[count++]=token;
        if(token.kind==DIAMOND_TOKEN_EOF||token.kind==DIAMOND_TOKEN_ERROR)break;
    }
    *out_tokens=tokens;*out_count=count;
    return true;
}

/* True iff the def header at tokens[def_index] (itself DIAMOND_TOKEN_
 * DEF) is the endless `def name(...) = expr` form, which needs no
 * matching `end` at all -- docs/callables.md: "def always requires
 * parentheses, even for a zero-argument function", so a real
 * parameter list (possibly empty) always follows the name, whatever
 * shape the name itself takes (a plain identifier, an operator method
 * like `+`/`<=>`/`[]`, or a `self.name` singleton method) -- rather
 * than modeling that name grammar, this just skips forward to the
 * first LEFT_PAREN (never part of a name token itself) and treats that
 * as the parameter list's own open paren. Scans the balanced parameter
 * list, then an optional `-> ReturnType` (itself needing local bracket
 * tracking for a generic type like `Array[T]`), stopping at the first
 * token outside any nested bracket that is `=` (endless) or anything
 * else (an ordinary multi-line def, closed with `end`). Bounded by
 * token_count, never infinite even against truncated/malformed input. */
static bool def_is_endless(const DiamondToken *tokens,size_t token_count,size_t def_index) {
    size_t index=def_index+1;
    while(index<token_count&&tokens[index].kind!=DIAMOND_TOKEN_LEFT_PAREN&&
          tokens[index].kind!=DIAMOND_TOKEN_EOF)
        index++;
    if(index>=token_count||tokens[index].kind!=DIAMOND_TOKEN_LEFT_PAREN)return false;
    int paren_depth=0;
    for(;index<token_count;index++) {
        if(tokens[index].kind==DIAMOND_TOKEN_LEFT_PAREN)paren_depth++;
        else if(tokens[index].kind==DIAMOND_TOKEN_RIGHT_PAREN) {
            paren_depth--;
            if(paren_depth==0) {index++;break;}
        } else if(tokens[index].kind==DIAMOND_TOKEN_EOF)return false;
    }
    if(index<token_count&&tokens[index].kind==DIAMOND_TOKEN_ARROW) {
        /* NEWLINE tokens are filtered out of this whole array (see
         * tokenize's own comment), so nothing here naturally stops at
         * the end of the header line -- without an explicit line-
         * number check, this loop would keep scanning straight through
         * the def's own body and every function after it looking for
         * *any* `=` token anywhere in the rest of the file, and
         * (confirmed directly against lib/core.di: array_empty's
         * plain `-> Bool` return type, followed several functions
         * later by an unrelated `index = 0`) actually find one,
         * misclassifying a perfectly ordinary multi-line `def` as
         * endless. A return type can't itself span multiple lines
         * outside a bracket (the same "newline only right after an
         * open bracket/comma/before a close" rule every bracket-
         * delimited list already follows), so stop as soon as a token
         * outside any nested bracket lands on a different line than
         * the arrow itself -- that means the header ended with no `=`
         * in it, i.e. a real block-form def. */
        const size_t header_line=(size_t)tokens[index].span.line;
        index++;
        int bracket_depth=0;
        bool ran_past_header=false;
        for(;index<token_count;index++) {
            const DiamondToken current=tokens[index];
            if(current.kind==DIAMOND_TOKEN_LEFT_BRACKET)bracket_depth++;
            else if(current.kind==DIAMOND_TOKEN_RIGHT_BRACKET)bracket_depth--;
            else if(bracket_depth<=0) {
                if(current.kind==DIAMOND_TOKEN_EQUAL||current.kind==DIAMOND_TOKEN_EOF)break;
                if((size_t)current.span.line!=header_line) {ran_past_header=true;break;}
            }
        }
        if(ran_past_header)return false;
    }
    return index<token_count&&tokens[index].kind==DIAMOND_TOKEN_EQUAL;
}

/* True for a token kind that can be the very last token of a genuinely
 * *complete* value or statement -- the set a postfix if/unless/while/
 * until modifier (`return 1 if n <= 1`, `break if item == nil`) always
 * directly follows. Deliberately a deny-list, not the reverse: an
 * if/unless/while/until immediately preceded by anything else (an
 * assignment, a comma, an open bracket, a binary operator, or simply
 * being the first token on its own line) defaults to block-form,
 * needing a matching `end` -- the same safe default this project's own
 * "narrowest useful slice" bar would pick, and the one that actually
 * covers real code: an allow-list of "expects a new expression next"
 * token kinds (checked first, before this deny-list was found to be
 * both simpler and more complete) missed `if` nested inside a call's
 * own argument list (`foo(if x then a else b end)`, `,`/`(` before
 * it) -- found validating against this repo's own real corpus
 * (examples/project_board's own controllers). RETURN/RAISE/YIELD/
 * BREAK/NEXT/REDO/RETRY count as value-end too: each is already a
 * complete statement on its own with no value given, matching
 * src/compiler.c's own postfix_modifier_ahead comment ("a bare
 * return/raise, postfix-conditioned on cond"). */
static bool is_value_end_kind(DiamondTokenKind kind) {
    switch(kind) {
        case DIAMOND_TOKEN_IDENTIFIER:
        case DIAMOND_TOKEN_INTEGER:
        case DIAMOND_TOKEN_FLOAT:
        case DIAMOND_TOKEN_STRING:
        case DIAMOND_TOKEN_SYMBOL:
        case DIAMOND_TOKEN_INSTANCE_VARIABLE:
        case DIAMOND_TOKEN_CLASS_VARIABLE:
        case DIAMOND_TOKEN_RIGHT_PAREN:
        case DIAMOND_TOKEN_RIGHT_BRACKET:
        case DIAMOND_TOKEN_RIGHT_BRACE:
        case DIAMOND_TOKEN_TRUE:
        case DIAMOND_TOKEN_FALSE:
        case DIAMOND_TOKEN_NIL:
        case DIAMOND_TOKEN_SELF:
        case DIAMOND_TOKEN_SUPER:
        case DIAMOND_TOKEN_END:
        case DIAMOND_TOKEN_BREAK:
        case DIAMOND_TOKEN_NEXT:
        case DIAMOND_TOKEN_REDO:
        case DIAMOND_TOKEN_RETRY:
        case DIAMOND_TOKEN_RETURN:
        case DIAMOND_TOKEN_RAISE:
        case DIAMOND_TOKEN_YIELD:
            return true;
        default:
            return false;
    }
}

static bool is_leading_dedent_kind(DiamondTokenKind kind) {
    return kind==DIAMOND_TOKEN_END||kind==DIAMOND_TOKEN_RIGHT_PAREN||
        kind==DIAMOND_TOKEN_RIGHT_BRACKET||kind==DIAMOND_TOKEN_RIGHT_BRACE;
}

static bool is_branch_kind(DiamondTokenKind kind) {
    return kind==DIAMOND_TOKEN_ELSE||kind==DIAMOND_TOKEN_ELSIF||
        kind==DIAMOND_TOKEN_WHEN||kind==DIAMOND_TOKEN_RESCUE||
        kind==DIAMOND_TOKEN_ENSURE;
}

/* Per physical line (1-indexed, matching DiamondSpan.line): whether any
 * real token starts on it, and the indent depth (in levels, not
 * spaces) it should print at when it does. A line with no tokens at
 * all (blank, or comment-only -- `#` comments are lexer trivia, never
 * tokens, see src/lexer.c) carries the depth in effect going into it,
 * for a comment to line up with its surrounding code. */
typedef struct FormatLineDepth {
    bool has_tokens;
    int depth;
} FormatLineDepth;

/* Walks every real token once, computing each physical line's own
 * print depth -- see formatting.h's own doc comment for exactly what
 * counts as a block/bracket opener or closer, and the dedent-keyword/
 * postfix-modifier rules. Returns false (a real, expected outcome, not
 * an allocation failure) when depth goes negative or doesn't return to
 * exactly 0 by EOF -- an unbalanced construct, or a real grammar shape
 * this pass doesn't yet model; formatting_compute treats that as "make
 * no edits", never a guess. */
/* Generous enough that only a pathologically deep (or malformed) real
 * file would ever hit it; compute_line_depths bails out (returns
 * false, same as any other "not confident" case) rather than
 * overflowing this if it does. */
#define DIAMOND_FORMAT_MAX_DEPTH 256

/* Records whether the level about to be entered is an `interface` body
 * (see compute_line_depths' own interface_level doc comment) and
 * increments *depth, or returns false without changing anything if
 * *depth is already at DIAMOND_FORMAT_MAX_DEPTH. */
static bool push_level(int *depth,bool is_interface,bool *interface_level) {
    if(*depth>=DIAMOND_FORMAT_MAX_DEPTH)return false;
    interface_level[*depth]=is_interface;
    (*depth)++;
    return true;
}

static bool compute_line_depths(const DiamondToken *tokens,size_t token_count,
        size_t line_count,FormatLineDepth *lines) {
    for(size_t line=0;line<line_count;line++)lines[line]=(FormatLineDepth){0};
    int depth=0;
    /* interface_level[N]: true iff the level about to be entered when
     * depth is N (i.e. the *previous* opener) was an `interface`. Only
     * consulted by DEF/CLOSURE: a `def name(...) -> Type` header with
     * no body and no matching `end` of its own is legal exactly one
     * place -- directly inside an interface's own body (its member
     * signatures) -- since interfaces are pure declarations, never
     * class/module/struct/begin/case/loop/do/if/while/until/a bracket,
     * none of which can directly contain a bare signature this way.
     * Found the hard way against this repo's own real corpus
     * (packages/arel/lib/arel/support.di's own `interface
     * ArelTraversalNode` block): def_is_endless correctly said "not
     * endless" for a signature-only def (no `=` anywhere), so it got
     * an `end` expectation that never resolves, since the interface's
     * own single `end` closes the whole body, not each signature
     * individually. */
    bool interface_level[DIAMOND_FORMAT_MAX_DEPTH]={0};
    DiamondTokenKind prev_kind=DIAMOND_TOKEN_NEWLINE; /* sentinel: nothing before EOF-of-nothing yet counts as a fresh line */
    size_t token_index=0;
    size_t last_processed_line=0;
    for(size_t line=1;line<=line_count&&token_index<token_count;line++) {
        if(tokens[token_index].kind==DIAMOND_TOKEN_EOF)break;
        last_processed_line=line;
        if((size_t)tokens[token_index].span.line!=line) {
            /* No token starts on this physical line at all. */
            lines[line-1]=(FormatLineDepth){.has_tokens=false,.depth=depth};
            continue;
        }
        size_t k=token_index;
        int leading_dedent=0;
        while(k<token_count&&(size_t)tokens[k].span.line==line&&
              is_leading_dedent_kind(tokens[k].kind)) {
            depth--;leading_dedent++;
            if(depth<0)return false;
            prev_kind=tokens[k].kind;
            k++;
        }
        (void)leading_dedent;
        const bool branch_line=k<token_count&&(size_t)tokens[k].span.line==line&&
            is_branch_kind(tokens[k].kind);
        const int print_depth=depth-(branch_line?1:0);
        if(print_depth<0)return false;
        lines[line-1]=(FormatLineDepth){.has_tokens=true,.depth=print_depth};

        bool first_in_rest_of_line=true;
        /* True right after a block-form `loop`/`while`/`until` on this
         * same physical line, until either a `do` is actually seen (it
         * absorbs it, see below) or the line just ends -- naturally
         * scoped to one line since a loop/while/until's own optional
         * `do` must always directly follow it with no line break in
         * between (src/compiler.c's consume_loop_start), the same
         * "newline only right after an open bracket/comma/before a
         * close" rule already bounds a condition expression's own
         * span. Deliberately never cleared by any *other* token kind:
         * `while cond do` needs this to survive every token of `cond`
         * itself, however many there are, not just the token
         * immediately after `while`. */
        bool pending_loop_do=false;
        while(k<token_count&&(size_t)tokens[k].span.line==line) {
            const DiamondTokenKind kind=tokens[k].kind;
            switch(kind) {
                case DIAMOND_TOKEN_LEFT_PAREN:
                case DIAMOND_TOKEN_LEFT_BRACKET:
                case DIAMOND_TOKEN_LEFT_BRACE:
                    if(!push_level(&depth,false,interface_level))return false;
                    break;
                case DIAMOND_TOKEN_RIGHT_PAREN:
                case DIAMOND_TOKEN_RIGHT_BRACKET:
                case DIAMOND_TOKEN_RIGHT_BRACE:
                case DIAMOND_TOKEN_END:
                    depth--;
                    if(depth<0)return false;
                    break;
                case DIAMOND_TOKEN_CLASS:
                    /* `value.class()` -- src/compiler.c's own
                     * parse_invoke deliberately accepts DIAMOND_TOKEN_
                     * CLASS right after a `.` as this one compiler-
                     * recognized member name (a class *declaration*
                     * never starts mid-expression after a dot, so it's
                     * unambiguous there): "right after '.' it can only
                     * ever be a member name". No other keyword gets
                     * this treatment -- `.begin`/`.end`/`.def`/etc.
                     * right after a dot are real compile errors
                     * ("expected method name after '.'"), not
                     * something this pass needs to special-case too.
                     * Found against this repo's own real corpus
                     * (packages/logger's own `error.class()` call). */
                    if(prev_kind!=DIAMOND_TOKEN_DOT&&
                       !push_level(&depth,false,interface_level))
                        return false;
                    break;
                case DIAMOND_TOKEN_MODULE:
                case DIAMOND_TOKEN_STRUCT:
                case DIAMOND_TOKEN_BEGIN:
                case DIAMOND_TOKEN_CASE:
                    if(!push_level(&depth,false,interface_level))return false;
                    break;
                case DIAMOND_TOKEN_INTERFACE:
                    if(!push_level(&depth,true,interface_level))return false;
                    break;
                case DIAMOND_TOKEN_LOOP:
                    /* Bare `loop ... end` and `loop do ... end` are the
                     * exact same one-level construct (parse_loop's own
                     * consume_loop_start absorbs an optional `do` right
                     * after `loop` as pure syntax sugar, not a second
                     * nested block) -- only `loop` itself opens a
                     * level; the `do` case below must not also open one
                     * when it's this same construct's own. */
                    if(!push_level(&depth,false,interface_level))return false;
                    pending_loop_do=true;
                    break;
                case DIAMOND_TOKEN_DO:
                    /* Opens a level of its own (attaching a block to an
                     * arbitrary call, `values.each() do |x| ... end`)
                     * unless it's the optional decoration right after
                     * `loop`/a block-form `while`/`until`'s own
                     * condition, which already opened this exact level
                     * when the loop/while/until token itself was seen. */
                    if(!pending_loop_do&&!push_level(&depth,false,interface_level))
                        return false;
                    pending_loop_do=false;
                    break;
                case DIAMOND_TOKEN_DEF:
                case DIAMOND_TOKEN_CLOSURE:
                    /* `closure name(...) ... end` (a named closure) goes
                     * through the exact same compile_definition as `def`
                     * (src/compiler.c, just with is_closure=true) --
                     * identical grammar, identical possible endless
                     * `= expr` one-liner form, so def_is_endless applies
                     * unchanged. Found the hard way: this repo's own
                     * corpus (packages/active_tagging's own
                     * `closure collect_name(raw) ... end`) was the only
                     * thing that ever exercised this keyword during
                     * validation -- CLOSURE was simply absent from this
                     * switch entirely before, silently under-counting
                     * depth by one for its whole body.
                     *
                     * A third shape, checked first: `def name(...) ->
                     * Type` with no body and no `end` of its own at all
                     * -- legal exactly inside an `interface` body (a
                     * pure signature), where the interface's own single
                     * `end` closes the whole thing. def_is_endless would
                     * say "not endless" here (there's no `=`), which is
                     * correct on its own terms but would still wrongly
                     * expect a matching `end` that will never come --
                     * found against this repo's own real corpus
                     * (packages/arel/lib/arel/support.di's own
                     * `interface ArelTraversalNode` block). */
                    {
                        const bool signature_only=depth>0&&interface_level[depth-1];
                        if(!signature_only&&!def_is_endless(tokens,token_count,k)) {
                            if(!push_level(&depth,false,interface_level))return false;
                        }
                    }
                    break;
                case DIAMOND_TOKEN_IF:
                case DIAMOND_TOKEN_UNLESS:
                case DIAMOND_TOKEN_WHILE:
                case DIAMOND_TOKEN_UNTIL:
                    /* Block-form (opens a level, needs a matching `end`)
                     * iff this is the first token processed for its own
                     * physical line (a fresh statement, or a bracket-
                     * continuation line -- Diamond only allows a
                     * newline outside brackets at a real statement
                     * boundary, so "first on its own line" already
                     * covers both) or NOT immediately preceded by a
                     * value-end token -- see is_value_end_kind's own
                     * comment for why that's the deny-list this checks,
                     * not an allow-list of specific "expects a new
                     * expression" kinds. Matches src/compiler.c's own
                     * postfix_modifier_ahead in spirit, not by sharing
                     * its code (that one runs mid-parse with real
                     * expression-boundary information this token-only
                     * pass doesn't have). WHILE/UNTIL specifically also
                     * arm pending_loop_do the same way LOOP does, since
                     * they share consume_loop_start's own optional-`do`
                     * grammar. */
                    if(first_in_rest_of_line||!is_value_end_kind(prev_kind)) {
                        if(!push_level(&depth,false,interface_level))return false;
                        if(kind==DIAMOND_TOKEN_WHILE||kind==DIAMOND_TOKEN_UNTIL)
                            pending_loop_do=true;
                    }
                    break;
                default:break;
            }
            prev_kind=kind;
            first_in_rest_of_line=false;
            k++;
        }
        token_index=k;
    }
    /* Any lines past the last one the main loop actually visited
     * (trailing blank/comment lines once every real token is
     * consumed, or the loop stopped at a real EOF token before
     * reaching line_count) carry the final depth forward -- tracked
     * via last_processed_line directly rather than inferred from the
     * EOF token's own reported line, which could otherwise fall
     * inside the already-correctly-filled range and silently
     * overwrite a real line's own computed depth. */
    for(size_t line=last_processed_line+1;line<=line_count;line++)
        lines[line-1]=(FormatLineDepth){.has_tokens=false,.depth=depth};
    return depth==0;
}

static JsonValue *text_edit(size_t line,size_t start_col,size_t end_col,
        const char *new_text,size_t new_text_length) {
    JsonValue *range=json_object();
    JsonValue *start=json_object();
    JsonValue *end=json_object();
    JsonValue *result=json_object();
    if(range==nullptr||start==nullptr||end==nullptr||result==nullptr) {
        json_free(range);json_free(start);json_free(end);json_free(result);
        return nullptr;
    }
    json_object_set(start,"line",json_number((double)(line-1)));
    json_object_set(start,"character",json_number((double)start_col));
    json_object_set(end,"line",json_number((double)(line-1)));
    json_object_set(end,"character",json_number((double)end_col));
    json_object_set(range,"start",start);
    json_object_set(range,"end",end);
    json_object_set(result,"range",range);
    json_object_set(result,"newText",json_string(new_text,new_text_length));
    return result;
}

/* True iff `content[0..leading)` is already exactly `target_spaces`
 * plain ASCII spaces -- the common "nothing to do" case this function
 * exists to detect cheaply, so an already-conventional document gets
 * an empty edit list back rather than a same-content no-op edit for
 * every single line. Any other leading whitespace (a tab, the wrong
 * count, ...) always needs normalizing regardless of byte length. */
static bool leading_already_correct(const char *content,size_t leading,size_t target_spaces) {
    if(leading!=target_spaces)return false;
    for(size_t column=0;column<leading;column++)
        if(content[column]!=' ')return false;
    return true;
}

JsonValue *formatting_compute(const char *text,size_t length) {
    char *source_copy=malloc(length+1);
    if(source_copy==nullptr)return nullptr;
    memcpy(source_copy,text,length);
    source_copy[length]='\0';

    DiamondToken *tokens=nullptr;size_t token_count=0;
    if(!tokenize(source_copy,&tokens,&token_count)) {free(source_copy);return nullptr;}
    for(size_t index=0;index<token_count;index++)
        if(tokens[index].kind==DIAMOND_TOKEN_ERROR) {
            free(tokens);free(source_copy);
            return json_null();
        }

    FormatLine *lines=nullptr;size_t line_count=0;
    if(!split_lines(source_copy,length,&lines,&line_count)) {
        free(tokens);free(source_copy);return nullptr;
    }

    FormatLineDepth *depths=malloc((line_count==0?1:line_count)*sizeof *depths);
    if(depths==nullptr) {free(lines);free(tokens);free(source_copy);return nullptr;}
    const bool balanced=line_count==0?true:
        compute_line_depths(tokens,token_count,line_count,depths);
    free(tokens);
    if(!balanced) {
        free(depths);free(lines);free(source_copy);
        return json_null();
    }

    JsonValue *result=json_array();
    if(result==nullptr) {free(depths);free(lines);free(source_copy);return nullptr;}
    bool build_ok=true;
    for(size_t index=0;index<line_count&&build_ok;index++) {
        const FormatLine current=lines[index];
        const char *content=source_copy+current.start;
        size_t leading=0;
        while(leading<current.length&&(content[leading]==' '||content[leading]=='\t'))
            leading++;
        size_t trailing_start=current.length;
        while(trailing_start>leading&&
              (content[trailing_start-1]==' '||content[trailing_start-1]=='\t'))
            trailing_start--;
        const bool blank_line=trailing_start==leading;

        const size_t target_spaces=blank_line?0:
            (size_t)(depths[index].depth<0?0:depths[index].depth)*2;

        if(blank_line) {
            if(leading>0) {
                JsonValue *edit=text_edit(index+1,0,leading,"",0);
                if(edit==nullptr||!json_array_push(result,edit)) {
                    json_free(edit);build_ok=false;break;
                }
            }
            continue;
        }

        if(!leading_already_correct(content,leading,target_spaces)) {
            char *spaces=target_spaces==0?nullptr:malloc(target_spaces);
            if(target_spaces>0&&spaces==nullptr) {build_ok=false;break;}
            if(target_spaces>0)memset(spaces,' ',target_spaces);
            JsonValue *edit=text_edit(index+1,0,leading,spaces,target_spaces);
            free(spaces);
            if(edit==nullptr||!json_array_push(result,edit)) {
                json_free(edit);build_ok=false;break;
            }
        }

        if(trailing_start<current.length) {
            JsonValue *edit=text_edit(index+1,trailing_start,current.length,"",0);
            if(edit==nullptr||!json_array_push(result,edit)) {
                json_free(edit);build_ok=false;break;
            }
        }
    }
    free(depths);free(lines);free(source_copy);
    if(!build_ok) {json_free(result);return nullptr;}
    return result;
}
