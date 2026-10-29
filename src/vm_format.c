#define _GNU_SOURCE

#include "vm_internal.h"
#include "bignum.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *type_name(const DiamondChunk *chunk,uint16_t type) {
    const char *name="<invalid type>";
    if(type==DIAMOND_TYPE_INT) name="Int";
    else if(type==DIAMOND_TYPE_FLOAT) name="Float";
    else if(type==DIAMOND_TYPE_STRING) name="String";
    else if(type==DIAMOND_TYPE_SYMBOL) name="Symbol";
    else if(type==DIAMOND_TYPE_BOOL) name="Bool";
    else if(type==DIAMOND_TYPE_NIL) name="Nil";
    else if(type==DIAMOND_TYPE_ARRAY) name="Array";
    else if(type==DIAMOND_TYPE_HASH) name="Hash";
    else if(type==DIAMOND_TYPE_CALLABLE) name="Callable";
    else if(type==DIAMOND_TYPE_SIZED) name="Sized";
    else if(type==DIAMOND_TYPE_NATIVE) name="Native";
    else if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(type-DIAMOND_TYPE_VARIABLE_BASE);
        const DiamondTypeBinding *binding=chunk->type_variable_bindings;
        if(binding!=nullptr&&variable<chunk->type_variable_count&&
           binding[variable].node_count>0&&binding[variable].nodes[0].count>0) {
            const DiamondBoundTypeMember member=binding[variable].nodes[0].members[0];
            name=member.id==DIAMOND_TYPE_NATIVE?diamond_native_type_name(member.native_kind):
                type_name(chunk,member.id);
        }
        else name="TypeVariable";
    }
    else if(type>=DIAMOND_TYPE_INTERFACE_BASE&&
            (size_t)(type-DIAMOND_TYPE_INTERFACE_BASE)<chunk->interface_count)
        name=chunk->interfaces[type-DIAMOND_TYPE_INTERFACE_BASE]->name;
    else {
        const size_t index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
        if(index<chunk->class_count) name=chunk->classes[index]->name;
    }
    return name;
}

void format_type_set_index(char *buffer,size_t capacity,
                                  const DiamondChunk *chunk,uint16_t set_index) {
    if((size_t)set_index>=chunk->type_set_count) {
        snprintf(buffer,capacity,"<invalid type set>");return;
    }
    const DiamondTypeSet *set=&chunk->type_sets[set_index];
    size_t used=0;buffer[0]='\0';
    for(size_t index=0;index<set->count && used<capacity;index++) {
        const int written=snprintf(buffer+used,capacity-used,"%s%s",
            index==0?"":" | ",set->members[index].id==DIAMOND_TYPE_NATIVE?
                diamond_native_type_name(set->members[index].callable_arity):
                type_name(chunk,set->members[index].id));
        if(written<0)return;
        used+=(size_t)written;
        if(set->members[index].argument_set!=DIAMOND_NO_TYPE_SET&&used<capacity) {
            const int open=snprintf(buffer+used,capacity-used,"[");
            if(open<0)return;
            used+=(size_t)open;
            char nested[80];format_type_set_index(nested,sizeof nested,chunk,
                set->members[index].argument_set);
            char second[80]="";
            if(set->members[index].second_argument_set!=DIAMOND_NO_TYPE_SET)
                format_type_set_index(second,sizeof second,chunk,
                    set->members[index].second_argument_set);
            const int close=snprintf(buffer+used,capacity-used,"%s%s%s]",nested,
                second[0]=='\0'?"":", ",second);
            if(close<0)return;
            used+=(size_t)close;
        } else if(set->members[index].id==DIAMOND_TYPE_CALLABLE&&
                  set->members[index].callable_arity!=UINT8_MAX&&used<capacity) {
            const DiamondTypeMember member=set->members[index];
            const int arity=snprintf(buffer+used,capacity-used,
                member.callable_parameters_typed?"[[":"[%u",member.callable_arity);
            if(arity<0)return;
            used+=(size_t)arity;
            if(member.callable_parameters_typed) {
                for(size_t parameter=0;parameter<member.callable_arity&&used<capacity;
                    parameter++) {
                    char parameter_type[80];
                    format_type_set_index(parameter_type,sizeof parameter_type,chunk,
                        member.callable_parameter_sets[parameter]);
                    const int result=snprintf(buffer+used,capacity-used,"%s%s",
                        parameter==0?"":", ",parameter_type);
                    if(result<0)return;
                    used+=(size_t)result;
                }
                if(used<capacity) {
                    const int close=snprintf(buffer+used,capacity-used,"]");
                    if(close<0)return;
                    used+=(size_t)close;
                }
            }
            if(member.callable_return_set!=DIAMOND_NO_TYPE_SET&&used<capacity) {
                char returns[80];
                format_type_set_index(returns,sizeof returns,chunk,
                    member.callable_return_set);
                const int result=snprintf(buffer+used,capacity-used,", %s",returns);
                if(result<0)return;
                used+=(size_t)result;
            }
            if(used<capacity) {
                const int close=snprintf(buffer+used,capacity-used,"]");
                if(close<0)return;
                used+=(size_t)close;
            }
        }
    }
}

/* Annotation name <-> object kind for native values. The names match what
 * diamond_format_value_type prints for a value of that kind, so an error
 * reads "expected Time, got Fiber". */
