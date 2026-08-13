#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JsonValue *json_alloc(JsonKind kind) {
    JsonValue *value=calloc(1,sizeof(JsonValue));
    if(value==nullptr)return nullptr;
    value->kind=kind;
    return value;
}

JsonValue *json_null(void) {
    return json_alloc(JSON_NULL);
}

JsonValue *json_bool(bool value_) {
    JsonValue *value=json_alloc(JSON_BOOL);
    if(value==nullptr)return nullptr;
    value->as.boolean=value_;
    return value;
}

JsonValue *json_number(double value_) {
    JsonValue *value=json_alloc(JSON_NUMBER);
    if(value==nullptr)return nullptr;
    value->as.number=value_;
    return value;
}

JsonValue *json_string(const char *chars,size_t length) {
    JsonValue *value=json_alloc(JSON_STRING);
    if(value==nullptr)return nullptr;
    char *copy=malloc(length+1);
    if(copy==nullptr) {
        free(value);
        return nullptr;
    }
    memcpy(copy,chars,length);
    copy[length]='\0';
    value->as.string.chars=copy;
    value->as.string.length=length;
    return value;
}

JsonValue *json_string_z(const char *chars) {
    return json_string(chars,strlen(chars));
}

JsonValue *json_array(void) {
    return json_alloc(JSON_ARRAY);
}

JsonValue *json_object(void) {
    return json_alloc(JSON_OBJECT);
}

bool json_array_push(JsonValue *array,JsonValue *item) {
    if(array->as.array.count==array->as.array.capacity) {
        size_t capacity=array->as.array.capacity==0?8:array->as.array.capacity*2;
        JsonValue **grown=realloc(array->as.array.items,capacity*sizeof(JsonValue *));
        if(grown==nullptr)return false;
        array->as.array.items=grown;
        array->as.array.capacity=capacity;
    }
    array->as.array.items[array->as.array.count++]=item;
    return true;
}

bool json_object_set(JsonValue *object,const char *key,JsonValue *value) {
    if(object->as.object.count==object->as.object.capacity) {
        size_t capacity=object->as.object.capacity==0?8:object->as.object.capacity*2;
        JsonMember *grown=realloc(object->as.object.members,capacity*sizeof(JsonMember));
        if(grown==nullptr)return false;
        object->as.object.members=grown;
        object->as.object.capacity=capacity;
    }
    char *key_copy=malloc(strlen(key)+1);
    if(key_copy==nullptr)return false;
    strcpy(key_copy,key);
    object->as.object.members[object->as.object.count++]=(JsonMember){
        .key=key_copy,.value=value};
    return true;
}

const JsonValue *json_object_get(const JsonValue *object,const char *key) {
    if(object==nullptr||object->kind!=JSON_OBJECT)return nullptr;
    for(size_t index=0;index<object->as.object.count;index++)
        if(strcmp(object->as.object.members[index].key,key)==0)
            return object->as.object.members[index].value;
    return nullptr;
}

bool json_as_string(const JsonValue *value,const char **chars,size_t *length) {
    if(value==nullptr||value->kind!=JSON_STRING)return false;
    *chars=value->as.string.chars;
    *length=value->as.string.length;
    return true;
}

bool json_as_number(const JsonValue *value,double *out) {
    if(value==nullptr||value->kind!=JSON_NUMBER)return false;
    *out=value->as.number;
    return true;
}

bool json_as_bool(const JsonValue *value,bool *out) {
    if(value==nullptr||value->kind!=JSON_BOOL)return false;
    *out=value->as.boolean;
    return true;
}

void json_free(JsonValue *value) {
    if(value==nullptr)return;
    switch(value->kind) {
        case JSON_STRING:
            free(value->as.string.chars);
            break;
        case JSON_ARRAY:
            for(size_t index=0;index<value->as.array.count;index++)
                json_free(value->as.array.items[index]);
            free(value->as.array.items);
            break;
        case JSON_OBJECT:
            for(size_t index=0;index<value->as.object.count;index++) {
                free(value->as.object.members[index].key);
                json_free(value->as.object.members[index].value);
            }
            free(value->as.object.members);
            break;
        default:
            break;
    }
    free(value);
}

