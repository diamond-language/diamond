#include "vm_internal.h"
#include "bignum.h"

#include <math.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native recursive-descent JSON parser (RFC 8259), replacing lib/core/
 * json_codec.d6's own pure-Diamond JSONCodec#parse for JSON.parse's hot
 * path -- confirmed directly at ~330ms/MB (interpreted bytecode walking
 * a String one character/method-call at a time) against a real
 * training-corpus-scale dataset (examples/transformer/lib/corpus.d6's
 * own comment). Semantics match JSONCodec#parse exactly: same grammar
 * (no leading '+', no rejection of leading zeros -- neither did the
 * version this replaces), same surrogate-pair combining, same result
 * shape (String/Int/Float/Bool/nil/Array/Hash), same JSONError-on-
 * malformed-input contract -- verified against every tests/cases/
 * json_parse_*.d6 case. Diamond String is a raw byte buffer, not UTF-8-
 * validated (see docs/design.md), so this parses bytes throughout;
 * \uXXXX escapes are the one place UTF-8 encoding happens, matching
 * json_codec.d6's own utf8_encode exactly.
 *
 * GC safety: only *containers* (Array/Hash) are ever gc_protect'd, once
 * each, for the life of the whole top-level parse (unprotected together,
 * once, by the String#parse_json call site below) -- a leaf String/Int/
 * Float/Bool/nil value is never protected individually, since every
 * call site that receives one immediately either returns it straight up
 * the recursion (no allocation in between) or pushes/sets it into an
 * already-protected container with no allocation in between (array_push/
 * hash_set's own growth uses realloc, never a GC-tracked allocate_*
 * call, so neither can trigger a collection mid-push/set). The one real
 * exception is an object's own key: it must survive its *value*'s
 * parse, which can allocate arbitrarily many times before returning --
 * json_parse_object roots it as a placeholder entry in the
 * already-protected Hash first (nil needs no protection of its own),
 * the same pattern DIAMOND_OP_IO_POLL's own Hash result and
 * method_missing_helper's own args[]-building already use, overwritten
 * once the real value is ready. */
typedef struct JsonParser {
    DiamondVm *vm;
    const char *source;
    size_t length;
    size_t pos;
    /* Unlike ordinary Diamond-level recursion (run_chunk's own `depth`,
     * bounded by DIAMOND_MAX_CALL_DEPTH), array/object nesting here
     * recurses directly in C with no depth accounting at all otherwise
     * -- a real, not hypothetical, gap for a corpus-loading parser
     * specifically: deeply nested real-world JSON would crash the whole
     * process via a genuine C stack overflow instead of raising a clean
     * SystemStackError the way every other unbounded-recursion path in
     * this VM already does. Reuses DIAMOND_MAX_CALL_DEPTH itself rather
     * than a separate constant: this parser's own per-level C stack
     * frames are smaller than run_chunk's own (no register file, no
     * opcode dispatch), so the same bound that's already empirically
     * proven safe there is safe here too. */
    size_t depth;
} JsonParser;

static DiamondVmStatus json_parse_value(JsonParser *parser,DiamondValue *out);

static void json_skip_whitespace(JsonParser *parser) {
    while(parser->pos<parser->length) {
        const char c=parser->source[parser->pos];
        if(c!=' '&&c!='\t'&&c!='\n'&&c!='\r')break;
        parser->pos++;
    }
}

static bool json_utf8_append(StringBuilder *builder,int64_t codepoint) {
    char bytes[4];size_t count;
    if(codepoint<0x80) {
        bytes[0]=(char)codepoint;count=1;
    } else if(codepoint<0x800) {
        bytes[0]=(char)(0xC0|(codepoint>>6));
        bytes[1]=(char)(0x80|(codepoint&0x3F));count=2;
    } else if(codepoint<0x10000) {
        bytes[0]=(char)(0xE0|(codepoint>>12));
        bytes[1]=(char)(0x80|((codepoint>>6)&0x3F));
        bytes[2]=(char)(0x80|(codepoint&0x3F));count=3;
    } else {
        bytes[0]=(char)(0xF0|(codepoint>>18));
        bytes[1]=(char)(0x80|((codepoint>>12)&0x3F));
        bytes[2]=(char)(0x80|((codepoint>>6)&0x3F));
        bytes[3]=(char)(0x80|(codepoint&0x3F));count=4;
    }
    return builder_append(builder,bytes,count);
}

static DiamondVmStatus json_hex4(JsonParser *parser,int *out) {
    if(parser->pos+4>parser->length) {
        snprintf(parser->vm->error,sizeof parser->vm->error,"truncated unicode escape");
        return DIAMOND_VM_JSON_ERROR;
    }
    int value=0;
    for(size_t index=0;index<4;index++) {
        const char ch=parser->source[parser->pos+index];
        int digit;
        if(ch>='0'&&ch<='9')digit=ch-'0';
        else if(ch>='a'&&ch<='f')digit=ch-'a'+10;
        else if(ch>='A'&&ch<='F')digit=ch-'A'+10;
        else {
            snprintf(parser->vm->error,sizeof parser->vm->error,
                "invalid unicode escape hex digit");
            return DIAMOND_VM_JSON_ERROR;
        }
        value=value*16+digit;
    }
    parser->pos+=4;
    *out=value;
    return DIAMOND_VM_OK;
}

/* Parser is positioned right after the "\u" of a unicode escape. Appends
 * the decoded UTF-8 bytes to `builder` and advances past the whole
 * escape -- a high surrogate (0xD800-0xDBFF) consumes a second \uXXXX
 * low-surrogate escape too, combined per RFC 8259 into the single
 * codepoint >= 0x10000 the pair represents. */
static DiamondVmStatus json_unicode_escape(JsonParser *parser,StringBuilder *builder) {
    int code=0;
    DiamondVmStatus status=json_hex4(parser,&code);
    if(status!=DIAMOND_VM_OK)return status;
    if(code>=0xD800&&code<=0xDBFF) {
        if(parser->pos+2>parser->length||parser->source[parser->pos]!='\\'||
           parser->source[parser->pos+1]!='u') {
            snprintf(parser->vm->error,sizeof parser->vm->error,
                "unpaired high surrogate in unicode escape");
            return DIAMOND_VM_JSON_ERROR;
        }
        parser->pos+=2;
        int low=0;
        status=json_hex4(parser,&low);
        if(status!=DIAMOND_VM_OK)return status;
        if(low<0xDC00||low>0xDFFF) {
            snprintf(parser->vm->error,sizeof parser->vm->error,
                "high surrogate not followed by a low surrogate in unicode escape");
            return DIAMOND_VM_JSON_ERROR;
        }
        const int64_t codepoint=0x10000+(((int64_t)code-0xD800)*0x400)+(low-0xDC00);
        return json_utf8_append(builder,codepoint)?DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
    }
    if(code>=0xDC00&&code<=0xDFFF) {
        snprintf(parser->vm->error,sizeof parser->vm->error,
            "unpaired low surrogate in unicode escape");
        return DIAMOND_VM_JSON_ERROR;
    }
    return json_utf8_append(builder,code)?DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
}

static DiamondVmStatus json_parse_string(JsonParser *parser,DiamondValue *out) {
    parser->pos++;
    StringBuilder builder={};
    while(true) {
        if(parser->pos>=parser->length) {
            free(builder.chars);
            snprintf(parser->vm->error,sizeof parser->vm->error,"unterminated string");
            return DIAMOND_VM_JSON_ERROR;
        }
        const char ch=parser->source[parser->pos];
        if(ch=='"') {
            parser->pos++;
            DiamondString *result=allocate_string(parser->vm,
                builder.chars!=nullptr?builder.chars:"",builder.length);
            free(builder.chars);
            if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
            *out=DIAMOND_OBJECT(result);
            return DIAMOND_VM_OK;
        }
        if(ch=='\\') {
            parser->pos++;
            if(parser->pos>=parser->length) {
                free(builder.chars);
                snprintf(parser->vm->error,sizeof parser->vm->error,
                    "unterminated escape sequence");
                return DIAMOND_VM_JSON_ERROR;
            }
            const char escape=parser->source[parser->pos];
            parser->pos++;
            bool ok=true;
            switch(escape) {
                case '"':ok=builder_append(&builder,"\"",1);break;
                case '\\':ok=builder_append(&builder,"\\",1);break;
                case '/':ok=builder_append(&builder,"/",1);break;
                case 'n':ok=builder_append(&builder,"\n",1);break;
                case 'r':ok=builder_append(&builder,"\r",1);break;
                case 't':ok=builder_append(&builder,"\t",1);break;
                case 'b':{const char b=8;ok=builder_append(&builder,&b,1);break;}
                case 'f':{const char f=12;ok=builder_append(&builder,&f,1);break;}
                case 'u':{
                    const DiamondVmStatus status=json_unicode_escape(parser,&builder);
                    if(status!=DIAMOND_VM_OK){free(builder.chars);return status;}
                    break;
                }
                default:
                    free(builder.chars);
                    snprintf(parser->vm->error,sizeof parser->vm->error,
                        "invalid escape character");
                    return DIAMOND_VM_JSON_ERROR;
            }
            if(!ok){free(builder.chars);return DIAMOND_VM_OUT_OF_MEMORY;}
        } else {
            if(!builder_append(&builder,&ch,1)) {
                free(builder.chars);return DIAMOND_VM_OUT_OF_MEMORY;
            }
            parser->pos++;
        }
    }
}

static DiamondVmStatus json_parse_number(JsonParser *parser,DiamondValue *out) {
    const size_t start=parser->pos;
    bool negative=false;
    if(parser->pos<parser->length&&parser->source[parser->pos]=='-') {
        negative=true;parser->pos++;
    }
    const size_t digit_start=parser->pos;
    while(parser->pos<parser->length&&
          parser->source[parser->pos]>='0'&&parser->source[parser->pos]<='9')
        parser->pos++;
    if(parser->pos==digit_start) {
        snprintf(parser->vm->error,sizeof parser->vm->error,
            "invalid number at position %zu",start);
        return DIAMOND_VM_JSON_ERROR;
    }
    bool is_float=false;
    if(parser->pos<parser->length&&parser->source[parser->pos]=='.') {
        is_float=true;parser->pos++;
        const size_t fraction_start=parser->pos;
        while(parser->pos<parser->length&&
              parser->source[parser->pos]>='0'&&parser->source[parser->pos]<='9')
            parser->pos++;
        if(parser->pos==fraction_start) {
            snprintf(parser->vm->error,sizeof parser->vm->error,
                "invalid number at position %zu",start);
            return DIAMOND_VM_JSON_ERROR;
        }
    }
    if(parser->pos<parser->length&&
       (parser->source[parser->pos]=='e'||parser->source[parser->pos]=='E')) {
        is_float=true;parser->pos++;
        if(parser->pos<parser->length&&
           (parser->source[parser->pos]=='+'||parser->source[parser->pos]=='-'))
            parser->pos++;
        const size_t exponent_start=parser->pos;
        while(parser->pos<parser->length&&
              parser->source[parser->pos]>='0'&&parser->source[parser->pos]<='9')
            parser->pos++;
        if(parser->pos==exponent_start) {
            snprintf(parser->vm->error,sizeof parser->vm->error,
                "invalid number at position %zu",start);
            return DIAMOND_VM_JSON_ERROR;
        }
    }
    if(is_float) {
        char *end=nullptr;
        const double value=strtod(parser->source+start,&end);
        *out=DIAMOND_FLOAT(value);
        return DIAMOND_VM_OK;
    }
    int64_t value=0;bool overflowed=false;
    for(size_t index=digit_start;index<parser->pos&&!overflowed;index++) {
        int64_t widened=0;
        if(ckd_mul(&widened,value,(int64_t)10)||
           ckd_add(&value,widened,(int64_t)(parser->source[index]-'0')))
            overflowed=true;
    }
    if(overflowed) {
        const DiamondValue bignum_result=diamond_bignum_from_decimal_digits(
            parser->vm,parser->source+digit_start,parser->pos-digit_start,negative);
        if(bignum_result.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
        *out=bignum_result;
        return DIAMOND_VM_OK;
    }
    *out=DIAMOND_INT(negative?-value:value);
    return DIAMOND_VM_OK;
}

static DiamondVmStatus json_parse_literal(JsonParser *parser,const char *literal,
        size_t literal_length,DiamondValue value,DiamondValue *out) {
    if(parser->pos+literal_length>parser->length||
       memcmp(parser->source+parser->pos,literal,literal_length)!=0) {
        snprintf(parser->vm->error,sizeof parser->vm->error,
            "invalid literal at position %zu",parser->pos);
        return DIAMOND_VM_JSON_ERROR;
    }
    parser->pos+=literal_length;
    *out=value;
    return DIAMOND_VM_OK;
}

static DiamondVmStatus json_parse_array_body(JsonParser *parser,DiamondValue *out) {
    parser->pos++;
    json_skip_whitespace(parser);
    DiamondArray *array=allocate_array(parser->vm,nullptr,0);
    if(array==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!gc_protect(parser->vm,DIAMOND_OBJECT(array)))return DIAMOND_VM_OUT_OF_MEMORY;
    if(parser->pos<parser->length&&parser->source[parser->pos]==']') {
        parser->pos++;
        *out=DIAMOND_OBJECT(array);
        return DIAMOND_VM_OK;
    }
    while(true) {
        DiamondValue element=DIAMOND_NIL;
        const DiamondVmStatus status=json_parse_value(parser,&element);
        if(status!=DIAMOND_VM_OK)return status;
        if(!array_push(parser->vm,array,element))return DIAMOND_VM_OUT_OF_MEMORY;
        json_skip_whitespace(parser);
        if(parser->pos>=parser->length) {
            snprintf(parser->vm->error,sizeof parser->vm->error,"unterminated array");
            return DIAMOND_VM_JSON_ERROR;
        }
        if(parser->source[parser->pos]==',') {
            parser->pos++;
            json_skip_whitespace(parser);
        } else if(parser->source[parser->pos]==']') {
            parser->pos++;
            *out=DIAMOND_OBJECT(array);
            return DIAMOND_VM_OK;
        } else {
            snprintf(parser->vm->error,sizeof parser->vm->error,
                "expected ',' or ']' in array");
            return DIAMOND_VM_JSON_ERROR;
        }
    }
}

/* Depth-guards json_parse_array_body/json_parse_object_body -- see
 * JsonParser's own `depth` field comment for why this exists at all.
 * `parser->depth` counts *current* nesting (incremented on entry,
 * decremented on every exit), not total containers seen, so a wide
 * flat array/object never trips this regardless of its element count --
 * only genuine nesting depth does. */
static DiamondVmStatus json_parse_array(JsonParser *parser,DiamondValue *out) {
    if(parser->depth>=DIAMOND_MAX_CALL_DEPTH)return DIAMOND_VM_STACK_OVERFLOW;
    parser->depth++;
    const DiamondVmStatus status=json_parse_array_body(parser,out);
    parser->depth--;
    return status;
}

static DiamondVmStatus json_parse_object_body(JsonParser *parser,DiamondValue *out) {
    parser->pos++;
    json_skip_whitespace(parser);
    DiamondHash *hash=allocate_hash(parser->vm);
    if(hash==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!gc_protect(parser->vm,DIAMOND_OBJECT(hash)))return DIAMOND_VM_OUT_OF_MEMORY;
    if(parser->pos<parser->length&&parser->source[parser->pos]=='}') {
        parser->pos++;
        *out=DIAMOND_OBJECT(hash);
        return DIAMOND_VM_OK;
    }
    while(true) {
        json_skip_whitespace(parser);
        if(parser->pos>=parser->length||parser->source[parser->pos]!='"') {
            snprintf(parser->vm->error,sizeof parser->vm->error,
                "expected string key in object");
            return DIAMOND_VM_JSON_ERROR;
        }
        DiamondValue key=DIAMOND_NIL;
        DiamondVmStatus status=json_parse_string(parser,&key);
        if(status!=DIAMOND_VM_OK)return status;
        if(!hash_set(parser->vm,hash,key,DIAMOND_NIL))return DIAMOND_VM_OUT_OF_MEMORY;
        json_skip_whitespace(parser);
        if(parser->pos>=parser->length||parser->source[parser->pos]!=':') {
            snprintf(parser->vm->error,sizeof parser->vm->error,
                "expected ':' after object key");
            return DIAMOND_VM_JSON_ERROR;
        }
        parser->pos++;
        json_skip_whitespace(parser);
        DiamondValue value=DIAMOND_NIL;
        status=json_parse_value(parser,&value);
        if(status!=DIAMOND_VM_OK)return status;
        if(!hash_set(parser->vm,hash,key,value))return DIAMOND_VM_OUT_OF_MEMORY;
        json_skip_whitespace(parser);
        if(parser->pos>=parser->length) {
            snprintf(parser->vm->error,sizeof parser->vm->error,"unterminated object");
            return DIAMOND_VM_JSON_ERROR;
        }
        if(parser->source[parser->pos]==',') {
            parser->pos++;
        } else if(parser->source[parser->pos]=='}') {
            parser->pos++;
            *out=DIAMOND_OBJECT(hash);
            return DIAMOND_VM_OK;
        } else {
            snprintf(parser->vm->error,sizeof parser->vm->error,
                "expected ',' or '}' in object");
            return DIAMOND_VM_JSON_ERROR;
        }
    }
}

static DiamondVmStatus json_parse_object(JsonParser *parser,DiamondValue *out) {
    if(parser->depth>=DIAMOND_MAX_CALL_DEPTH)return DIAMOND_VM_STACK_OVERFLOW;
    parser->depth++;
    const DiamondVmStatus status=json_parse_object_body(parser,out);
    parser->depth--;
    return status;
}

static DiamondVmStatus json_parse_value(JsonParser *parser,DiamondValue *out) {
    json_skip_whitespace(parser);
    if(parser->pos>=parser->length) {
        snprintf(parser->vm->error,sizeof parser->vm->error,"unexpected end of input");
        return DIAMOND_VM_JSON_ERROR;
    }
    const char ch=parser->source[parser->pos];
    if(ch=='{')return json_parse_object(parser,out);
    if(ch=='[')return json_parse_array(parser,out);
    if(ch=='"')return json_parse_string(parser,out);
    if(ch=='t')return json_parse_literal(parser,"true",4,DIAMOND_BOOL(true),out);
    if(ch=='f')return json_parse_literal(parser,"false",5,DIAMOND_BOOL(false),out);
    if(ch=='n')return json_parse_literal(parser,"null",4,DIAMOND_NIL,out);
    if(ch=='-'||(ch>='0'&&ch<='9'))return json_parse_number(parser,out);
    snprintf(parser->vm->error,sizeof parser->vm->error,
        "unexpected character at position %zu",parser->pos);
    return DIAMOND_VM_JSON_ERROR;
}

/* Top-level entry: one value, then trailing-content rejection, matching
 * JSONCodec#parse's own "parsed = parse_value(...); pos =
 * skip_whitespace(...); pos != length -> error" shape exactly. */
DiamondVmStatus json_parse_document(DiamondVm *vm,const char *source,
        size_t length,DiamondValue *out) {
    JsonParser parser={.vm=vm,.source=source,.length=length,.pos=0};
    const DiamondVmStatus status=json_parse_value(&parser,out);
    if(status!=DIAMOND_VM_OK)return status;
    json_skip_whitespace(&parser);
    if(parser.pos!=parser.length) {
        snprintf(vm->error,sizeof vm->error,"trailing content after JSON value");
        return DIAMOND_VM_JSON_ERROR;
    }
    return DIAMOND_VM_OK;
}

enum { JSON_MAX_INDENT = 16 };

typedef struct JsonWriter {
    DiamondVm *vm;
    const DiamondChunk *chunk;
    size_t call_depth;
    size_t nesting;
    StringBuilder out;
    /* JSON.stringify options: a non-empty `indent` lays the document out one element per line,
     * and `sort_keys` writes each Hash's keys in sorted order. */
    char indent[JSON_MAX_INDENT+1];
    size_t indent_length;
    bool sort_keys;
} JsonWriter;

static DiamondVmStatus json_write_value(JsonWriter *writer,DiamondValue value);

/* A newline and `level` copies of the indent, when the document is being laid out. */
static bool json_write_break(JsonWriter *writer,size_t level) {
    if(writer->indent_length==0)return true;
    if(!builder_append(&writer->out,"\n",1))return false;
    for(size_t index=0;index<level;index++)
        if(!builder_append(&writer->out,writer->indent,writer->indent_length))return false;
    return true;
}

static bool json_write_string(StringBuilder *out,const char *chars,size_t length) {
    static const char hex[]="0123456789abcdef";
    if(!builder_append(out,"\"",1))return false;
    size_t run=0;
    for(size_t index=0;index<length;index++) {
        const unsigned char c=(unsigned char)chars[index];
        if(c>=0x20&&c!='"'&&c!='\\')continue;
        if(index>run&&!builder_append(out,chars+run,index-run))return false;
        run=index+1;
        bool ok;
        switch(c) {
            case '"':ok=builder_append(out,"\\\"",2);break;
            case '\\':ok=builder_append(out,"\\\\",2);break;
            case '\n':ok=builder_append(out,"\\n",2);break;
            case '\r':ok=builder_append(out,"\\r",2);break;
            case '\t':ok=builder_append(out,"\\t",2);break;
            default: {
                const char escape[6]={'\\','u','0','0',hex[c>>4],hex[c&15]};
                ok=builder_append(out,escape,6);
            }
        }
        if(!ok)return false;
    }
    if(length>run&&!builder_append(out,chars+run,length-run))return false;
    return builder_append(out,"\"",1);
}

static DiamondVmStatus json_write_hash_key(JsonWriter *writer,DiamondValue key) {
    StringBuilder *out=&writer->out;
    if(key.kind==DIAMOND_VALUE_OBJECT) {
        const DiamondObject *object=key.as.object;
        if(object->kind==DIAMOND_OBJECT_STRING) {
            const DiamondString *string=(const DiamondString *)object;
            return json_write_string(out,string->chars,string->length)?
                DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
        }
        if(object->kind==DIAMOND_OBJECT_SYMBOL) {
            const DiamondSymbol *symbol=(const DiamondSymbol *)object;
            return json_write_string(out,symbol->chars,symbol->length)?
                DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    /* Everything else is "#{key}" -- the same conversion string
     * interpolation does, including a user class's own to_s. */
    DiamondValue text=DIAMOND_NIL;
    const DiamondVmStatus status=stringify_value(writer->vm,writer->chunk,
        writer->call_depth,key,&text);
    if(status!=DIAMOND_VM_OK)return status;
    const DiamondString *string=(const DiamondString *)text.as.object;
    return json_write_string(out,string->chars,string->length)?
        DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
}

/* The text a Hash key is written as, for sorting: a String or Symbol is itself, anything else
 * is its string form (the same conversion json_write_hash_key uses). */
typedef struct JsonKeySort {
    char *chars;
    size_t length;
    size_t index;
} JsonKeySort;

static int json_key_compare(const void *left_pointer,const void *right_pointer) {
    const JsonKeySort *left=left_pointer,*right=right_pointer;
    const size_t shared=left->length<right->length?left->length:right->length;
    const int order=shared==0?0:memcmp(left->chars,right->chars,shared);
    if(order!=0)return order;
    if(left->length!=right->length)return left->length<right->length?-1:1;
    return left->index<right->index?-1:left->index>right->index?1:0;
}

/* Entry indices of `hash` in ascending key order (bytewise; equal text keeps insertion order).
 * `*order` is malloc'd for the caller to free. */
static DiamondVmStatus json_sorted_order(JsonWriter *writer,const DiamondHash *hash,size_t **order) {
    const size_t count=hash->count;
    JsonKeySort *keys=calloc(count,sizeof *keys);
    size_t *result=malloc(count*sizeof *result);
    if(keys==nullptr||result==nullptr) {
        free(keys);free(result);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondVmStatus status=DIAMOND_VM_OK;
    for(size_t index=0;index<count&&status==DIAMOND_VM_OK;index++) {
        if(index>=hash->count) {
            keys[index].index=index;
            continue;
        }
        const DiamondValue key=hash->entries[index].key;
        DiamondValue text=key;
        if(!(key.kind==DIAMOND_VALUE_OBJECT&&(key.as.object->kind==DIAMOND_OBJECT_STRING||
                                              key.as.object->kind==DIAMOND_OBJECT_SYMBOL))) {
            status=stringify_value(writer->vm,writer->chunk,writer->call_depth,key,&text);
            if(status!=DIAMOND_VM_OK)break;
        }
        const char *chars;
        size_t length;
        if(text.as.object->kind==DIAMOND_OBJECT_SYMBOL) {
            chars=((const DiamondSymbol *)text.as.object)->chars;
            length=((const DiamondSymbol *)text.as.object)->length;
        } else {
            chars=((const DiamondString *)text.as.object)->chars;
            length=((const DiamondString *)text.as.object)->length;
        }
        keys[index].chars=malloc(length>0?length:1);
        if(keys[index].chars==nullptr) {
            status=DIAMOND_VM_OUT_OF_MEMORY;
            break;
        }
        memcpy(keys[index].chars,chars,length);
        keys[index].length=length;
        keys[index].index=index;
    }
    if(status==DIAMOND_VM_OK) {
        qsort(keys,count,sizeof *keys,json_key_compare);
        for(size_t position=0;position<count;position++)result[position]=keys[position].index;
    }
    for(size_t index=0;index<count;index++)free(keys[index].chars);
    free(keys);
    if(status!=DIAMOND_VM_OK) {
        free(result);
        return status;
    }
    *order=result;
    return DIAMOND_VM_OK;
}

static DiamondVmStatus json_write_value(JsonWriter *writer,DiamondValue value) {
    StringBuilder *out=&writer->out;
    switch(value.kind) {
        case DIAMOND_VALUE_NIL:
            return builder_append(out,"null",4)?DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
        case DIAMOND_VALUE_BOOL:
            return builder_append(out,value.as.boolean?"true":"false",
                value.as.boolean?4:5)?DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
        case DIAMOND_VALUE_INT:
            return builder_format_value(out,value)?DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
        case DIAMOND_VALUE_FLOAT:
            /* JSON has no NaN or Infinity. */
            if(isnan(value.as.real)||isinf(value.as.real)) {
                snprintf(writer->vm->error,sizeof writer->vm->error,
                    "cannot convert %s to JSON",isnan(value.as.real)?"NaN":
                    value.as.real<0?"-Infinity":"Infinity");
                return DIAMOND_VM_JSON_ERROR;
            }
            return builder_format_value(out,value)?DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
        case DIAMOND_VALUE_OBJECT:
            break;
        default:
            snprintf(writer->vm->error,sizeof writer->vm->error,
                "cannot convert this value to JSON");
            return DIAMOND_VM_JSON_ERROR;
    }
    if(value_is_bignum(value))
        return builder_format_value(out,value)?DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
    const DiamondObject *object=value.as.object;
    if(object->kind==DIAMOND_OBJECT_STRING) {
        const DiamondString *string=(const DiamondString *)object;
        return json_write_string(out,string->chars,string->length)?
            DIAMOND_VM_OK:DIAMOND_VM_OUT_OF_MEMORY;
    }
    if(object->kind!=DIAMOND_OBJECT_ARRAY&&object->kind!=DIAMOND_OBJECT_HASH) {
        snprintf(writer->vm->error,sizeof writer->vm->error,
            "cannot convert this value to JSON");
        return DIAMOND_VM_JSON_ERROR;
    }
    if(writer->nesting>=DIAMOND_MAX_CALL_DEPTH)return DIAMOND_VM_STACK_OVERFLOW;
    writer->nesting++;
    DiamondVmStatus status=DIAMOND_VM_OK;
    if(object->kind==DIAMOND_OBJECT_ARRAY) {
        const DiamondArray *array=(const DiamondArray *)object;
        if(!builder_append(out,"[",1))status=DIAMOND_VM_OUT_OF_MEMORY;
        for(size_t index=0;status==DIAMOND_VM_OK&&index<array->count;index++) {
            if((index>0&&!builder_append(out,",",1))||!json_write_break(writer,writer->nesting)) {
                status=DIAMOND_VM_OUT_OF_MEMORY;break;
            }
            status=json_write_value(writer,array->values[index]);
        }
        if(status==DIAMOND_VM_OK&&array->count>0&&!json_write_break(writer,writer->nesting-1))
            status=DIAMOND_VM_OUT_OF_MEMORY;
        if(status==DIAMOND_VM_OK&&!builder_append(out,"]",1))
            status=DIAMOND_VM_OUT_OF_MEMORY;
    } else {
        const DiamondHash *hash=(const DiamondHash *)object;
        size_t *order=nullptr;
        const size_t entry_count=hash->count;
        if(writer->sort_keys&&entry_count>1) {
            status=json_sorted_order(writer,hash,&order);
        }
        if(status==DIAMOND_VM_OK&&!builder_append(out,"{",1))status=DIAMOND_VM_OUT_OF_MEMORY;
        for(size_t position=0;status==DIAMOND_VM_OK&&position<entry_count;position++) {
            const size_t index=order!=nullptr?order[position]:position;
            /* A key's to_s may have shrunk the Hash. */
            if(index>=hash->count)break;
            if((position>0&&!builder_append(out,",",1))||!json_write_break(writer,writer->nesting)) {
                status=DIAMOND_VM_OUT_OF_MEMORY;break;
            }
            status=json_write_hash_key(writer,hash->entries[index].key);
            if(status!=DIAMOND_VM_OK)break;
            if(!builder_append(out,writer->indent_length>0?": ":":",writer->indent_length>0?2:1)) {
                status=DIAMOND_VM_OUT_OF_MEMORY;break;
            }
            if(index>=hash->count)break;
            status=json_write_value(writer,hash->entries[index].value);
        }
        free(order);
        if(status==DIAMOND_VM_OK&&entry_count>0&&!json_write_break(writer,writer->nesting-1))
            status=DIAMOND_VM_OUT_OF_MEMORY;
        if(status==DIAMOND_VM_OK&&!builder_append(out,"}",1))
            status=DIAMOND_VM_OUT_OF_MEMORY;
    }
    writer->nesting--;
    return status;
}

/* JSON.stringify's single entry point; `*result` is a fresh String. */
static DiamondVmStatus json_type_error(DiamondVm *vm,const char *message) {
    snprintf(vm->error,sizeof vm->error,"%s",message);
    return DIAMOND_VM_TYPE_ERROR;
}

/* Reads JSON.stringify's options Hash into `writer`: "indent" is an Int number of spaces (0 for
 * none, at most JSON_MAX_INDENT) or a String of JSON whitespace; "sort_keys" is a Bool. Keys may
 * be Strings or Symbols. nil means no options. */
static DiamondVmStatus json_read_options(JsonWriter *writer,DiamondValue options) {
    if(options.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OK;
    if(options.kind!=DIAMOND_VALUE_OBJECT||options.as.object->kind!=DIAMOND_OBJECT_HASH)
        return json_type_error(writer->vm,"JSON.stringify options must be a Hash");
    const DiamondHash *hash=(const DiamondHash *)options.as.object;
    for(size_t index=0;index<hash->count;index++) {
        const DiamondValue key=hash->entries[index].key;
        const DiamondValue value=hash->entries[index].value;
        const char *name=nullptr;
        size_t name_length=0;
        if(key.kind==DIAMOND_VALUE_OBJECT&&key.as.object->kind==DIAMOND_OBJECT_STRING) {
            name=((const DiamondString *)key.as.object)->chars;
            name_length=((const DiamondString *)key.as.object)->length;
        } else if(key.kind==DIAMOND_VALUE_OBJECT&&key.as.object->kind==DIAMOND_OBJECT_SYMBOL) {
            name=((const DiamondSymbol *)key.as.object)->chars;
            name_length=((const DiamondSymbol *)key.as.object)->length;
        }
        if(name!=nullptr&&name_length==6&&memcmp(name,"indent",6)==0) {
            if(value.kind==DIAMOND_VALUE_NIL)continue;
            if(value.kind==DIAMOND_VALUE_INT) {
                if(value.as.integer<0||value.as.integer>JSON_MAX_INDENT)
                    return json_type_error(writer->vm,"JSON.stringify indent must be between 0 and 16 spaces");
                writer->indent_length=(size_t)value.as.integer;
                memset(writer->indent,' ',writer->indent_length);
            } else if(value.kind==DIAMOND_VALUE_OBJECT&&value.as.object->kind==DIAMOND_OBJECT_STRING) {
                const DiamondString *text=(const DiamondString *)value.as.object;
                if(text->length>JSON_MAX_INDENT)
                    return json_type_error(writer->vm,"JSON.stringify indent is longer than 16 characters");
                for(size_t at=0;at<text->length;at++) {
                    const char c=text->chars[at];
                    if(c!=' '&&c!='\t')
                        return json_type_error(writer->vm,"JSON.stringify indent must be spaces or tabs");
                }
                writer->indent_length=text->length;
                memcpy(writer->indent,text->chars,text->length);
            } else {
                return json_type_error(writer->vm,"JSON.stringify indent must be an Int or a String");
            }
        } else if(name!=nullptr&&name_length==9&&memcmp(name,"sort_keys",9)==0) {
            if(value.kind==DIAMOND_VALUE_NIL)continue;
            if(value.kind!=DIAMOND_VALUE_BOOL)
                return json_type_error(writer->vm,"JSON.stringify sort_keys must be true or false");
            writer->sort_keys=value.as.boolean;
        } else {
            return json_type_error(writer->vm,"unknown JSON.stringify option (expected indent or sort_keys)");
        }
    }
    return DIAMOND_VM_OK;
}

DiamondVmStatus json_stringify_document(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,DiamondValue value,DiamondValue options,DiamondValue *result) {
    JsonWriter writer={.vm=vm,.chunk=chunk,.call_depth=depth};
    const DiamondVmStatus options_status=json_read_options(&writer,options);
    if(options_status!=DIAMOND_VM_OK)return options_status;
    DiamondVmStatus status=json_write_value(&writer,value);
    if(status==DIAMOND_VM_OK) {
        DiamondString *text=allocate_string(vm,writer.out.chars!=nullptr?
            writer.out.chars:"",writer.out.length);
        if(text==nullptr)status=DIAMOND_VM_OUT_OF_MEMORY;
        else *result=DIAMOND_OBJECT(text);
    }
    free(writer.out.chars);
    return status;
}

/* Appends `chars`/`length` to `buffer` as one double-quoted, escaped JSON
 * string literal -- same escaping rules (and the same six named escapes
 * plus \u00XX for every other control character) as lsp/json.c's own
 * writer_append_string_literal, reimplemented independently here rather
 * than shared: see debugger_structured_helper's own comment for why the
 * core VM doesn't link lsp/json.c at all. */
bool debug_json_append_escaped_string(GrowBuffer *buffer,
        const char *chars,size_t length) {
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\""))return false;
    for(size_t index=0;index<length;index++) {
        const unsigned char c=(unsigned char)chars[index];
        switch(c) {
            case '"':if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\\\""))return false;break;
            case '\\':if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\\\\"))return false;break;
            case '\b':if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\\b"))return false;break;
            case '\f':if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\\f"))return false;break;
            case '\n':if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\\n"))return false;break;
            case '\r':if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\\r"))return false;break;
            case '\t':if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\\t"))return false;break;
            default:
                if(c<0x20) {
                    char escape[8];
                    const int written=snprintf(escape,sizeof escape,"\\u%04x",c);
                    if(written<0||!grow_buffer_append(buffer,escape,(size_t)written))
                        return false;
                } else if(!grow_buffer_append(buffer,(const char *)&chars[index],1)) {
                    return false;
                }
        }
    }
    return GROW_BUFFER_APPEND_LITERAL(buffer,"\"");
}