static const struct {const char *name;DiamondObjectKind kind;} native_type_names[]={
    {"Fiber",DIAMOND_OBJECT_FIBER},{"File",DIAMOND_OBJECT_FILE},
    {"Listener",DIAMOND_OBJECT_LISTENER},{"Socket",DIAMOND_OBJECT_SOCKET},
    {"UDPSocket",DIAMOND_OBJECT_UDP_SOCKET},{"TLSSocket",DIAMOND_OBJECT_TLS_SOCKET},
    {"Regexp",DIAMOND_OBJECT_REGEXP},{"ProgramBuilder",DIAMOND_OBJECT_PROGRAM_BUILDER},
    {"Thread",DIAMOND_OBJECT_THREAD},{"SQLite3",DIAMOND_OBJECT_SQLITE3},
    {"Statement",DIAMOND_OBJECT_SQLITE3_STATEMENT},{"PostgreSQL",DIAMOND_OBJECT_POSTGRES},
    {"MySQL",DIAMOND_OBJECT_MYSQL},{"Time",DIAMOND_OBJECT_TIME},
    {"ProcessResult",DIAMOND_OBJECT_PROCESS_RESULT},
    {"ProcessHandle",DIAMOND_OBJECT_PROCESS_HANDLE},
    {"ProcessStream",DIAMOND_OBJECT_PROCESS_STREAM},{"Tensor",DIAMOND_OBJECT_TENSOR},
    {"Channel",DIAMOND_OBJECT_CHANNEL},{"Supervisor",DIAMOND_OBJECT_SUPERVISOR},
};

bool diamond_native_type_kind(const char *name, uint8_t *kind) {
    for(size_t index=0;index<sizeof native_type_names/sizeof native_type_names[0];index++)
        if(strcmp(name,native_type_names[index].name)==0) {
            *kind=(uint8_t)native_type_names[index].kind;
            return true;
        }
    return false;
}

const char *diamond_native_type_name(uint8_t kind) {
    for(size_t index=0;index<sizeof native_type_names/sizeof native_type_names[0];index++)
        if((uint8_t)native_type_names[index].kind==kind)return native_type_names[index].name;
    return "<native>";
}

void diamond_format_value_type(char *buffer, size_t capacity,
                              DiamondValue value) {
    const char *name="<unknown>";
    if(value.kind==DIAMOND_VALUE_NIL) name="Nil";
    else if(value.kind==DIAMOND_VALUE_BOOL) name="Bool";
    else if(value.kind==DIAMOND_VALUE_INT) name="Int";
    else if(value.kind==DIAMOND_VALUE_FLOAT) name="Float";
    /* DIAMOND_VALUE_CLASS needs its own branch, checked before the
     * object switch below: DIAMOND_CLASS() (value.h) stores a small
     * `class_index` in the same union slot the object switch reads as
     * `value.as.object`, so a real class value -- reachable from
     * ordinary Diamond source, not just adversarial bytecode; `self`
     * inside a `def self.x` singleton method already *is* one, and
     * exit(Foo.bar()) crashed here confirming it -- would otherwise get
     * its class_index byte reinterpreted as a pointer and dereferenced.
     * Mirrors the same guard this file's own builder_format_value
     * already has for its DIAMOND_VALUE_CLASS branch (which has a chunk
     * to look the real class name up in; this function only reports a
     * value's type, like "String" or "Int", not the class's own name,
     * so "Class" is the right level of detail regardless). */
    else if(value.kind==DIAMOND_VALUE_CLASS) name="Class";
    /* value.as.object can also be null (or, after a single-bit fuzzer
     * mutation of the same input, any other small non-pointer value)
     * even though value.kind claims DIAMOND_VALUE_OBJECT: unverified,
     * hand-assembled bytecode (fuzz/execute_fuzzer.c, which deliberately
     * calls diamond_vm_run beneath diamond_verify_bytecode's own
     * trust boundary -- see that file's own doc comment) can still
     * reach exit_helper's "exit code must be an Int, got %s" with a
     * register holding whatever garbage bit pattern an earlier
     * instruction happened to produce. Not reachable from real Diamond
     * source (ProgramBuilder#run always verifies first), but cheap
     * insurance against a second SEGV in this same function regardless.
     * Every real DiamondObject kind (object.h) still needs an explicit
     * branch below: the catch-all reads the object through
     * DiamondInstance's own layout, which is only valid for
     * DIAMOND_OBJECT_INSTANCE -- any other kind reaching it is a
     * garbage-pointer read on whatever that kind's struct happens to
     * place at the same offset as DiamondInstance's `class` field. */
    else if(value.as.object==nullptr) name="<unknown>";
    else switch(value.as.object->kind) {
        case DIAMOND_OBJECT_STRING: name="String"; break;
        case DIAMOND_OBJECT_SYMBOL: name="Symbol"; break;
        case DIAMOND_OBJECT_ARRAY: name="Array"; break;
        case DIAMOND_OBJECT_HASH: name="Hash"; break;
        case DIAMOND_OBJECT_CLOSURE: name="Callable"; break;
        /* A promoted Int (see object.h's DiamondBignum) is still
         * conceptually an Int, not a distinct user-facing type. */
        case DIAMOND_OBJECT_BIGNUM: name="Int"; break;
        case DIAMOND_OBJECT_CELL: name="Cell"; break;
        case DIAMOND_OBJECT_FIBER: name="Fiber"; break;
        case DIAMOND_OBJECT_FILE: name="File"; break;
        case DIAMOND_OBJECT_LISTENER: name="Listener"; break;
        case DIAMOND_OBJECT_SOCKET: name="Socket"; break;
        case DIAMOND_OBJECT_UDP_SOCKET: name="UDPSocket"; break;
        case DIAMOND_OBJECT_TLS_SOCKET: name="TLSSocket"; break;
        case DIAMOND_OBJECT_REGEXP: name="Regexp"; break;
        case DIAMOND_OBJECT_PROGRAM_BUILDER: name="ProgramBuilder"; break;
        case DIAMOND_OBJECT_THREAD: name="Thread"; break;
        case DIAMOND_OBJECT_SQLITE3: name="SQLite3"; break;
        case DIAMOND_OBJECT_SQLITE3_STATEMENT: name="Statement"; break;
        case DIAMOND_OBJECT_POSTGRES: name="PostgreSQL"; break;
        case DIAMOND_OBJECT_MYSQL: name="MySQL"; break;
        case DIAMOND_OBJECT_TIME: name="Time"; break;
        case DIAMOND_OBJECT_PROCESS_RESULT: name="ProcessResult"; break;
        case DIAMOND_OBJECT_PROCESS_HANDLE: name="ProcessHandle"; break;
        case DIAMOND_OBJECT_PROCESS_STREAM: name="ProcessStream"; break;
        case DIAMOND_OBJECT_TENSOR: name="Tensor"; break;
        case DIAMOND_OBJECT_CHANNEL: name="Channel"; break;
        case DIAMOND_OBJECT_SUPERVISOR: name="Supervisor"; break;
        case DIAMOND_OBJECT_INSTANCE: {
            const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
            name=instance->class->name;
            break;
        }
    }
    snprintf(buffer,capacity,"%s",name);
}

