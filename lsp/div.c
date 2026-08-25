/* open_memstream is POSIX.1-2008, not ISO C -- see hover.c's identical
 * comment on why this needs requesting explicitly under -std=c23. */
#define _POSIX_C_SOURCE 200809L

#include "div.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool div_is_template_path(const char *path_or_uri) {
    const size_t length=strlen(path_or_uri);
    static constexpr char suffix[]=".div";
    static constexpr size_t suffix_length=sizeof(suffix)-1;
    if(length<suffix_length)return false;
    return memcmp(path_or_uri+length-suffix_length,suffix,suffix_length)==0;
}

/* One "<%"/"%>"-delimited tag, or a run of ordinary text between two tags
 * (or between a tag and either end of the source) -- mirrors Div.scan's
 * own ["text"|"tag", content] token shape, but as an offset/length slice
 * into the caller's own `source` rather than a copied substring. */
typedef struct DivToken {
    bool is_tag;
    size_t start;
    size_t length;
} DivToken;

static const char *find_substring(const char *haystack,size_t haystack_length,
        const char *needle,size_t needle_length) {
    if(needle_length==0||haystack_length<needle_length)return nullptr;
    for(size_t index=0;index+needle_length<=haystack_length;index++)
        if(memcmp(haystack+index,needle,needle_length)==0)return haystack+index;
    return nullptr;
}

/* Scans `source` into `*out_tokens`/`*out_count` (both malloc'd, caller
 * frees the array), mirroring Div.scan exactly (compiler.di) -- the same
 * "<%"/"%>" delimiter search, the same "text before the first unmatched
 * <%" special case, the same unterminated-tag failure. Returns false only
 * for that last case, leaving *out_error_offset at the unterminated tag's
 * own "<%" start; *out_tokens and *out_count are untouched on failure. */
static bool div_scan(const char *source,size_t length,
        DivToken **out_tokens,size_t *out_count,size_t *out_error_offset) {
    size_t capacity=32,count=0;
    DivToken *tokens=malloc(capacity*sizeof *tokens);
    if(tokens==nullptr)return false;
    size_t pos=0;
    while(pos<length) {
        const char *found=find_substring(source+pos,length-pos,"<%",2);
        const size_t tag_start=found!=nullptr?(size_t)(found-source):length;
        if(tag_start>pos) {
            if(count==capacity) {
                capacity*=2;
                DivToken *grown=realloc(tokens,capacity*sizeof *grown);
                if(grown==nullptr) {free(tokens);return false;}
                tokens=grown;
            }
            tokens[count++]=(DivToken){.is_tag=false,.start=pos,.length=tag_start-pos};
        }
        if(found==nullptr)break;
        const size_t after_open=tag_start+2;
        const char *close=find_substring(source+after_open,length-after_open,"%>",2);
        if(close==nullptr) {
            free(tokens);
            *out_error_offset=tag_start;
            return false;
        }
        const size_t close_rel=(size_t)(close-(source+after_open));
        if(count==capacity) {
            capacity*=2;
            DivToken *grown=realloc(tokens,capacity*sizeof *grown);
            if(grown==nullptr) {free(tokens);return false;}
            tokens=grown;
        }
        tokens[count++]=(DivToken){.is_tag=true,.start=after_open,.length=close_rel};
        pos=after_open+close_rel+2;
    }
    *out_tokens=tokens;*out_count=count;
    return true;
}

/* Byte-offset -> 1-based (line,column) lookup, built once per translation
 * and reused for every token/chunk -- see div_translate's own comment for
 * why a running-position pass would work too, but this reads closer to
 * Div.scan/Div.emit_body's own already-separate passes. */
typedef struct DivSourceIndex {
    size_t *line_starts;
    size_t line_count;
} DivSourceIndex;

static bool div_source_index_build(DivSourceIndex *index,const char *source,size_t length) {
    size_t capacity=64,count=0;
    size_t *starts=malloc(capacity*sizeof *starts);
    if(starts==nullptr)return false;
    starts[count++]=0;
    for(size_t offset=0;offset<length;offset++) {
        if(source[offset]!='\n')continue;
        if(count==capacity) {
            capacity*=2;
            size_t *grown=realloc(starts,capacity*sizeof *grown);
            if(grown==nullptr) {free(starts);return false;}
            starts=grown;
        }
        starts[count++]=offset+1;
    }
    index->line_starts=starts;index->line_count=count;
    return true;
}

static void div_source_index_free(DivSourceIndex *index) {
    free(index->line_starts);
}