/* --- parsing --- */

typedef struct Parser {
    const char *source;
    size_t length;
    size_t offset;
    const char *error;
} Parser;

static void skip_whitespace(Parser *parser) {
    while(parser->offset<parser->length) {
        const char c=parser->source[parser->offset];
        if(c==' '||c=='\t'||c=='\n'||c=='\r') {
            parser->offset++;
        } else break;
    }
}

static bool parser_at_end(const Parser *parser) {
    return parser->offset>=parser->length;
}

static char parser_peek(const Parser *parser) {
    return parser_at_end(parser)?'\0':parser->source[parser->offset];
}

static JsonValue *parse_value(Parser *parser);

static bool parse_hex4(Parser *parser,unsigned *out) {
    if(parser->offset+4>parser->length)return false;
    unsigned value=0;
    for(size_t index=0;index<4;index++) {
        const char c=parser->source[parser->offset+index];
        value<<=4;
        if(c>='0'&&c<='9')value|=(unsigned)(c-'0');
        else if(c>='a'&&c<='f')value|=(unsigned)(c-'a'+10);
        else if(c>='A'&&c<='F')value|=(unsigned)(c-'A'+10);
        else return false;
    }
    parser->offset+=4;
    *out=value;
    return true;
}

/* Encodes a Unicode code point as UTF-8 into builder -- \uXXXX escapes
 * (including surrogate pairs, since LSP text sent by real editors can
 * contain any Unicode character in string values) need this rather than
 * passing the raw 16-bit units through. */
static bool append_utf8(char **chars,size_t *length,size_t *capacity,unsigned code_point) {
    unsigned char bytes[4];
    size_t count;
    if(code_point<0x80) {
        bytes[0]=(unsigned char)code_point;
        count=1;
    } else if(code_point<0x800) {
        bytes[0]=(unsigned char)(0xC0|(code_point>>6));
        bytes[1]=(unsigned char)(0x80|(code_point&0x3F));
        count=2;
    } else if(code_point<0x10000) {
        bytes[0]=(unsigned char)(0xE0|(code_point>>12));
        bytes[1]=(unsigned char)(0x80|((code_point>>6)&0x3F));
        bytes[2]=(unsigned char)(0x80|(code_point&0x3F));
        count=3;
    } else {
        bytes[0]=(unsigned char)(0xF0|(code_point>>18));
        bytes[1]=(unsigned char)(0x80|((code_point>>12)&0x3F));
        bytes[2]=(unsigned char)(0x80|((code_point>>6)&0x3F));
        bytes[3]=(unsigned char)(0x80|(code_point&0x3F));
        count=4;
    }
    if(*length+count+1>*capacity) {
        size_t grown_capacity=*capacity==0?32:*capacity;
        while(grown_capacity<*length+count+1)grown_capacity*=2;
        char *grown=realloc(*chars,grown_capacity);
        if(grown==nullptr)return false;
        *chars=grown;
        *capacity=grown_capacity;
    }
    memcpy(*chars+*length,bytes,count);
    *length+=count;
    return true;
}