/* strcat that stops at `capacity`, keeping `buffer` terminated. */
static void append_bounded(char *buffer,size_t capacity,const char *text) {
    size_t used=strlen(buffer);
    while(*text!='\0'&&used+1<capacity)buffer[used++]=*text++;
    buffer[used]='\0';
}

/* Appends the distinct element type names of `values` to `buffer`,
 * joined with " | ", at most four and then "...". */
static void append_element_types(char *buffer,size_t capacity,
        const DiamondValue *values,size_t count) {
    char seen[4][80];size_t seen_count=0;bool more=false;
    for(size_t index=0;index<count;index++) {
        char name[80];
        diamond_format_value_type(name,sizeof name,values[index]);
        bool known=false;
        for(size_t earlier=0;earlier<seen_count;earlier++)
            if(strcmp(seen[earlier],name)==0){known=true;break;}
        if(known)continue;
        if(seen_count==4){more=true;break;}
        snprintf(seen[seen_count++],sizeof seen[0],"%s",name);
    }
    for(size_t index=0;index<seen_count;index++) {
        if(index>0)append_bounded(buffer,capacity," | ");
        append_bounded(buffer,capacity,seen[index]);
    }
    if(more)append_bounded(buffer,capacity," | ...");
}

/* diamond_format_value_type, plus the element types a non-empty Array or
 * Hash actually holds -- "Array[String]" rather than just "Array" -- so a
 * failed `values: Array[Int]` check says what was in the Array, and a
 * closure's arity, so `3.times() do ... end` says "got Callable[0]". */
static void append_bound_type(char *buffer,size_t capacity,const DiamondChunk *chunk,
        const DiamondTypeBinding *binding,uint8_t node,size_t depth) {
    if(binding->invalid||node>=binding->node_count||depth>=DIAMOND_BOUND_TYPE_NODES||
       binding->nodes[node].count==0) {append_bounded(buffer,capacity,"?");return;}
    for(size_t i=0;i<binding->nodes[node].count;i++) {
        const DiamondBoundTypeMember member=binding->nodes[node].members[i];
        if(i>0)append_bounded(buffer,capacity," | ");
        if(member.id==DIAMOND_TYPE_NATIVE) {
            append_bounded(buffer,capacity,diamond_native_type_name(member.native_kind));
            continue;
        }
        append_bounded(buffer,capacity,type_name(chunk,member.id));
        if(member.id==DIAMOND_TYPE_CALLABLE&&member.signature<binding->signature_count) {
            const DiamondBoundSignature signature=binding->signatures[member.signature];
            if(signature.arity==UINT8_MAX)continue;
            append_bounded(buffer,capacity,signature.parameters_typed?"[[":"[");
            if(signature.parameters_typed) {
                for(size_t p=0;p<signature.arity;p++) {
                    if(p>0)append_bounded(buffer,capacity,", ");
                    append_bound_type(buffer,capacity,chunk,binding,signature.parameter_nodes[p],depth+1);
                }
                append_bounded(buffer,capacity,"]");
            } else {
                char arity[8];snprintf(arity,sizeof arity,"%u",(unsigned)signature.arity);
                append_bounded(buffer,capacity,arity);
            }
            if(signature.return_node!=UINT8_MAX) {
                append_bounded(buffer,capacity,", ");
                append_bound_type(buffer,capacity,chunk,binding,signature.return_node,depth+1);
            }
            append_bounded(buffer,capacity,"]");
            continue;
        }
        if(member.argument_node==UINT8_MAX)continue;
        append_bounded(buffer,capacity,"[");
        append_bound_type(buffer,capacity,chunk,binding,member.argument_node,depth+1);
        if(member.second_argument_node!=UINT8_MAX) {
            append_bounded(buffer,capacity,", ");
            append_bound_type(buffer,capacity,chunk,binding,member.second_argument_node,depth+1);
        }
        append_bounded(buffer,capacity,"]");
    }
}