static void div_offset_to_position(const DivSourceIndex *index,size_t offset,
        size_t *out_line,size_t *out_column) {
    size_t low=0,high=index->line_count-1;
    while(low<high) {
        const size_t mid=low+(high-low+1)/2;
        if(index->line_starts[mid]<=offset)low=mid;else high=mid-1;
    }
    *out_line=low+1;
    *out_column=offset-index->line_starts[low]+1;
}

static bool is_div_whitespace(char ch) {
    return ch==' '||ch=='\t'||ch=='\n'||ch=='\r';
}

/* Trims leading/trailing whitespace from the (start,length) slice in
 * place, matching String#strip's own whitespace set closely enough for
 * this file's own narrow use (tag bodies, directive text -- never
 * arbitrary user strings). */
static void div_trim_slice(const char *source,size_t *start,size_t *length) {
    while(*length>0&&is_div_whitespace(source[*start])) {(*start)++;(*length)--;}
    while(*length>0&&is_div_whitespace(source[*start+*length-1]))(*length)--;
}

typedef struct DivTranslator {
    FILE *out;
    char *buffer;
    size_t buffer_length;
    DivPosition *positions;
    size_t position_count;
    size_t position_capacity;
    bool ok;
} DivTranslator;

static void div_push_position(DivTranslator *translator,size_t line,size_t column) {
    if(!translator->ok)return;
    if(translator->position_count==translator->position_capacity) {
        const size_t capacity=translator->position_capacity==0?
            64:translator->position_capacity*2;
        DivPosition *grown=realloc(translator->positions,capacity*sizeof *grown);
        if(grown==nullptr) {translator->ok=false;return;}
        translator->positions=grown;translator->position_capacity=capacity;
    }
    translator->positions[translator->position_count++]=(DivPosition){line,column};
}

/* Writes one boilerplate line (no single corresponding .div source
 * position -- see DivPosition's own comment) followed by '\n'. */
static void div_emit_fixed_line(DivTranslator *translator,const char *text) {
    if(!translator->ok)return;
    div_push_position(translator,0,0);
    if(!translator->ok)return;
    fputs(text,translator->out);
    fputc('\n',translator->out);
}

/* Writes `source[start,start+length)` -- code copied verbatim from a tag
 * body, so it may itself contain embedded newlines if the template
 * author wrote a multi-line `<% %>`/`<%= %>`/`<%== %>` tag -- as one
 * generated physical line per embedded line, each with `prefix`/`suffix`
 * added only to the very first/last physical line respectively (`nullptr`
 * for either omits it). Every resulting physical line keeps a real,
 * accurate .div source position: since this is a verbatim copy, each
 * physical line's own first character sits at a real, trackable offset
 * in `source`, unlike the escaped literal-text case
 * (div_emit_literal_line below), where escaping can change the character
 * count entirely. */
static void div_emit_wrapped(DivTranslator *translator,const DivSourceIndex *index,
        const char *prefix,const char *source,size_t start,size_t length,
        const char *suffix) {
    if(!translator->ok)return;
    size_t line_start=start;
    const size_t end=start+length;
    bool first=true;
    while(translator->ok) {
        size_t newline=SIZE_MAX;
        for(size_t offset=line_start;offset<end;offset++) {
            if(source[offset]=='\n') {newline=offset;break;}
        }
        const size_t line_end=newline==SIZE_MAX?end:newline;
        size_t source_line,source_column;
        div_offset_to_position(index,line_start,&source_line,&source_column);
        div_push_position(translator,source_line,source_column);
        if(!translator->ok)return;
        if(first&&prefix!=nullptr)fputs(prefix,translator->out);
        fwrite(source+line_start,1,line_end-line_start,translator->out);
        const bool is_last=newline==SIZE_MAX;
        if(is_last&&suffix!=nullptr)fputs(suffix,translator->out);
        fputc('\n',translator->out);
        if(is_last)break;
        line_start=newline+1;
        first=false;
    }
}

/* Writes a text token as one or more `sb.append("...")` lines, matching
 * Div.emit_literal_chunks -- chunk_size-limited slices, each escaped so
 * it's safe as a Diamond string literal's content (Div.escape_di_string).
 * Unlike div_emit_wrapped above, escaping can change the byte count (a
 * real newline becomes the two literal characters `\`+`n`), so this
 * never has to worry about embedded newlines splitting one chunk into
 * several physical lines -- each chunk is always exactly one. */