static JsonValue *parse_string(Parser *parser) {
    parser->offset++;
    char *chars=nullptr;
    size_t length=0,capacity=0;
    while(true) {
        if(parser_at_end(parser)) {
            parser->error="unterminated string";
            free(chars);
            return nullptr;
        }
        const char c=parser->source[parser->offset];
        if(c=='"') {
            parser->offset++;
            break;
        }
        if(c=='\\') {
            parser->offset++;
            if(parser_at_end(parser)) {
                parser->error="unterminated escape";
                free(chars);
                return nullptr;
            }
            const char escape=parser->source[parser->offset++];
            unsigned code_point;
            switch(escape) {
                case '"':code_point='"';break;
                case '\\':code_point='\\';break;
                case '/':code_point='/';break;
                case 'b':code_point='\b';break;
                case 'f':code_point='\f';break;
                case 'n':code_point='\n';break;
                case 'r':code_point='\r';break;
                case 't':code_point='\t';break;
                case 'u': {
                    if(!parse_hex4(parser,&code_point)) {
                        parser->error="invalid \\u escape";
                        free(chars);
                        return nullptr;
                    }
                    if(code_point>=0xD800&&code_point<=0xDBFF&&
                       parser->offset+1<parser->length&&
                       parser->source[parser->offset]=='\\'&&
                       parser->source[parser->offset+1]=='u') {
                        size_t saved=parser->offset;
                        parser->offset+=2;
                        unsigned low;
                        if(parse_hex4(parser,&low)&&low>=0xDC00&&low<=0xDFFF) {
                            code_point=0x10000+((code_point-0xD800)<<10)+(low-0xDC00);
                        } else {
                            parser->offset=saved;
                        }
                    }
                    break;
                }
                default:
                    parser->error="invalid escape character";
                    free(chars);
                    return nullptr;
            }
            if(!append_utf8(&chars,&length,&capacity,code_point)) {
                parser->error="out of memory";
                free(chars);
                return nullptr;
            }
            continue;
        }
        if(!append_utf8(&chars,&length,&capacity,(unsigned char)c)) {
            parser->error="out of memory";
            free(chars);
            return nullptr;
        }
        parser->offset++;
    }
    JsonValue *value=json_alloc(JSON_STRING);
    if(value==nullptr) {
        free(chars);
        parser->error="out of memory";
        return nullptr;
    }
    if(chars==nullptr) {
        chars=malloc(1);
        if(chars==nullptr) {
            free(value);
            parser->error="out of memory";
            return nullptr;
        }
    }
    chars[length]='\0';
    value->as.string.chars=chars;
    value->as.string.length=length;
    return value;
}

static JsonValue *parse_number(Parser *parser) {
    const size_t start=parser->offset;
    if(parser_peek(parser)=='-')parser->offset++;
    if(parser_at_end(parser)||parser->source[parser->offset]<'0'||
       parser->source[parser->offset]>'9') {
        parser->error="invalid number";
        return nullptr;
    }
    while(!parser_at_end(parser)&&parser->source[parser->offset]>='0'&&
          parser->source[parser->offset]<='9')
        parser->offset++;
    if(parser_peek(parser)=='.') {
        parser->offset++;
        while(!parser_at_end(parser)&&parser->source[parser->offset]>='0'&&
              parser->source[parser->offset]<='9')
            parser->offset++;
    }
    if(parser_peek(parser)=='e'||parser_peek(parser)=='E') {
        parser->offset++;
        if(parser_peek(parser)=='+'||parser_peek(parser)=='-')parser->offset++;
        while(!parser_at_end(parser)&&parser->source[parser->offset]>='0'&&
              parser->source[parser->offset]<='9')
            parser->offset++;
    }
    char buffer[64];
    const size_t token_length=parser->offset-start;
    if(token_length>=sizeof buffer) {
        parser->error="number literal too long";
        return nullptr;
    }
    memcpy(buffer,parser->source+start,token_length);
    buffer[token_length]='\0';
    return json_number(strtod(buffer,nullptr));
}

static bool match_literal(Parser *parser,const char *literal) {
    const size_t literal_length=strlen(literal);
    if(parser->offset+literal_length>parser->length)return false;
    if(memcmp(parser->source+parser->offset,literal,literal_length)!=0)return false;
    parser->offset+=literal_length;
    return true;
}