void format_value_type_detailed(char *buffer,size_t capacity,
        const DiamondChunk *chunk,DiamondValue value) {
    diamond_format_value_type(buffer,capacity,value);
    if(value.kind!=DIAMOND_VALUE_OBJECT)return;
    if(value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
        if(instance->type_binding!=nullptr) {
            append_bounded(buffer,capacity,"[");
            for(size_t variable=0;variable<DIAMOND_CLASS_TYPE_PARAMETER_COUNT(instance->class);variable++) {
                if(variable!=0)append_bounded(buffer,capacity,", ");
                append_bound_type(buffer,capacity,instance->owner!=nullptr?instance->owner:chunk,
                    &instance->type_binding[variable],0,0);
            }
            append_bounded(buffer,capacity,"]");
        }
        return;
    }
    if(value.as.object->kind==DIAMOND_OBJECT_CLOSURE) {
        const DiamondClosure *closure=(const DiamondClosure *)value.as.object;
        if(closure->foreign_chunk!=nullptr||
           closure->function_index>=chunk->function_count)return;
        const DiamondFunction *function=chunk->functions[closure->function_index];
        if(function->has_variadic)return;
        /* The same self-less count value_matches_set compares. */
        snprintf(buffer,capacity,"Callable[%u]",
            (unsigned)(function->arity-diamond_function_self_offset(function)));
        return;
    }
    if(value.as.object->kind==DIAMOND_OBJECT_ARRAY) {
        const DiamondArray *array=(const DiamondArray *)value.as.object;
        if(array->count==0)return;
        snprintf(buffer,capacity,"Array[");
        append_element_types(buffer,capacity,array->values,array->count);
    } else if(value.as.object->kind==DIAMOND_OBJECT_HASH) {
        const DiamondHash *hash=(const DiamondHash *)value.as.object;
        if(hash->count==0)return;
        /* Entries hold key, value, and hash together; gather the keys and
         * the values into their own arrays. */
        DiamondValue *keys=malloc(hash->count*sizeof(DiamondValue));
        DiamondValue *values=malloc(hash->count*sizeof(DiamondValue));
        if(keys==nullptr||values==nullptr){free(keys);free(values);return;}
        for(size_t index=0;index<hash->count;index++) {
            keys[index]=hash->entries[index].key;
            values[index]=hash->entries[index].value;
        }
        snprintf(buffer,capacity,"Hash[");
        append_element_types(buffer,capacity,keys,hash->count);
        size_t used=strlen(buffer);
        if(used<capacity)snprintf(buffer+used,capacity-used,", ");
        append_element_types(buffer,capacity,values,hash->count);
        free(keys);free(values);
    } else return;
    const size_t used=strlen(buffer);
    if(used<capacity)snprintf(buffer+used,capacity-used,"]");
}

/* "X does not support 'op' with Y" -- shared by every arithmetic/
 * comparison operator's type-mismatch error site. When either operand
 * is a Callable, appends a hint: a bare `obj.method` (no parens) is a
 * method *reference*, not an invocation -- Diamond doesn't error on
 * that, it silently returns a Callable, so a call site expecting the
 * method's actual return value only finds out something's wrong here,
 * often nowhere near the real mistake (found the hard way in this
 * exact session: a bug that looked like data corruption -- `#<Closure>`
 * printed where a value should have been -- turned out to be exactly
 * this). Doesn't change that behavior itself (a bare method reference
 * is a deliberate, real feature -- singleton method references), only
 * makes the resulting error easier to actually debug. */
void format_operator_type_error(DiamondVm *vm,DiamondValue left_value,
        DiamondValue right_value,const char *op_name) {
    char left_type[80],right_type[80];
    diamond_format_value_type(left_type,sizeof left_type,left_value);
    diamond_format_value_type(right_type,sizeof right_type,right_value);
    const bool either_callable=
        (left_value.kind==DIAMOND_VALUE_OBJECT&&
         left_value.as.object->kind==DIAMOND_OBJECT_CLOSURE)||
        (right_value.kind==DIAMOND_VALUE_OBJECT&&
         right_value.as.object->kind==DIAMOND_OBJECT_CLOSURE);
    if(either_callable) {
        snprintf(vm->error,sizeof vm->error,
            "%s does not support '%s' with %s (a bare 'obj.method' without "
            "parens is a method reference, not a call -- did you mean 'obj.method()'?)",
            left_type,op_name,right_type);
    } else {
        snprintf(vm->error,sizeof vm->error,"%s does not support '%s' with %s",
            left_type,op_name,right_type);
    }
}

/* A double-quoted literal for `chars`, escaped so it reads back as the same
 * String: the quote and backslash, the common whitespace escapes, and any
 * other control byte as \xNN. Bytes >= 0x80 pass through (UTF-8). */
static bool builder_append_quoted(StringBuilder *builder,const char *chars,size_t length) {
    if(!builder_append(builder,"\"",1))return false;
    for(size_t index=0;index<length;index++) {
        const unsigned char byte=(unsigned char)chars[index];
        const char *escape=nullptr;
        char hex[5];
        switch(byte) {
            case '"': escape="\\\""; break;
            case '\\': escape="\\\\"; break;
            case '\n': escape="\\n"; break;
            case '\t': escape="\\t"; break;
            case '\r': escape="\\r"; break;
            default:
                if(byte<0x20||byte==0x7f) {
                    (void)snprintf(hex,sizeof hex,"\\x%02x",byte);
                    escape=hex;
                }
        }
        if(escape!=nullptr?!builder_append(builder,escape,strlen(escape)):
                           !builder_append(builder,(const char *)&byte,1))
            return false;
    }
    return builder_append(builder,"\"",1);
}