static void div_emit_literal(DivTranslator *translator,const DivSourceIndex *index,
        const char *source,size_t start,size_t length) {
    static constexpr size_t chunk_size=150;
    size_t pos=0;
    while(pos<length&&translator->ok) {
        size_t chunk_length=length-pos;
        if(chunk_length>chunk_size)chunk_length=chunk_size;
        size_t source_line,source_column;
        div_offset_to_position(index,start+pos,&source_line,&source_column);
        div_push_position(translator,source_line,source_column);
        if(!translator->ok)return;
        fputs("  sb.append(\"",translator->out);
        for(size_t offset=0;offset<chunk_length;offset++) {
            const char ch=source[start+pos+offset];
            switch(ch) {
                case '\\':fputs("\\\\",translator->out);break;
                case '"':fputs("\\\"",translator->out);break;
                case '#':fputs("\\#",translator->out);break;
                case '\n':fputs("\\n",translator->out);break;
                case '\r':fputs("\\r",translator->out);break;
                case '\t':fputs("\\t",translator->out);break;
                default:fputc(ch,translator->out);
            }
        }
        fputs("\")\n",translator->out);
        pos+=chunk_length;
    }
}

/* Writes one tag token's own generated code, matching Div.emit_tag's own
 * three shapes (a `#` comment/directive emits nothing -- Div.extract_locals
 * already pulled a `locals:` directive's names out separately), each via
 * div_emit_wrapped so a multi-line tag body still maps accurately. */
static void div_emit_tag(DivTranslator *translator,const DivSourceIndex *index,
        const char *source,size_t start,size_t length) {
    size_t body_start=start,body_length=length;
    div_trim_slice(source,&body_start,&body_length);
    if(body_length==0)return;
    if(source[body_start]=='#')return;
    if(body_length>=2&&source[body_start]=='='&&source[body_start+1]=='=') {
        size_t expr_start=body_start+2,expr_length=body_length-2;
        div_trim_slice(source,&expr_start,&expr_length);
        div_emit_wrapped(translator,index,"  sb.append(",source,expr_start,expr_length,")");
        return;
    }
    if(source[body_start]=='=') {
        size_t expr_start=body_start+1,expr_length=body_length-1;
        div_trim_slice(source,&expr_start,&expr_length);
        div_emit_wrapped(translator,index,"  sb.append(__div_escape_html__(",
            source,expr_start,expr_length,"))");
        return;
    }
    div_emit_wrapped(translator,index,nullptr,source,body_start,body_length,nullptr);
}

/* Pulls every name out of a `<%# locals: a, b %>` directive across every
 * tag token, matching Div.extract_locals -- malloc'd null-terminated
 * copies (caller frees each, then the two arrays) rather than slices,
 * since the parameter list gets written out after every token has
 * already been scanned once, well past `source`'s own lifetime concerns
 * mattering either way. */
static bool div_extract_locals(const char *source,const DivToken *tokens,size_t token_count,
        char ***out_names,size_t *out_count) {
    size_t capacity=8,count=0;
    char **names=malloc(capacity*sizeof *names);
    if(names==nullptr)return false;
    static constexpr char directive_prefix[]="locals:";
    static constexpr size_t directive_prefix_length=sizeof(directive_prefix)-1;
    for(size_t index=0;index<token_count;index++) {
        if(!tokens[index].is_tag)continue;
        size_t body_start=tokens[index].start,body_length=tokens[index].length;
        div_trim_slice(source,&body_start,&body_length);
        if(body_length==0||source[body_start]!='#')continue;
        size_t directive_start=body_start+1,directive_length=body_length-1;
        div_trim_slice(source,&directive_start,&directive_length);
        if(directive_length<directive_prefix_length||
           memcmp(source+directive_start,directive_prefix,directive_prefix_length)!=0)
            continue;
        size_t remainder_start=directive_start+directive_prefix_length;
        const size_t remainder_end=directive_start+directive_length;
        size_t part_start=remainder_start;
        for(size_t pos=remainder_start;pos<=remainder_end;pos++) {
            if(pos!=remainder_end&&source[pos]!=',')continue;
            size_t name_start=part_start,name_length=pos-part_start;
            div_trim_slice(source,&name_start,&name_length);
            if(name_length>0) {
                if(count==capacity) {
                    capacity*=2;
                    char **grown=realloc(names,capacity*sizeof *grown);
                    if(grown==nullptr) {
                        for(size_t free_index=0;free_index<count;free_index++)free(names[free_index]);
                        free(names);
                        return false;
                    }
                    names=grown;
                }
                char *copy=malloc(name_length+1);
                if(copy==nullptr) {
                    for(size_t free_index=0;free_index<count;free_index++)free(names[free_index]);
                    free(names);
                    return false;
                }
                memcpy(copy,source+name_start,name_length);
                copy[name_length]='\0';
                names[count++]=copy;
            }
            part_start=pos+1;
        }
    }
    *out_names=names;*out_count=count;
    return true;
}