static JsonValue *parse_array(Parser *parser) {
    parser->offset++;
    JsonValue *array=json_array();
    if(array==nullptr) {
        parser->error="out of memory";
        return nullptr;
    }
    skip_whitespace(parser);
    if(parser_peek(parser)==']') {
        parser->offset++;
        return array;
    }
    while(true) {
        skip_whitespace(parser);
        JsonValue *item=parse_value(parser);
        if(item==nullptr) {
            json_free(array);
            return nullptr;
        }
        if(!json_array_push(array,item)) {
            json_free(item);
            json_free(array);
            parser->error="out of memory";
            return nullptr;
        }
        skip_whitespace(parser);
        if(parser_peek(parser)==',') {
            parser->offset++;
            continue;
        }
        if(parser_peek(parser)==']') {
            parser->offset++;
            return array;
        }
        parser->error="expected ',' or ']' in array";
        json_free(array);
        return nullptr;
    }
}

static JsonValue *parse_object(Parser *parser) {
    parser->offset++;
    JsonValue *object=json_object();
    if(object==nullptr) {
        parser->error="out of memory";
        return nullptr;
    }
    skip_whitespace(parser);
    if(parser_peek(parser)=='}') {
        parser->offset++;
        return object;
    }
    while(true) {
        skip_whitespace(parser);
        if(parser_peek(parser)!='"') {
            parser->error="expected string key in object";
            json_free(object);
            return nullptr;
        }
        JsonValue *key_value=parse_string(parser);
        if(key_value==nullptr) {
            json_free(object);
            return nullptr;
        }
        skip_whitespace(parser);
        if(parser_peek(parser)!=':') {
            parser->error="expected ':' after object key";
            json_free(key_value);
            json_free(object);
            return nullptr;
        }
        parser->offset++;
        skip_whitespace(parser);
        JsonValue *member_value=parse_value(parser);
        if(member_value==nullptr) {
            json_free(key_value);
            json_free(object);
            return nullptr;
        }
        const bool set=json_object_set(object,key_value->as.string.chars,member_value);
        json_free(key_value);
        if(!set) {
            json_free(member_value);
            json_free(object);
            parser->error="out of memory";
            return nullptr;
        }
        skip_whitespace(parser);
        if(parser_peek(parser)==',') {
            parser->offset++;
            continue;
        }
        if(parser_peek(parser)=='}') {
            parser->offset++;
            return object;
        }
        parser->error="expected ',' or '}' in object";
        json_free(object);
        return nullptr;
    }
}

static JsonValue *parse_value(Parser *parser) {
    skip_whitespace(parser);
    if(parser_at_end(parser)) {
        parser->error="unexpected end of input";
        return nullptr;
    }
    const char c=parser_peek(parser);
    if(c=='{')return parse_object(parser);
    if(c=='[')return parse_array(parser);
    if(c=='"')return parse_string(parser);
    if(c=='t') {
        if(match_literal(parser,"true"))return json_bool(true);
        parser->error="invalid literal";
        return nullptr;
    }
    if(c=='f') {
        if(match_literal(parser,"false"))return json_bool(false);
        parser->error="invalid literal";
        return nullptr;
    }
    if(c=='n') {
        if(match_literal(parser,"null"))return json_null();
        parser->error="invalid literal";
        return nullptr;
    }
    if(c=='-'||(c>='0'&&c<='9'))return parse_number(parser);
    parser->error="unexpected character";
    return nullptr;
}

JsonValue *json_parse(const char *source,size_t length,const char **error) {
    Parser parser={.source=source,.length=length,.offset=0,.error=nullptr};
    JsonValue *value=parse_value(&parser);
    if(value==nullptr) {
        if(error!=nullptr)*error=parser.error==nullptr?"parse error":parser.error;
        return nullptr;
    }
    skip_whitespace(&parser);
    if(!parser_at_end(&parser)) {
        json_free(value);
        if(error!=nullptr)*error="trailing data after JSON value";
        return nullptr;
    }
    return value;
}

/* --- serialization --- */

typedef struct Writer {
    char *chars;
    size_t length;
    size_t capacity;
    bool failed;
} Writer;