bool builder_format_value(StringBuilder *builder,DiamondValue value) {
    char scalar[96];int length=0;
    if(value.kind==DIAMOND_VALUE_NIL)return builder_append(builder,"nil",3);
    if(value.kind==DIAMOND_VALUE_BOOL)
        return builder_append(builder,value.as.boolean?"true":"false",
                              value.as.boolean?4:5);
    if(value.kind==DIAMOND_VALUE_INT) {
        length=snprintf(scalar,sizeof scalar,"%" PRId64,value.as.integer);
        return length>=0&&(size_t)length<sizeof scalar&&
            builder_append(builder,scalar,(size_t)length);
    }
    if(value_is_bignum(value)) {
        /* Writes into a freshly-sized buffer rather than through the
         * fixed 96-byte `scalar` array above -- a bignum has no bound
         * on its digit count. */
        const DiamondBignum *bignum=(const DiamondBignum *)value.as.object;
        const size_t capacity=diamond_bignum_string_length(bignum);
        char *digits=malloc(capacity);
        if(digits==nullptr)return false;
        const size_t digit_length=diamond_bignum_to_string(bignum,digits,capacity);
        const bool ok=builder_append(builder,digits,digit_length);
        free(digits);
        return ok;
    }
    if(value.kind==DIAMOND_VALUE_FLOAT) {
        const double real=value.as.real;
        if(isnan(real))return builder_append(builder,"NaN",3);
        if(isinf(real))
            return real<0?builder_append(builder,"-Infinity",9):
                          builder_append(builder,"Infinity",8);
        /* Shortest decimal that round-trips exactly -- see value.c's
         * fprint_float for the full rationale (same algorithm,
         * duplicated per this codebase's existing Int/Float
         * formatting convention between the two call sites), including
         * probing the exponent first so %g doesn't jump to scientific
         * notation for round values like 10.0 just because the search
         * started at a low precision. */
        uint64_t real_bits;memcpy(&real_bits,&real,sizeof real_bits);
        char probe[32];
        snprintf(probe,sizeof probe,"%.0e",real);
        const char *exponent_marker=strchr(probe,'e');
        const int exponent=exponent_marker?atoi(exponent_marker+1):0;
        /* Only bump the starting precision when it can actually keep %g
         * in fixed-point mode (exponent 1..16) -- see value.c's
         * fprint_float for the full rationale (past that, %g would use
         * scientific notation at every precision anyway, so starting at
         * 1 costs nothing and finds a genuinely shorter form when one
         * exists). */
        const int start_precision=(exponent>=1&&exponent<=16)?exponent+1:1;
        for(int precision=start_precision;precision<=17;precision++) {
            length=snprintf(scalar,sizeof scalar,"%.*g",precision,real);
            if(length<0||(size_t)length>=sizeof scalar)continue;
            char *end=nullptr;
            const double parsed=strtod(scalar,&end);
            uint64_t parsed_bits;memcpy(&parsed_bits,&parsed,sizeof parsed_bits);
            if(end!=scalar&&parsed_bits==real_bits)break;
        }
        if(length<0||(size_t)length>=sizeof scalar)return false;
        bool has_marker=false;
        for(int index=0;index<length;index++)
            if(scalar[index]=='.'||scalar[index]=='e'||scalar[index]=='E') {
                has_marker=true;break;
            }
        if(!builder_append(builder,scalar,(size_t)length))return false;
        return has_marker||builder_append(builder,".0",2);
    }
    if(value.kind==DIAMOND_VALUE_CLASS&&builder->format_context!=nullptr&&
       (size_t)value.as.class_index<builder->format_context->chunk->class_count) {
        /* With a chunk to look it up in, a class prints as its name. */
        const char *class_name=
            builder->format_context->chunk->classes[value.as.class_index]->name;
        return builder_append(builder,class_name,strlen(class_name));
    }
    if(value.kind==DIAMOND_VALUE_CLASS) {
        /* No chunk/program context reaches this value-only formatting
         * path (unlike DIAMOND_OBJECT_INSTANCE below, which prints its
         * real class name straight off the object's own `->class`
         * pointer) -- see value.c's diamond_value_fprint for the same
         * deliberate scope cut. Handled before the unconditional
         * `value.as.object` read just below, which would otherwise
         * reinterpret this value's `class_index` byte as a pointer. */
        length=snprintf(scalar,sizeof scalar,"#<Class:%u>",value.as.class_index);
        return length>=0&&(size_t)length<sizeof scalar&&
            builder_append(builder,scalar,(size_t)length);
    }
    const DiamondObject *object=value.as.object;
    if(object->kind==DIAMOND_OBJECT_STRING) {
        const DiamondString *string=(const DiamondString *)object;
        if(builder->inspect)return builder_append_quoted(builder,string->chars,string->length);
        return builder_append(builder,string->chars,string->length);
    }
    if(object->kind==DIAMOND_OBJECT_SYMBOL&&builder->inspect) {
        const DiamondSymbol *symbol=(const DiamondSymbol *)object;
        return builder_append(builder,":",1)&&
            builder_append(builder,symbol->chars,symbol->length);
    }
    if(object->kind==DIAMOND_OBJECT_INSTANCE&&builder->inspect)
        return builder_inspect_instance(builder,value);
    if(object->kind==DIAMOND_OBJECT_SYMBOL) {
        /* Bare name, no leading ':' -- matches Ruby's to_s/puts/
         * interpolation convention (only inspect/p show the colon there,
         * and Diamond has no separate inspect mechanism), and keeps
         * to_sym(to_s(sym)) == sym a true round trip. */
        const DiamondSymbol *symbol=(const DiamondSymbol *)object;
        return builder_append(builder,symbol->chars,symbol->length);
    }
    for(size_t index=0;index<builder->active_count;index++)
        if(builder->active[index]==object)
            return builder_append(builder,
                object->kind==DIAMOND_OBJECT_HASH?"{...}":"[...]",5);
    if(object->kind==DIAMOND_OBJECT_ARRAY||object->kind==DIAMOND_OBJECT_HASH) {
        if(builder->active_count==32)return builder_append(builder,"...",3);
        builder->active[builder->active_count++]=object;
        const bool hash=object->kind==DIAMOND_OBJECT_HASH;
        if(!builder_append(builder,hash?"{":"[",1))return false;
        /* The count is re-read every step: an element's to_s is arbitrary
         * code and may shrink this very collection. */
        for(size_t index=0;index<(hash?((const DiamondHash *)object)->count:
                                        ((const DiamondArray *)object)->count);
            index++) {
            if(index>0&&!builder_append(builder,", ",2))return false;
            if(hash) {
                const DiamondHashEntry entry=((const DiamondHash *)object)->entries[index];
                if(!builder_format_value(builder,entry.key)||
                   !builder_append(builder,": ",2)||
                   !builder_format_value(builder,entry.value))return false;
            } else if(!builder_format_value(builder,
                ((const DiamondArray *)object)->values[index]))return false;
        }
        builder->active_count--;
        return builder_append(builder,hash?"}":"]",1);
    }
    if(object->kind==DIAMOND_OBJECT_INSTANCE&&builder->format_context!=nullptr&&
       builder->active_count>0) {
        /* Nested in a collection: use the class's to_s, as a top-level
         * value already does. (At the top level, stringify_value has
         * already handled an instance with a to_s before calling here.) */
        FormatContext *context=builder->format_context;
        DiamondValue text=DIAMOND_NIL;
        context->status=stringify_value(context->vm,context->chunk,context->depth,
                                        value,&text);
        if(context->status!=DIAMOND_VM_OK)return false;
        const DiamondString *string=(const DiamondString *)text.as.object;
        return builder_append(builder,string->chars,string->length);
    }
    if(object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *instance=(const DiamondInstance *)object;
        length=snprintf(scalar,sizeof scalar,"#<%s>",instance->class->name);
        return length>=0&&(size_t)length<sizeof scalar&&
            builder_append(builder,scalar,(size_t)length);
    }
    if(object->kind==DIAMOND_OBJECT_TIME)
        return format_time_default((const DiamondTime *)object,builder);
    /* Every other object kind (Fiber, File, Thread, Tensor, Channel,
     * Supervisor, Regexp, SQLite3, ...) used to fall through to a single
     * hardcoded "#<Closure>" literal here -- correct only for a real
     * DIAMOND_OBJECT_CLOSURE (see format_operator_type_error's own
     * comment on why that exact string matters for the bare-method-
     * reference footgun), wrong and misleading for everything else:
     * `puts(some_tensor)` printed "#<Closure>", indistinguishable from
     * an actual closure printed by mistake. diamond_format_value_type already
     * has the real per-kind name table (used for type-error messages);
     * reused here instead of hand-duplicating it, and left updated
     * automatically the same way as any new object kind added there. */
    if(object->kind==DIAMOND_OBJECT_CLOSURE)
        return builder_append(builder,"#<Closure>",10);
    char kind_name[80];
    diamond_format_value_type(kind_name,sizeof kind_name,value);
    length=snprintf(scalar,sizeof scalar,"#<%s>",kind_name);
    return length>=0&&(size_t)length<sizeof scalar&&
        builder_append(builder,scalar,(size_t)length);
}