char *div_translate(const char *source,size_t length,
        DivPosition **out_positions,size_t *out_position_count,
        size_t *out_error_line,size_t *out_error_column,
        const char **out_error_message) {
    DivToken *tokens=nullptr;size_t token_count=0;size_t error_offset=0;
    if(!div_scan(source,length,&tokens,&token_count,&error_offset)) {
        DivSourceIndex index;
        if(div_source_index_build(&index,source,length)) {
            div_offset_to_position(&index,error_offset,out_error_line,out_error_column);
            div_source_index_free(&index);
        } else {
            *out_error_line=1;*out_error_column=1;
        }
        *out_error_message="unterminated <% tag";
        return nullptr;
    }
    DivSourceIndex index;
    if(!div_source_index_build(&index,source,length)) {
        free(tokens);
        *out_error_line=1;*out_error_column=1;
        *out_error_message="out of memory";
        return nullptr;
    }
    char **locals_names=nullptr;size_t locals_count=0;
    if(!div_extract_locals(source,tokens,token_count,&locals_names,&locals_count)) {
        free(tokens);div_source_index_free(&index);
        *out_error_line=1;*out_error_column=1;
        *out_error_message="out of memory";
        return nullptr;
    }

    DivTranslator translator={.ok=true};
    translator.out=open_memstream(&translator.buffer,&translator.buffer_length);
    if(translator.out==nullptr) {
        free(tokens);div_source_index_free(&index);
        for(size_t i=0;i<locals_count;i++)free(locals_names[i]);
        free(locals_names);
        *out_error_line=1;*out_error_column=1;
        *out_error_message="out of memory";
        return nullptr;
    }

    /* Kept behaviorally identical to Div.escape_helper_lines
     * (compiler.di) and Div.escape_html (runtime.di) -- see that file's
     * own comment and test.sh's cross-check between the two. Fixed name
     * rather than divc's own per-file derived one: nothing outside this
     * one compile ever calls it (see div.h's own comment). */
    static const char *const helper_lines[]={
        "def __div_escape_html__(value)",
        "  s = \"#{value}\"",
        "  sb = StringBuilder.new()",
        "  i = 0",
        "  while i < s.length()",
        "    ch = s[i]",
        "    if ch == \"&\"",
        "      sb.append(\"&amp;\")",
        "    elsif ch == \"<\"",
        "      sb.append(\"&lt;\")",
        "    elsif ch == \">\"",
        "      sb.append(\"&gt;\")",
        "    elsif ch == \"\\\"\"",
        "      sb.append(\"&quot;\")",
        "    elsif ch == \"'\"",
        "      sb.append(\"&#39;\")",
        "    else",
        "      sb.append(ch)",
        "    end",
        "    i += 1",
        "  end",
        "  sb.to_s()",
        "end",
        "",
    };
    for(size_t i=0;i<sizeof(helper_lines)/sizeof(helper_lines[0]);i++)
        div_emit_fixed_line(&translator,helper_lines[i]);

    div_push_position(&translator,0,0);
    if(translator.ok) {
        fputs("def __div_template__(",translator.out);
        for(size_t i=0;i<locals_count;i++) {
            if(i>0)fputs(", ",translator.out);
            fputs(locals_names[i],translator.out);
        }
        fputs(")\n",translator.out);
    }
    div_emit_fixed_line(&translator,"  sb = StringBuilder.new()");

    for(size_t i=0;i<token_count&&translator.ok;i++) {
        if(tokens[i].is_tag)
            div_emit_tag(&translator,&index,source,tokens[i].start,tokens[i].length);
        else
            div_emit_literal(&translator,&index,source,tokens[i].start,tokens[i].length);
    }

    div_emit_fixed_line(&translator,"  sb.to_s()");
    div_emit_fixed_line(&translator,"end");

    fclose(translator.out);
    free(tokens);
    div_source_index_free(&index);
    for(size_t i=0;i<locals_count;i++)free(locals_names[i]);
    free(locals_names);

    if(!translator.ok) {
        free(translator.buffer);
        free(translator.positions);
        *out_error_line=1;*out_error_column=1;
        *out_error_message="out of memory";
        return nullptr;
    }
    *out_positions=translator.positions;
    *out_position_count=translator.position_count;
    return translator.buffer;
}
