#define _GNU_SOURCE

#include "vm_internal.h"
#include "bignum.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* String#tr's from/to specs: c1-c2 ranges and, for the from-spec only, a
 * leading ^ that negates the set. A backslash escapes the very next byte
 * (so \\, \^, and \- can appear as literal data instead of triggering
 * their special meaning) -- the same convention the caller already uses
 * to build both specs from ordinary Diamond string literals. `negate` is
 * only ever set true when interpret_negation is true and the spec is at
 * least two bytes starting with an unescaped '^'; the to-spec call passes
 * interpret_negation=false so a leading '^' there is just a literal byte,
 * matching Ruby's own tr. */
static DiamondVmStatus tr_expand_spec(const DiamondString *spec,
        bool interpret_negation,bool *negate,ByteBuffer *out) {
    size_t start=0;
    *negate=false;
    if(interpret_negation&&spec->length>1&&spec->chars[0]=='^') {
        *negate=true;start=1;
    }
    const size_t remaining=spec->length-start;
    char *literal=remaining>0?malloc(remaining):nullptr;
    bool *escaped=remaining>0?malloc(remaining):nullptr;
    if(remaining>0&&(literal==nullptr||escaped==nullptr)) {
        free(literal);free(escaped);return DIAMOND_VM_OUT_OF_MEMORY;
    }
    size_t count=0;
    for(size_t index=start;index<spec->length;index++) {
        char ch=spec->chars[index];
        bool is_escaped=false;
        if(ch=='\\'&&index+1<spec->length) {
            index++;ch=spec->chars[index];is_escaped=true;
        }
        literal[count]=ch;escaped[count]=is_escaped;count++;
    }
    bool ok=true;
    size_t index=0;
    while(index<count&&ok) {
        if(!escaped[index]&&index+2<count&&
           !escaped[index+1]&&literal[index+1]=='-') {
            const unsigned char low=(unsigned char)literal[index];
            const unsigned char high=(unsigned char)literal[index+2];
            if(low<=high) {
                for(unsigned int code=low;code<=high&&ok;code++) {
                    const char byte=(char)(unsigned char)code;
                    ok=byte_buffer_append(out,&byte,1);
                }
                index+=3;continue;
            }
        }
        ok=byte_buffer_append(out,&literal[index],1);
        index++;
    }
    free(literal);free(escaped);
    if(!ok) {
        free(out->data);out->data=nullptr;out->length=0;out->capacity=0;
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    return DIAMOND_VM_OK;
}

/* String#format's real body -- kept out of run_chunk's own INVOKE case
 * for the same stack-frame-budget reason array_join_helper/
 * time_dispatch_helper already are (see array_join_helper's own
 * comment): the flag/width/precision parsing state below is real stack
 * weight this switch doesn't need baked into every call's frame.
 *
 * A %-directive's flags/width/precision are parsed and bounds-checked
 * here, then used to build a small, internally-constructed conversion
 * string (e.g. "%-05lld") handed to a real snprintf alongside exactly
 * one correctly-typed C argument -- never the caller's own format
 * string forwarded into printf-family varargs directly (that would be
 * a real format-string vulnerability, since a Diamond value's runtime
 * type has no relationship to whatever C conversion a matching printf
 * call site expects; every conversion here is chosen by this function
 * from a fixed, closed set, not from caller-controlled text).
 *
 * `args_value` is either the single value to format, or an Array of
 * them (`"%d-%s".format([1, "x"])`) -- matching Ruby's String#%,
 * without needing variadic/splat call support Diamond doesn't have. */
static DiamondVmStatus string_format_helper(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,const DiamondString *format,DiamondValue args_value,
        DiamondValue *out) {
    const DiamondValue *args=&args_value;
    size_t arg_count=1;
    if(args_value.kind==DIAMOND_VALUE_OBJECT&&
       args_value.as.object->kind==DIAMOND_OBJECT_ARRAY) {
        const DiamondArray *array=(const DiamondArray *)args_value.as.object;
        args=array->values;arg_count=array->count;
    }
    StringBuilder builder={};
    size_t arg_index=0;
    DiamondVmStatus status=DIAMOND_VM_OK;
    for(size_t index=0;index<format->length;index++) {
        const char ch=format->chars[index];
        if(ch!='%') {
            if(!builder_append(&builder,&ch,1)){status=DIAMOND_VM_OUT_OF_MEMORY;break;}
            continue;
        }
        index++;
        if(index>=format->length) {
            snprintf(vm->error,sizeof vm->error,
                "String#format: trailing '%%' with no directive");
            status=DIAMOND_VM_TYPE_ERROR;break;
        }
        if(format->chars[index]=='%') {
            if(!builder_append(&builder,"%",1)){status=DIAMOND_VM_OUT_OF_MEMORY;break;}
            continue;
        }
        bool left_justify=false,zero_pad=false;
        while(index<format->length&&
              (format->chars[index]=='-'||format->chars[index]=='0')) {
            if(format->chars[index]=='-')left_justify=true;else zero_pad=true;
            index++;
        }
        int width=0;
        while(index<format->length&&isdigit((unsigned char)format->chars[index])) {
            width=width*10+(format->chars[index]-'0');index++;
        }
        int precision=-1;
        if(index<format->length&&format->chars[index]=='.') {
            index++;precision=0;
            while(index<format->length&&isdigit((unsigned char)format->chars[index])) {
                precision=precision*10+(format->chars[index]-'0');index++;
            }
        }
        if(index>=format->length) {
            snprintf(vm->error,sizeof vm->error,
                "String#format: incomplete directive at end of format string");
            status=DIAMOND_VM_TYPE_ERROR;break;
        }
        const char conversion=format->chars[index];
        if(arg_index>=arg_count) {
            snprintf(vm->error,sizeof vm->error,
                "String#format: too few arguments for format string");
            status=DIAMOND_VM_ARITY_ERROR;break;
        }
        const DiamondValue arg=args[arg_index++];
        char piece[512];
        int piece_length=-1;
        if(conversion=='d'||conversion=='i') {
            if(arg.kind!=DIAMOND_VALUE_INT&&arg.kind!=DIAMOND_VALUE_FLOAT) {
                snprintf(vm->error,sizeof vm->error,
                    "String#format: %%%c needs an Int or Float argument",conversion);
                status=DIAMOND_VM_TYPE_ERROR;break;
            }
            const int64_t value=arg.kind==DIAMOND_VALUE_FLOAT?
                (int64_t)arg.as.real:arg.as.integer;
            char spec[16];
            snprintf(spec,sizeof spec,"%%%s%s%dlld",
                left_justify?"-":"",zero_pad?"0":"",width);
            piece_length=snprintf(piece,sizeof piece,spec,(long long)value);
        } else if(conversion=='f') {
            if(arg.kind!=DIAMOND_VALUE_INT&&arg.kind!=DIAMOND_VALUE_FLOAT) {
                snprintf(vm->error,sizeof vm->error,
                    "String#format: %%f needs an Int or Float argument");
                status=DIAMOND_VM_TYPE_ERROR;break;
            }
            const double value=arg.kind==DIAMOND_VALUE_FLOAT?
                arg.as.real:(double)arg.as.integer;
            char spec[24];
            snprintf(spec,sizeof spec,"%%%s%s%d.%df",
                left_justify?"-":"",zero_pad?"0":"",width,precision<0?6:precision);
            piece_length=snprintf(piece,sizeof piece,spec,value);
        } else if(conversion=='x'||conversion=='X'||conversion=='o'||conversion=='b') {
            if(arg.kind!=DIAMOND_VALUE_INT) {
                snprintf(vm->error,sizeof vm->error,
                    "String#format: %%%c needs an Int argument",conversion);
                status=DIAMOND_VM_TYPE_ERROR;break;
            }
            if(conversion=='b') {
                char digits[65];size_t digit_count=0;
                uint64_t bits=(uint64_t)arg.as.integer;
                do {digits[digit_count++]=(char)('0'+(bits&1));bits>>=1;}
                while(bits!=0&&digit_count<sizeof digits);
                char reversed[65];
                for(size_t i=0;i<digit_count;i++)reversed[i]=digits[digit_count-1-i];
                reversed[digit_count]='\0';
                char spec[16];
                snprintf(spec,sizeof spec,"%%%s%s%ds",
                    left_justify?"-":"",zero_pad?"0":"",width);
                piece_length=snprintf(piece,sizeof piece,spec,reversed);
            } else {
                char spec[16];
                snprintf(spec,sizeof spec,"%%%s%s%d%c",
                    left_justify?"-":"",zero_pad?"0":"",width,conversion);
                piece_length=snprintf(piece,sizeof piece,spec,
                    (unsigned long long)arg.as.integer);
            }
        } else if(conversion=='s') {
            DiamondValue stringified=DIAMOND_NIL;
            status=stringify_value(vm,chunk,depth,arg,&stringified);
            if(status!=DIAMOND_VM_OK)break;
            const DiamondString *piece_string=
                (const DiamondString *)stringified.as.object;
            const size_t pad=width>0&&(size_t)width>piece_string->length?
                (size_t)width-piece_string->length:0;
            bool ok=true;
            if(pad>0&&!left_justify)
                for(size_t i=0;i<pad&&ok;i++)ok=builder_append(&builder," ",1);
            if(ok)ok=builder_append(&builder,piece_string->chars,piece_string->length);
            if(pad>0&&left_justify)
                for(size_t i=0;i<pad&&ok;i++)ok=builder_append(&builder," ",1);
            if(!ok){status=DIAMOND_VM_OUT_OF_MEMORY;break;}
            continue;
        } else {
            snprintf(vm->error,sizeof vm->error,
                "String#format: unknown directive '%%%c'",conversion);
            status=DIAMOND_VM_TYPE_ERROR;break;
        }
        if(piece_length<0||(size_t)piece_length>=sizeof piece) {
            status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
        if(!builder_append(&builder,piece,(size_t)piece_length)) {
            status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
    }
    if(status!=DIAMOND_VM_OK) {free(builder.chars);return status;}
    DiamondString *formatted=allocate_string(vm,
        builder.chars?builder.chars:"",builder.length);
    free(builder.chars);
    if(formatted==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(formatted);
    return DIAMOND_VM_OK;
}

/* `format % values`, Ruby's spelling of `format.format(values)`: the `%`
 * operator with a String on the left. Outlined from run_chunk's MODULO
 * case to keep its locals out of run_chunk's frame. */
__attribute__((noinline))
DiamondVmStatus string_percent_helper(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,
        DiamondValue *registers,uint16_t left,uint16_t right,uint16_t dest) {
    DiamondValue formatted=DIAMOND_NIL;
    const DiamondVmStatus status=string_format_helper(vm,chunk,depth,
        (const DiamondString *)registers[left].as.object,registers[right],&formatted);
    if(status!=DIAMOND_VM_OK)return status;
    registers[dest]=formatted;
    return DIAMOND_VM_OK;
}

/* sub/gsub/scan's pattern: a Regexp, or a String matched literally. A
 * String becomes a Regexp parked in registers[dest], which keeps it alive
 * until the method writes its result there. */
static DiamondVmStatus string_pattern_argument(DiamondVm *vm,DiamondValue *registers,uint16_t base,
        uint16_t dest,const char *method,const DiamondRegexp **pattern) {
    const DiamondValue argument=registers[base];
    if(argument.kind==DIAMOND_VALUE_OBJECT&&argument.as.object->kind==DIAMOND_OBJECT_REGEXP) {
        *pattern=(const DiamondRegexp *)argument.as.object;
        return DIAMOND_VM_OK;
    }
    if(argument.kind!=DIAMOND_VALUE_OBJECT||argument.as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "String#%s pattern must be a Regexp or a String",method);
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondValue literal=DIAMOND_NIL;
    const DiamondVmStatus status=regexp_literal_helper(vm,
        (const DiamondString *)argument.as.object,&literal);
    if(status!=DIAMOND_VM_OK)return status;
    registers[dest]=literal;
    *pattern=(const DiamondRegexp *)literal.as.object;
    return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_substitute_method(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source,const bool gsub_method) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    const DiamondRegexp *pattern=nullptr;
    const DiamondVmStatus pattern_status=string_pattern_argument(vm,registers,base,dest,
        gsub_method?"gsub":"sub",&pattern);
    if(pattern_status!=DIAMOND_VM_OK)return pattern_status;
    if(registers[(size_t)base+1].kind==DIAMOND_VALUE_OBJECT&&
       registers[(size_t)base+1].as.object->kind==DIAMOND_OBJECT_CLOSURE) {
        DiamondValue replace_result=DIAMOND_NIL;
        const DiamondVmStatus replace_status=
            regexp_replace_block_helper(vm,chunk,depth,
            pattern,source,
            (const DiamondClosure *)registers[(size_t)base+1].as.object,
            gsub_method,&replace_result);
        if(replace_status!=DIAMOND_VM_OK)return replace_status;
        registers[dest]=replace_result;return DIAMOND_VM_OK;
    }
    if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "String#%s replacement must be a String or a block",
            gsub_method?"gsub":"sub");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondValue replace_result=DIAMOND_NIL;
    const DiamondVmStatus replace_status=regexp_replace_helper(vm,
        pattern,source,
        (const DiamondString *)registers[(size_t)base+1].as.object,
        gsub_method,&replace_result);
    if(replace_status!=DIAMOND_VM_OK)return replace_status;
    registers[dest]=replace_result;return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_scan_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    const DiamondRegexp *pattern=nullptr;
    const DiamondVmStatus pattern_status=string_pattern_argument(vm,registers,base,dest,
        "scan",&pattern);
    if(pattern_status!=DIAMOND_VM_OK)return pattern_status;
    const DiamondVmStatus scan_status=regexp_scan_helper(vm,pattern,source,
        registers,dest);
    if(scan_status!=DIAMOND_VM_OK)return scan_status;
    return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_repeat_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,
            "String#repeat argument must be an Int");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t count=registers[base].as.integer;
    if(count<0) {
        snprintf(vm->error,sizeof vm->error,
            "String#repeat argument must be a non-negative Int");
        return DIAMOND_VM_INTEGER_OVERFLOW;
    }
    size_t total_length=0;
    if(ckd_mul(&total_length,source->length,(size_t)count)) {
        snprintf(vm->error,sizeof vm->error,
            "String#repeat result is too large");
        return DIAMOND_VM_INTEGER_OVERFLOW;
    }
    char *buffer=malloc(total_length+1);
    if(buffer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    for(size_t copy=0;copy<(size_t)count;copy++)
        memcpy(buffer+copy*source->length,source->chars,
               source->length);
    DiamondString *repeated=
        allocate_string(vm,buffer,total_length);
    free(buffer);
    if(repeated==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(repeated);return DIAMOND_VM_OK;
}

/* getbyte(i): the byte at i (0-255), counting from
 * the end when negative, or nil past either end --
 * byte access without copying the String, as in
 * Ruby. */
__attribute__((noinline))
static DiamondVmStatus string_getbyte_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,
            "String#getbyte index must be an Int");
        return DIAMOND_VM_TYPE_ERROR;
    }
    int64_t at=registers[base].as.integer;
    if(at<0)at+=(int64_t)source->length;
    registers[dest]=at<0||(uint64_t)at>=source->length?DIAMOND_NIL:
        DIAMOND_INT((unsigned char)source->chars[at]);
    return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_split_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind==DIAMOND_VALUE_OBJECT&&
       registers[base].as.object->kind==DIAMOND_OBJECT_REGEXP) {
        const DiamondVmStatus split_status=regexp_split_helper(vm,
            (const DiamondRegexp *)registers[base].as.object,
            source,registers,dest);
        if(split_status!=DIAMOND_VM_OK)return split_status;
        return DIAMOND_VM_OK;
    }
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "String#split argument must be a String or Regexp");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *separator=
        (const DiamondString *)registers[base].as.object;
    DiamondArray *pieces=allocate_array(vm,nullptr,0);
    if(pieces==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    /* Root the result array in registers[dest] before any
     * further allocation (each piece below) can trigger a
     * GC collection - registers are the VM's root set. */
    registers[dest]=DIAMOND_OBJECT(pieces);
    if(separator->length==0) {
        for(size_t index=0;index<source->length;index++) {
            DiamondString *piece=
                allocate_string(vm,source->chars+index,1);
            if(piece==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
            if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
                return DIAMOND_VM_OUT_OF_MEMORY;
        }
    } else {
        size_t start=0,cursor=0;
        while(cursor+separator->length<=source->length) {
            if(memcmp(source->chars+cursor,separator->chars,
                      separator->length)==0) {
                DiamondString *piece=allocate_string(vm,
                    source->chars+start,cursor-start);
                if(piece==nullptr)
                    return DIAMOND_VM_OUT_OF_MEMORY;
                if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
                    return DIAMOND_VM_OUT_OF_MEMORY;
                cursor+=separator->length;start=cursor;
            } else {
                cursor++;
            }
        }
        DiamondString *piece=allocate_string(vm,
            source->chars+start,source->length-start);
        if(piece==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
            return DIAMOND_VM_OUT_OF_MEMORY;
    }
    return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_strip_method(DiamondVm *vm,DiamondValue *registers,uint8_t argc,uint16_t dest,const DiamondString *source,const bool lstrip_method,const bool rstrip_method) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    size_t start=0;
    if(!rstrip_method)
        while(start<source->length&&
              isspace((unsigned char)source->chars[start]))start++;
    size_t end=source->length;
    if(!lstrip_method)
        while(end>start&&
              isspace((unsigned char)source->chars[end-1]))end--;
    DiamondString *stripped=
        allocate_string(vm,source->chars+start,end-start);
    if(stripped==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(stripped);return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_reverse_method(DiamondVm *vm,DiamondValue *registers,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    DiamondString *reversed=
        allocate_string(vm,source->chars,source->length);
    if(reversed==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    for(size_t index=0;index<reversed->length/2;index++) {
        const char swap=reversed->chars[index];
        reversed->chars[index]=
            reversed->chars[reversed->length-1-index];
        reversed->chars[reversed->length-1-index]=swap;
    }
    registers[dest]=DIAMOND_OBJECT(reversed);return DIAMOND_VM_OK;
}

/* to_i(base): digits in base 2..36 (either case, `_`
 * separators allowed) after an optional sign, up to
 * the first character that isn't one -- 0 if there
 * are none, as with to_i(). */
__attribute__((noinline))
static DiamondVmStatus string_to_i_radix_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint16_t dest,const DiamondString *source) {
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[base].as.integer<2||registers[base].as.integer>36) {
        snprintf(vm->error,sizeof vm->error,
            "String#to_i base must be an Int from 2 to 36");
        return DIAMOND_VM_ARITY_ERROR;
    }
    const int64_t radix=registers[base].as.integer;
    size_t position=0;bool negative=false;
    while(position<source->length&&
          (source->chars[position]==' '||source->chars[position]=='\t'))
        position++;
    if(position<source->length&&
       (source->chars[position]=='-'||source->chars[position]=='+')) {
        negative=source->chars[position]=='-';position++;
    }
    int64_t value=0;bool overflowed=false;
    const size_t digits_start=position;
    for(;position<source->length;position++) {
        const char c=source->chars[position];
        if(c=='_')continue;
        const int digit=c>='0'&&c<='9'?c-'0':
            c>='a'&&c<='z'?c-'a'+10:c>='A'&&c<='Z'?c-'A'+10:99;
        if(digit>=radix)break;
        if(!overflowed&&
           (ckd_mul(&value,value,radix)||ckd_add(&value,value,(int64_t)digit)))
            overflowed=true;
    }
    if(overflowed) {
        /* Past 64 bits: parse the digits again into a bignum. */
        char *clean=malloc(position-digits_start+1);
        if(clean==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        size_t clean_length=0;
        for(size_t index=digits_start;index<position;index++)
            if(source->chars[index]!='_')clean[clean_length++]=source->chars[index];
        const DiamondValue big=diamond_bignum_from_radix_digits(vm,clean,
            clean_length,(int)radix,negative);
        free(clean);
        if(big.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=big;return DIAMOND_VM_OK;
    }
    registers[dest]=DIAMOND_INT(negative?-value:value);return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_to_i_method(DiamondVm *vm,DiamondValue *registers,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    size_t position=0;bool negative=false;
    if(position<source->length&&
       (source->chars[position]=='-'||source->chars[position]=='+')) {
        negative=source->chars[position]=='-';position++;
    }
    const size_t digit_start=position;
    int64_t value=0;bool saw_digit=false;bool overflowed=false;
    while(position<source->length&&
          source->chars[position]>='0'&&source->chars[position]<='9') {
        saw_digit=true;
        if(!overflowed) {
            int64_t widened=0;
            if(ckd_mul(&widened,value,(int64_t)10)||
               ckd_add(&value,widened,
                       (int64_t)(source->chars[position]-'0')))
                overflowed=true;
        }
        position++;
    }
    if(!saw_digit) {
        registers[dest]=DIAMOND_INT(0);
        return DIAMOND_VM_OK;
    }
    if(overflowed) {
        /* Wider than int64_t: promote instead of
         * raising, matching every other overflow
         * site now that Int auto-promotes. */
        const DiamondValue bignum_result=
            diamond_bignum_from_decimal_digits(vm,
                source->chars+digit_start,
                position-digit_start,negative);
        if(bignum_result.kind==DIAMOND_VALUE_NIL)
            return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=bignum_result;
        return DIAMOND_VM_OK;
    }
    registers[dest]=DIAMOND_INT(negative?-value:value);
    return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_to_f_method(DiamondValue *registers,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    /* No leading-whitespace skip, matching to_i's
     * convention -- strtod's own grammar would
     * otherwise skip it. Overflow is allowed to
     * become Infinity (unlike to_i, which must
     * reject out-of-range values): Float already
     * has a well-defined way to represent "too
     * large", Int does not. */
    double value=0.0;
    if(source->length>0) {
        const char first=source->chars[0];
        if((first>='0'&&first<='9')||
           first=='+'||first=='-'||first=='.') {
            char *end=nullptr;
            const double parsed=strtod(source->chars,&end);
            if(end!=source->chars)value=parsed;
        }
    }
    registers[dest]=DIAMOND_FLOAT(value);
    return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_index_of_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "String#index_of argument must be a String");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *needle=
        (const DiamondString *)registers[base].as.object;
    registers[dest]=DIAMOND_NIL;
    if(needle->length==0) {
        registers[dest]=DIAMOND_INT(0);
    } else if(needle->length<=source->length) {
        for(size_t start=0;
            start+needle->length<=source->length;start++) {
            if(memcmp(source->chars+start,needle->chars,
                      needle->length)==0) {
                registers[dest]=DIAMOND_INT((int64_t)start);return DIAMOND_VM_OK;
            }
        }
    }
    return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_slice_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,
            "String#slice arguments must be Int");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t start=registers[base].as.integer;
    const int64_t requested_length=
        registers[(size_t)base+1].as.integer;
    if(start<0||(uint64_t)start>source->length||
       requested_length<0) {
        snprintf(vm->error,sizeof vm->error,
            "index %" PRId64 " out of bounds for String of length %zu",
            start,source->length);
        return DIAMOND_VM_INDEX_ERROR;
    }
    const size_t available=source->length-(size_t)start;
    const size_t take=(size_t)requested_length<available?
        (size_t)requested_length:available;
    DiamondString *sliced=
        allocate_string(vm,source->chars+(size_t)start,take);
    if(sliced==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(sliced);return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_affix_test_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source,const bool start_with_method) {
    /* Several candidates, as in Ruby: true if any of them matches. */
    if(argc==0)return DIAMOND_VM_ARITY_ERROR;
    bool matches=false;
    for(uint8_t index=0;index<argc;index++) {
        const DiamondValue argument=registers[(size_t)base+index];
        if(argument.kind!=DIAMOND_VALUE_OBJECT||
           argument.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "String#%s argument must be a String",
                start_with_method?"start_with?":"end_with?");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *needle=(const DiamondString *)argument.as.object;
        matches=matches||(needle->length<=source->length&&
            memcmp(start_with_method?source->chars:
                   source->chars+source->length-needle->length,
                   needle->chars,needle->length)==0);
    }
    registers[dest]=DIAMOND_BOOL(matches);return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_includes_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "String#include? argument must be a String");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *needle=
        (const DiamondString *)registers[base].as.object;
    bool found=needle->length==0;
    for(size_t start=0;
        !found&&needle->length>0&&
        start+needle->length<=source->length;start++) {
        if(memcmp(source->chars+start,needle->chars,
                  needle->length)==0)found=true;
    }
    registers[dest]=DIAMOND_BOOL(found);return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_capitalize_method(DiamondVm *vm,DiamondValue *registers,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    DiamondString *capitalized=
        allocate_string(vm,source->chars,source->length);
    if(capitalized==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    for(size_t index=0;index<capitalized->length;index++)
        capitalized->chars[index]=
            (char)tolower((unsigned char)capitalized->chars[index]);
    if(capitalized->length>0)
        capitalized->chars[0]=
            (char)toupper((unsigned char)capitalized->chars[0]);
    registers[dest]=DIAMOND_OBJECT(capitalized);return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_chars_method(DiamondVm *vm,DiamondValue *registers,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    DiamondArray *pieces=allocate_array(vm,nullptr,0);
    if(pieces==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(pieces);
    for(size_t index=0;index<source->length;index++) {
        DiamondString *piece=
            allocate_string(vm,source->chars+index,1);
        if(piece==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
            return DIAMOND_VM_OUT_OF_MEMORY;
    }
    return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_bytes_method(DiamondVm *vm,DiamondValue *registers,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    DiamondArray *values=allocate_array(vm,nullptr,0);
    if(values==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(values);
    for(size_t index=0;index<source->length;index++) {
        const DiamondValue byte_value=
            DIAMOND_INT((unsigned char)source->chars[index]);
        if(!array_push(vm,values,byte_value))
            return DIAMOND_VM_OUT_OF_MEMORY;
    }
    return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_chomp_method(DiamondVm *vm,DiamondValue *registers,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    size_t end=source->length;
    if(end>=2&&source->chars[end-2]=='\r'&&
       source->chars[end-1]=='\n')
        end-=2;
    else if(end>=1&&(source->chars[end-1]=='\n'||
                      source->chars[end-1]=='\r'))
        end-=1;
    DiamondString *chomped=
        allocate_string(vm,source->chars,end);
    if(chomped==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(chomped);return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_justify_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source,const bool ljust_method) {
    /* The padding defaults to one space, as in Ruby. */
    if(argc!=2&&argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,
            "String#%s width argument must be an Int",
            ljust_method?"ljust":"rjust");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(argc==2&&(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=
           DIAMOND_OBJECT_STRING)) {
        snprintf(vm->error,sizeof vm->error,
            "String#%s padding argument must be a String",
            ljust_method?"ljust":"rjust");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t width=registers[base].as.integer;
    if(width<0) {
        snprintf(vm->error,sizeof vm->error,
            "String#%s width argument must be a non-negative Int",
            ljust_method?"ljust":"rjust");
        return DIAMOND_VM_ARITY_ERROR;
    }
    DiamondString *default_pad=nullptr;
    if(argc==1) {
        default_pad=allocate_string(vm," ",1);
        if(default_pad==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    }
    const DiamondString *pad=argc==1?default_pad:(const DiamondString *)
        registers[(size_t)base+1].as.object;
    if((uint64_t)width<=source->length) {
        DiamondString *unchanged=
            allocate_string(vm,source->chars,source->length);
        if(unchanged==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(unchanged);return DIAMOND_VM_OK;
    }
    if(pad->length==0) {
        snprintf(vm->error,sizeof vm->error,
            "String#%s padding string must not be empty",
            ljust_method?"ljust":"rjust");
        return DIAMOND_VM_ARITY_ERROR;
    }
    const size_t pad_needed=(size_t)width-source->length;
    char *buffer=malloc((size_t)width+1);
    if(buffer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(ljust_method) {
        memcpy(buffer,source->chars,source->length);
        for(size_t index=0;index<pad_needed;index++)
            buffer[source->length+index]=
                pad->chars[index%pad->length];
    } else {
        for(size_t index=0;index<pad_needed;index++)
            buffer[index]=pad->chars[index%pad->length];
        memcpy(buffer+pad_needed,source->chars,source->length);
    }
    DiamondString *justified=
        allocate_string(vm,buffer,(size_t)width);
    free(buffer);
    if(justified==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(justified);return DIAMOND_VM_OK;
}

__attribute__((noinline))
static DiamondVmStatus string_tr_method(DiamondVm *vm,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,const DiamondString *source) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=
           DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "String#tr arguments must be Strings");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *from_spec=(const DiamondString *)
        registers[base].as.object;
    const DiamondString *to_spec=(const DiamondString *)
        registers[(size_t)base+1].as.object;
    if(from_spec->length==0) {
        snprintf(vm->error,sizeof vm->error,
            "String#tr from-string must not be empty");
        return DIAMOND_VM_ARITY_ERROR;
    }
    bool from_negate=false;
    ByteBuffer from_buffer={0};
    DiamondVmStatus tr_status=
        tr_expand_spec(from_spec,true,&from_negate,&from_buffer);
    if(tr_status!=DIAMOND_VM_OK)return tr_status;
    bool to_negate_ignored=false;
    ByteBuffer to_buffer={0};
    tr_status=
        tr_expand_spec(to_spec,false,&to_negate_ignored,&to_buffer);
    if(tr_status!=DIAMOND_VM_OK) {
        free(from_buffer.data);
        return tr_status;
    }
    bool member[256]={false};
    for(size_t index=0;index<from_buffer.length;index++)
        member[(unsigned char)from_buffer.data[index]]=true;
    int map[256];
    for(int code=0;code<256;code++)map[code]=-1;
    if(!from_negate) {
        for(size_t index=0;index<from_buffer.length;index++) {
            const unsigned char key=
                (unsigned char)from_buffer.data[index];
            const size_t to_index=index<to_buffer.length?
                index:to_buffer.length-1;
            map[key]=to_buffer.length==0?-2:
                (int)(unsigned char)to_buffer.data[to_index];
        }
    }
    const int negate_replacement=to_buffer.length==0?-2:
        (int)(unsigned char)to_buffer.data[to_buffer.length-1];
    ByteBuffer result_buffer={0};
    bool ok=true;
    for(size_t index=0;index<source->length&&ok;index++) {
        const unsigned char byte=
            (unsigned char)source->chars[index];
        const int replacement=from_negate?
            (member[byte]?-1:negate_replacement):map[byte];
        if(replacement==-1)
            ok=byte_buffer_append(&result_buffer,
                &source->chars[index],1);
        else if(replacement!=-2) {
            const char byte_out=(char)(unsigned char)replacement;
            ok=byte_buffer_append(&result_buffer,&byte_out,1);
        }
    }
    free(from_buffer.data);free(to_buffer.data);
    if(!ok) {
        free(result_buffer.data);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondString *translated=allocate_string(vm,
        result_buffer.data!=nullptr?result_buffer.data:"",
        result_buffer.length);
    free(result_buffer.data);
    if(translated==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(translated);return DIAMOND_VM_OK;
}

/* String receivers of collection_invoke_helper: the native String methods, then the
 * prelude's string_ extensions. Kept out of that function so its frame, which every
 * Array and Hash method call pays for, does not carry this block's locals. */
__attribute__((noinline))
DiamondVmStatus string_invoke_helper(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,const DiamondStringConstant *method_name) {
    const bool index_of_method=method_name->length==8&&
        memcmp(method_name->chars,"index_of",8)==0;
    const bool slice_method=method_name->length==5&&
        memcmp(method_name->chars,"slice",5)==0;
    const bool to_i_method=method_name->length==4&&
        memcmp(method_name->chars,"to_i",4)==0;
    const bool to_f_method=method_name->length==4&&
        memcmp(method_name->chars,"to_f",4)==0;
    const bool downcase_method=method_name->length==8&&
        memcmp(method_name->chars,"downcase",8)==0;
    const bool upcase_method=method_name->length==6&&
        memcmp(method_name->chars,"upcase",6)==0;
    const bool reverse_method=method_name->length==7&&
        memcmp(method_name->chars,"reverse",7)==0;
    const bool strip_method=method_name->length==5&&
        memcmp(method_name->chars,"strip",5)==0;
    const bool split_method=method_name->length==5&&
        memcmp(method_name->chars,"split",5)==0;
    const bool ord_method=method_name->length==3&&
        memcmp(method_name->chars,"ord",3)==0;
    const bool repeat_method=method_name->length==6&&
        memcmp(method_name->chars,"repeat",6)==0;
    const bool gsub_method=method_name->length==4&&
        memcmp(method_name->chars,"gsub",4)==0;
    const bool sub_method=method_name->length==3&&
        memcmp(method_name->chars,"sub",3)==0;
    const bool scan_method=method_name->length==4&&
        memcmp(method_name->chars,"scan",4)==0;
    const bool start_with_method=method_name->length==11&&
        memcmp(method_name->chars,"start_with?",11)==0;
    const bool end_with_method=method_name->length==9&&
        memcmp(method_name->chars,"end_with?",9)==0;
    const bool includes_method=method_name->length==8&&
        memcmp(method_name->chars,"include?",8)==0;
    const bool capitalize_method=method_name->length==10&&
        memcmp(method_name->chars,"capitalize",10)==0;
    const bool chars_method=method_name->length==5&&
        memcmp(method_name->chars,"chars",5)==0;
    const bool bytes_method=method_name->length==5&&
        memcmp(method_name->chars,"bytes",5)==0;
    const bool chomp_method=method_name->length==5&&
        memcmp(method_name->chars,"chomp",5)==0;
    const bool ljust_method=method_name->length==5&&
        memcmp(method_name->chars,"ljust",5)==0;
    const bool rjust_method=method_name->length==5&&
        memcmp(method_name->chars,"rjust",5)==0;
    const bool tr_method=method_name->length==2&&
        memcmp(method_name->chars,"tr",2)==0;
    const bool format_method=method_name->length==6&&
        memcmp(method_name->chars,"format",6)==0;
    const bool parse_json_method=method_name->length==10&&
        memcmp(method_name->chars,"parse_json",10)==0;
    const DiamondString *source=
        (const DiamondString *)registers[recv].as.object;
    /* Same name and meaning as Array#empty?/Hash#empty?. */
    if(method_name->length==6&&
       memcmp(method_name->chars,"empty?",6)==0) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_BOOL(source->length==0);return DIAMOND_VM_OK;
    }
    if(parse_json_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        const size_t protect_mark=vm->gc_protected_count;
        DiamondValue parsed=DIAMOND_NIL;
        const DiamondVmStatus parse_status=json_parse_document(
            vm,source->chars,source->length,&parsed);
        gc_unprotect(vm,protect_mark);
        if(parse_status!=DIAMOND_VM_OK)return parse_status;
        registers[dest]=parsed;return DIAMOND_VM_OK;
    }
    if(gsub_method||sub_method)
        return string_substitute_method(vm,chunk,depth,registers,base,argc,dest,source,gsub_method);
    if(scan_method)
        return string_scan_method(vm,registers,base,argc,dest,source);
    if(repeat_method)
        return string_repeat_method(vm,registers,base,argc,dest,source);
    if(method_name->length==7&&memcmp(method_name->chars,"getbyte",7)==0)
        return string_getbyte_method(vm,registers,base,argc,dest,source);
    if(ord_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(source->length==0) {
            snprintf(vm->error,sizeof vm->error,
                "cannot take ord of an empty String");
            return DIAMOND_VM_INDEX_ERROR;
        }
        registers[dest]=
            DIAMOND_INT((unsigned char)source->chars[0]);
        return DIAMOND_VM_OK;
    }
    /* split() on whitespace, and split(separator, limit):
     * in the prelude (diamond_string_split_extended). */
    if(split_method&&argc!=1) {
        const DiamondFunction *extended=find_top_level_function(
            chunk,"diamond_string_split_extended",29);
        if(extended==nullptr)return DIAMOND_VM_ARITY_ERROR;
        DiamondValue split_result=DIAMOND_NIL;
        const DiamondVmStatus split_status=forward_to_top_level_helper(vm,
            chunk,extended,registers,recv,base,argc,depth,&split_result);
        if(split_status!=DIAMOND_VM_OK)return split_status;
        registers[dest]=split_result;return DIAMOND_VM_OK;
    }
    if(split_method)
        return string_split_method(vm,registers,base,argc,dest,source);
    /* lstrip/rstrip trim one side only, the same ASCII
     * whitespace strip removes. */
    const bool lstrip_method=method_name->length==6&&
        memcmp(method_name->chars,"lstrip",6)==0;
    const bool rstrip_method=method_name->length==6&&
        memcmp(method_name->chars,"rstrip",6)==0;
    if(strip_method||lstrip_method||rstrip_method)
        return string_strip_method(vm,registers,argc,dest,source,lstrip_method,rstrip_method);
    if(reverse_method)
        return string_reverse_method(vm,registers,argc,dest,source);
    if(downcase_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        DiamondString *lowered=
            allocate_string(vm,source->chars,source->length);
        if(lowered==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        for(size_t index=0;index<lowered->length;index++)
            lowered->chars[index]=
                (char)tolower((unsigned char)lowered->chars[index]);
        registers[dest]=DIAMOND_OBJECT(lowered);return DIAMOND_VM_OK;
    }
    if(upcase_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        DiamondString *raised=
            allocate_string(vm,source->chars,source->length);
        if(raised==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        for(size_t index=0;index<raised->length;index++)
            raised->chars[index]=
                (char)toupper((unsigned char)raised->chars[index]);
        registers[dest]=DIAMOND_OBJECT(raised);return DIAMOND_VM_OK;
    }
    if(to_i_method&&argc==1)
        return string_to_i_radix_method(vm,registers,base,dest,source);
    if(to_i_method)
        return string_to_i_method(vm,registers,argc,dest,source);
    if(to_f_method)
        return string_to_f_method(registers,argc,dest,source);
    if(index_of_method)
        return string_index_of_method(vm,registers,base,argc,dest,source);
    if(slice_method)
        return string_slice_method(vm,registers,base,argc,dest,source);
    if(start_with_method||end_with_method)
        return string_affix_test_method(vm,registers,base,argc,dest,source,start_with_method);
    if(includes_method)
        return string_includes_method(vm,registers,base,argc,dest,source);
    if(capitalize_method)
        return string_capitalize_method(vm,registers,argc,dest,source);
    if(chars_method)
        return string_chars_method(vm,registers,argc,dest,source);
    if(bytes_method)
        return string_bytes_method(vm,registers,argc,dest,source);
    if(chomp_method)
        return string_chomp_method(vm,registers,argc,dest,source);
    if(ljust_method||rjust_method)
        return string_justify_method(vm,registers,base,argc,dest,source,ljust_method);
    if(tr_method)
        return string_tr_method(vm,registers,base,argc,dest,source);
    if(format_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        DiamondValue formatted=DIAMOND_NIL;
        const DiamondVmStatus format_status=string_format_helper(
            vm,chunk,depth,source,registers[base],&formatted);
        if(format_status!=DIAMOND_VM_OK)return format_status;
        registers[dest]=formatted;return DIAMOND_VM_OK;
    }
    /* Not chunk->code+instruction_offset (an ordinary call site's
     * own would be stable, but the native-spread synthetic re-entry
     * just above builds its own tiny bytecode buffer fresh on the C
     * stack every time -- a different logical call site, same reused
     * stack address, which collided two unrelated method names onto
     * one cache entry and returned the wrong function). method_name
     * itself is a pointer into the calling function's own permanent,
     * never-reallocated string-constant table (fn->strings, set at
     * compile time), stable and correctly distinct per method name
     * regardless of which of the several dispatch paths reaches it. */
    const uint8_t *extension_site=(const uint8_t *)(const void *)method_name;
    const DiamondFunction *extension=nullptr;
    if(!cached_extension_lookup(vm,extension_site,
            DIAMOND_EXTENSION_KIND_STRING,&extension)) {
        static const char *const string_prefixes[]={"string_"};
        extension=find_value_extension(chunk,
            string_prefixes,1,method_name->chars,method_name->length);
        store_extension_lookup(vm,extension_site,
            DIAMOND_EXTENSION_KIND_STRING,extension);
    }
    if(extension!=nullptr) {
        DiamondValue call_result=DIAMOND_NIL;
        const DiamondVmStatus status=forward_to_top_level_helper(vm,
            chunk,extension,registers,recv,base,argc,depth,&call_result);
        if(status!=DIAMOND_VM_OK)return status;
        registers[dest]=call_result;return DIAMOND_VM_OK;
    }
    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
        (int)method_name->length,method_name->chars,"String");
    static const char *const suggestion_prefixes[]={"string_"};
    diamond_append_method_suggestion(vm,chunk,DIAMOND_TYPE_STRING,suggestion_prefixes,1,
        nullptr,method_name->chars,method_name->length);
    return DIAMOND_VM_TYPE_ERROR;
}