/* Fills vm->error with "uncaught exception: <detail>" for whatever
 * value was raised -- shared by every site that re-raises a value
 * already known to the VM (DIAMOND_OP_RAISE, and DIAMOND_OP_END_ENSURE's
 * own re-raise of a PENDING_EXCEPTION suspended by an ensure block)
 * rather than routing through catch_runtime_error (which only handles
 * status-code-originated errors, e.g. a native TYPE_ERROR, converting
 * them into a catchable exception instance first -- not applicable
 * here, since `exception` is already a real raised value). Previously
 * duplicated ad hoc at each such site; DIAMOND_OP_END_ENSURE's own copy
 * was missing entirely, so an exception that outlived an ensure block
 * before going uncaught printed a bare "uncaught exception" with no
 * class name or detail at all. */
/* The Int value of an already-whole Float: RangeError (INTEGER_OVERFLOW) for
 * NaN and infinities, which have none, and bignum promotion beyond int64_t,
 * matching every other overflow site. `what` names the caller in errors. */
DiamondVmStatus float_to_int(DiamondVm *vm,double whole,const char *what,
                                    DiamondValue *out) {
    if(isnan(whole)||isinf(whole)) {
        snprintf(vm->error,sizeof vm->error,"%s argument must be a finite Float",what);
        return DIAMOND_VM_INTEGER_OVERFLOW;
    }
    if(whole>=9223372036854775808.0||whole<-9223372036854775808.0) {
        const DiamondValue bignum=diamond_bignum_from_double(vm,whole);
        if(bignum.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
        *out=bignum;
        return DIAMOND_VM_OK;
    }
    *out=DIAMOND_INT((int64_t)whole);
    return DIAMOND_VM_OK;
}

/* Whether `instance` is an Exception or a subclass of one, resolving its class
 * chain in the chunk that owns the instance (or the root chunk). */
bool instance_is_exception(const DiamondVm *vm,const DiamondInstance *instance) {
    const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
    if(owner==nullptr)return false;
    const DiamondClass *ancestor=instance->class;
    while(ancestor!=nullptr) {
        if(ancestor==owner->classes[DIAMOND_CLASS_EXCEPTION])return true;
        ancestor=ancestor->superclass==DIAMOND_NO_CLASS?nullptr:
            owner->classes[ancestor->superclass];
    }
    return false;
}

/* show_origin: the exception is being re-raised away from where it was
 * first raised, so name that place too -- the "at" lines that follow only
 * start from here. */
void format_uncaught_exception_message(DiamondVm *vm,DiamondValue exception,
                                              bool show_origin) {
    if(exception.kind==DIAMOND_VALUE_INT)
        snprintf(vm->error,sizeof vm->error,"uncaught exception: %" PRId64,
                 exception.as.integer);
    else if(exception.kind==DIAMOND_VALUE_BOOL)
        snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                 exception.as.boolean?"true":"false");
    else if(exception.kind==DIAMOND_VALUE_NIL)
        snprintf(vm->error,sizeof vm->error,"uncaught exception: nil");
    else if(exception.kind==DIAMOND_VALUE_FLOAT||
            exception.kind==DIAMOND_VALUE_CLASS) {
        /* A class value keeps its index in the same union slot as an object
         * pointer, so it must not fall through to the object branches. */
        StringBuilder message_builder={};
        if(builder_format_value(&message_builder,exception))
            snprintf(vm->error,sizeof vm->error,"uncaught exception: %.*s",
                     (int)message_builder.length,message_builder.chars);
        else
            snprintf(vm->error,sizeof vm->error,"uncaught exception: <%s>",
                     exception.kind==DIAMOND_VALUE_CLASS?"class":"float");
        free(message_builder.chars);
    } else if(exception.kind!=DIAMOND_VALUE_OBJECT) {
        snprintf(vm->error,sizeof vm->error,"uncaught exception: object");
    } else if(exception.as.object->kind==DIAMOND_OBJECT_STRING) {
        const DiamondString *string=(const DiamondString *)exception.as.object;
        snprintf(vm->error,sizeof vm->error,"uncaught exception: %.*s",
                 (int)string->length,string->chars);
    } else if(exception.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *instance=(const DiamondInstance *)exception.as.object;
        /* Exceptions keep their message in field 0; include it so a report
         * says what went wrong, not only which class was raised. */
        const DiamondValue message=instance->field_count>0?instance->fields[0]:DIAMOND_NIL;
        if(instance_is_exception(vm,instance)&&message.kind==DIAMOND_VALUE_OBJECT&&
           message.as.object->kind==DIAMOND_OBJECT_STRING&&
           ((const DiamondString *)message.as.object)->length>0) {
            const DiamondString *text=(const DiamondString *)message.as.object;
            snprintf(vm->error,sizeof vm->error,"uncaught exception: %s: %.*s",
                     instance->class->name,(int)text->length,text->chars);
        } else {
            snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                     instance->class->name);
        }
    } else snprintf(vm->error,sizeof vm->error,"uncaught exception: object");
    if(!show_origin||exception.kind!=DIAMOND_VALUE_OBJECT||
       exception.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return;
    const DiamondInstance *raised=(const DiamondInstance *)exception.as.object;
    if(raised->field_count<=2||raised->fields[2].kind!=DIAMOND_VALUE_OBJECT||
       raised->fields[2].as.object->kind!=DIAMOND_OBJECT_ARRAY)return;
    const DiamondArray *backtrace=(const DiamondArray *)raised->fields[2].as.object;
    if(backtrace->count==0||backtrace->values[0].kind!=DIAMOND_VALUE_OBJECT||
       backtrace->values[0].as.object->kind!=DIAMOND_OBJECT_STRING)return;
    const DiamondString *origin=(const DiamondString *)backtrace->values[0].as.object;
    const size_t used=strlen(vm->error);
    if(used<sizeof vm->error)
        (void)snprintf(vm->error+used,sizeof vm->error-used," (raised at %.*s)",
                       (int)origin->length,origin->chars);
}