static void writer_reserve(Writer *writer,size_t additional) {
    if(writer->failed)return;
    if(writer->length+additional+1<=writer->capacity)return;
    size_t capacity=writer->capacity==0?128:writer->capacity;
    while(capacity<writer->length+additional+1)capacity*=2;
    char *grown=realloc(writer->chars,capacity);
    if(grown==nullptr) {
        writer->failed=true;
        return;
    }
    writer->chars=grown;
    writer->capacity=capacity;
}

static void writer_append(Writer *writer,const char *chars,size_t length) {
    writer_reserve(writer,length);
    if(writer->failed)return;
    memcpy(writer->chars+writer->length,chars,length);
    writer->length+=length;
}

static void writer_append_z(Writer *writer,const char *chars) {
    writer_append(writer,chars,strlen(chars));
}

static void writer_append_string_literal(Writer *writer,const char *chars,size_t length) {
    writer_append(writer,"\"",1);
    for(size_t index=0;index<length;index++) {
        const unsigned char c=(unsigned char)chars[index];
        switch(c) {
            case '"':writer_append(writer,"\\\"",2);break;
            case '\\':writer_append(writer,"\\\\",2);break;
            case '\b':writer_append(writer,"\\b",2);break;
            case '\f':writer_append(writer,"\\f",2);break;
            case '\n':writer_append(writer,"\\n",2);break;
            case '\r':writer_append(writer,"\\r",2);break;
            case '\t':writer_append(writer,"\\t",2);break;
            default:
                if(c<0x20) {
                    char escape[7];
                    snprintf(escape,sizeof escape,"\\u%04x",c);
                    writer_append(writer,escape,6);
                } else {
                    writer_append(writer,(const char *)&chars[index],1);
                }
        }
    }
    writer_append(writer,"\"",1);
}

static void write_value(Writer *writer,const JsonValue *value) {
    if(writer->failed)return;
    switch(value->kind) {
        case JSON_NULL:
            writer_append_z(writer,"null");
            break;
        case JSON_BOOL:
            writer_append_z(writer,value->as.boolean?"true":"false");
            break;
        case JSON_NUMBER: {
            char buffer[32];
            const double number=value->as.number;
            /* Every number an LSP message actually carries is a
             * non-negative integer (ids, positions, counts) -- printing
             * without a fractional part when the value is exactly
             * integral keeps `"line": 4` instead of `"line": 4.0`,
             * which some clients parse more strictly than others. */
            if(number==(double)(long long)number)
                snprintf(buffer,sizeof buffer,"%lld",(long long)number);
            else
                snprintf(buffer,sizeof buffer,"%g",number);
            writer_append_z(writer,buffer);
            break;
        }
        case JSON_STRING:
            writer_append_string_literal(writer,value->as.string.chars,value->as.string.length);
            break;
        case JSON_ARRAY:
            writer_append(writer,"[",1);
            for(size_t index=0;index<value->as.array.count;index++) {
                if(index>0)writer_append(writer,",",1);
                write_value(writer,value->as.array.items[index]);
            }
            writer_append(writer,"]",1);
            break;
        case JSON_OBJECT:
            writer_append(writer,"{",1);
            for(size_t index=0;index<value->as.object.count;index++) {
                if(index>0)writer_append(writer,",",1);
                const JsonMember *member=&value->as.object.members[index];
                writer_append_string_literal(writer,member->key,strlen(member->key));
                writer_append(writer,":",1);
                write_value(writer,member->value);
            }
            writer_append(writer,"}",1);
            break;
    }
}

char *json_serialize(const JsonValue *value,size_t *out_length) {
    Writer writer={};
    write_value(&writer,value);
    if(writer.failed) {
        free(writer.chars);
        return nullptr;
    }
    writer_reserve(&writer,0);
    if(writer.failed) {
        free(writer.chars);
        return nullptr;
    }
    if(writer.chars==nullptr) {
        writer.chars=malloc(1);
        if(writer.chars==nullptr)return nullptr;
    }
    writer.chars[writer.length]='\0';
    if(out_length!=nullptr)*out_length=writer.length;
    return writer.chars;
}