/* Runs a zero-argument instance method that must return a String (to_s,
 * inspect) and stores the result. `label` names the method in the error. */
DiamondVmStatus run_instance_string_method(DiamondVm *vm,size_t depth,
        const DiamondChunk *owner,DiamondValue value,const DiamondMethod *method,
        const char *label,DiamondValue *out) {
    if(method->required_arity>0)return DIAMOND_VM_ARITY_ERROR;
    /* See DIAMOND_OP_INVOKE_TYPED's own comment on function_chunk. */
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner;
    const DiamondFunction *fn=function_chunk->functions[method->function_index];
    const DiamondChunk child={.name=fn->name,.code=fn->code,
      .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
      .constants=fn->constants,.constant_count=fn->constant_count,
      .strings=fn->strings,.string_count=fn->string_count,
      .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
      .functions=function_chunk->functions,.function_count=function_chunk->function_count,
      .classes=function_chunk->classes,.class_count=function_chunk->class_count,
      .interfaces=function_chunk->interfaces,.interface_count=function_chunk->interface_count,
      .parameter_type_sets=fn->parameter_type_sets,
      .type_variable_count=fn->type_variable_count,
      .class_type_variable=fn->class_type_variable,
      .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
      .register_count=fn->register_count,.has_variadic=fn->has_variadic};
    DiamondValue args[1+DIAMOND_MAX_BOUND_VALUES]={value};
    for(size_t i=0;i<method->bound_value_count;i++)
        args[1+i]=method->bound_values[i];
    DiamondValue converted=DIAMOND_NIL;
    DiamondVmStatus status=run_chunk(&child,vm,args,
                                      (size_t)1+method->bound_value_count,
                                      depth+1,nullptr,&converted);
    if(status!=DIAMOND_VM_OK)return status;
    if(converted.kind!=DIAMOND_VALUE_OBJECT||
       converted.as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"%s must return String",label);
        return DIAMOND_VM_TYPE_ERROR;
    }
    *out=converted;return DIAMOND_VM_OK;
}

/* inspect for an Instance: its class's own inspect when it defines one,
 * otherwise `#<Name field=value, ...>`. Fields of an Instance already being
 * printed (a cycle) show as `#<Name ...>`. */
bool builder_inspect_instance(StringBuilder *builder,DiamondValue value) {
    const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
    FormatContext *context=builder->format_context;
    if(context!=nullptr) {
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:context->vm->root_chunk;
        const DiamondMethod *method=lookup_method(owner,instance->class,"inspect",7);
        if(method!=nullptr) {
            DiamondValue text=DIAMOND_NIL;
            context->status=run_instance_string_method(context->vm,context->depth,
                owner,value,method,"inspect",&text);
            if(context->status!=DIAMOND_VM_OK)return false;
            const DiamondString *string=(const DiamondString *)text.as.object;
            return builder_append(builder,string->chars,string->length);
        }
    }
    const char *name=instance->class->name;
    for(size_t index=0;index<builder->active_count;index++)
        if(builder->active[index]==(const DiamondObject *)instance)
            return builder_append(builder,"#<",2)&&
                builder_append(builder,name,strlen(name))&&
                builder_append(builder," ...>",5);
    if(builder->active_count==32)return builder_append(builder,"...",3);
    builder->active[builder->active_count++]=(const DiamondObject *)instance;
    bool ok=builder_append(builder,"#<",2)&&builder_append(builder,name,strlen(name));
    const size_t fields=instance->field_count<instance->class->field_count?
        instance->field_count:instance->class->field_count;
    for(size_t index=0;ok&&index<fields;index++) {
        const char *field=instance->class->fields[index];
        ok=builder_append(builder,index==0?" ":", ",index==0?1:2)&&
           builder_append(builder,field,strlen(field))&&
           builder_append(builder,"=",1)&&
           builder_format_value(builder,instance->fields[index]);
    }
    builder->active_count--;
    return ok&&builder_append(builder,">",1);
}

DiamondVmStatus inspect_value(DiamondVm *vm,const DiamondChunk *chunk,
                                     size_t depth,DiamondValue value,
                                     DiamondValue *out) {
    FormatContext context={.vm=vm,.chunk=chunk,.depth=depth,.status=DIAMOND_VM_OK};
    StringBuilder builder={.format_context=&context,.inspect=true};
    if(!builder_format_value(&builder,value)) {
        free(builder.chars);
        return context.status!=DIAMOND_VM_OK?context.status:DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondString *string=allocate_string(vm,builder.chars,builder.length);
    free(builder.chars);
    if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
}

DiamondVmStatus stringify_value(DiamondVm *vm,const DiamondChunk *chunk,
                                        size_t depth,DiamondValue value,
                                        DiamondValue *out) {
    /* See invoke_operator_method's own comment on this same pattern. */
    (void)chunk;
    if(value.kind==DIAMOND_VALUE_OBJECT&&
       value.as.object->kind==DIAMOND_OBJECT_STRING) {
        *out=value;return DIAMOND_VM_OK;
    }
    if(value.kind==DIAMOND_VALUE_OBJECT&&
       value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
        const DiamondMethod *method=lookup_method(owner,instance->class,
            "to_s",sizeof("to_s")-1);
        if(method!=nullptr)
            return run_instance_string_method(vm,depth,owner,value,method,"to_s",out);
    }
    FormatContext context={.vm=vm,.chunk=chunk,.depth=depth,.status=DIAMOND_VM_OK};
    StringBuilder builder={.format_context=&context};
    if(!builder_format_value(&builder,value)) {
        free(builder.chars);
        return context.status!=DIAMOND_VM_OK?context.status:DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondString *string=allocate_string(vm,builder.chars,builder.length);
    free(builder.chars);
    if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
}

/* Array#join's real body, factored out of run_chunk's own INVOKE case
 * for the same stack-frame-isolation reason as program_builder_run_
 * helper above -- StringBuilder's own `active[32]` pointer array alone
 * is real stack weight, and run_chunk is a single, deeply (self-)
 * recursive function whose safe recursion depth is a measured
 * constraint (see DIAMOND_MAX_CALL_DEPTH's own comment); a local this
 * size declared directly inside its switch gets baked into every
 * run_chunk call's stack frame, not just calls that actually reach
 * #join. stringify_value (not builder_format_value) matches
 * interpolation's own `"#{value}"` semantics exactly, including
 * calling a user-defined to_s override on an Instance -- the behavior
 * array_join's old Diamond-level `lib/core.d6` loop had via its own
 * `"#{}"` interpolation, before it became this native, O(n) method. */
DiamondVmStatus array_join_helper(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,const DiamondArray *array,const char *separator_chars,
        size_t separator_length,DiamondValue *out) {
    StringBuilder builder={};
    DiamondVmStatus status=DIAMOND_VM_OK;
    for(size_t index=0;index<array->count;index++) {
        if(index>0&&!builder_append(&builder,separator_chars,separator_length)) {
            status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
        DiamondValue piece=DIAMOND_NIL;
        status=stringify_value(vm,chunk,depth,array->values[index],&piece);
        if(status!=DIAMOND_VM_OK)break;
        const DiamondString *piece_string=(const DiamondString *)piece.as.object;
        if(!builder_append(&builder,piece_string->chars,piece_string->length)) {
            status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
    }
    if(status!=DIAMOND_VM_OK) {free(builder.chars);return status;}
    DiamondString *joined=allocate_string(vm,builder.chars?builder.chars:"",builder.length);
    free(builder.chars);
    if(joined==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(joined);
    return DIAMOND_VM_OK;
}
