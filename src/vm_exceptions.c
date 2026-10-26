#define _GNU_SOURCE

#include "vm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool catch_exception(DiamondVm *vm,const DiamondChunk *chunk,
                            UnwindHandler *handlers,size_t *handler_count,
                            PendingUnwind *pending,DiamondValue *registers,
                            size_t *ip) {
    while(*handler_count>0) {
        UnwindHandler *handler=&handlers[*handler_count-1];
        if(handler->kind==HANDLER_ENSURE) {
            *ip=enter_ensure(handler,pending,(PendingUnwind){
                .kind=PENDING_EXCEPTION,.value=vm->exception});
            vm->has_exception=false;return true;
        }
        const UnwindHandler popped=pop_unwind_handler(handlers,handler_count,pending);
        if(popped.kind==HANDLER_ACTIVE_ENSURE)continue;
        if(!handler->enabled)continue;
        bool matches=handler->type_count==0;
        for(size_t i=0;i<handler->type_count&&!matches;i++)
            matches=value_matches_type(chunk,vm->exception,handler->types[i]);
        if(!matches)continue;
        *ip=handler->target;
        registers[handler->destination]=vm->exception;
        vm->has_exception=false;vm->error[0]='\0';return true;
    }
    return false;
}

/* DIAMOND_VM_OUT_OF_MEMORY deliberately has no case below (falls to
 * `default: return UINT8_MAX`, meaning it can never match any rescue
 * clause) -- a genuine host allocator failure is treated as unrecoverable
 * by design: the collector has already tried and failed to free anything,
 * and running more Diamond code to handle it (which itself needs to
 * allocate, starting with the exception instance this function's own
 * caller constructs) risks cascading rather than actually recovering.
 * The one exception (so to speak): vm->memory_limit_tripped is set only
 * by maybe_collect's own configured-budget check (src/vm.c, "Resource
 * limits" section), never by a real malloc/calloc/realloc failure --
 * when it's set, THIS specific OOM is known to be an artificial, self-
 * imposed cap with plenty of real host memory still available, not a
 * genuine crisis, so it's surfaced as the same catchable ResourceLimitError
 * instruction-count/wall-clock budgets already use, rather than silently
 * reusing the uncatchable path a real OOM deliberately takes. */
static uint16_t exception_class_for_status(const DiamondVm *vm,DiamondVmStatus status) {
    if(status==DIAMOND_VM_OUT_OF_MEMORY)
        return vm->memory_limit_tripped&&!vm->memory_hard_stopped?
            DIAMOND_CLASS_RESOURCE_LIMIT_ERROR:DIAMOND_NO_CLASS;
    switch(status) {
        case DIAMOND_VM_TYPE_ERROR: return DIAMOND_CLASS_TYPE_ERROR;
        case DIAMOND_VM_INTEGER_OVERFLOW: return DIAMOND_CLASS_RANGE_ERROR;
        case DIAMOND_VM_DIVISION_BY_ZERO: return DIAMOND_CLASS_ZERO_DIVISION_ERROR;
        case DIAMOND_VM_ARITY_ERROR: return DIAMOND_CLASS_ARGUMENT_ERROR;
        case DIAMOND_VM_STACK_OVERFLOW: return DIAMOND_CLASS_SYSTEM_STACK_ERROR;
        case DIAMOND_VM_INDEX_ERROR: return DIAMOND_CLASS_INDEX_ERROR;
        case DIAMOND_VM_FIBER_NOT_RESUMABLE: return DIAMOND_CLASS_FIBER_ERROR;
        case DIAMOND_VM_YIELD_WITHOUT_FIBER: return DIAMOND_CLASS_FIBER_ERROR;
        case DIAMOND_VM_IO_ERROR: return DIAMOND_CLASS_IO_ERROR;
        case DIAMOND_VM_REGEXP_ERROR: return DIAMOND_CLASS_REGEXP_ERROR;
        case DIAMOND_VM_WOULD_BLOCK: return DIAMOND_CLASS_WOULD_BLOCK_ERROR;
        case DIAMOND_VM_PROGRAM_ERROR: return DIAMOND_CLASS_RUNTIME_ERROR;
        case DIAMOND_VM_CONSTANT_ERROR: return DIAMOND_CLASS_RUNTIME_ERROR;
        case DIAMOND_VM_THREAD_ERROR: return DIAMOND_CLASS_THREAD_ERROR;
        case DIAMOND_VM_SQLITE3_ERROR: return DIAMOND_CLASS_SQLITE3_ERROR;
        case DIAMOND_VM_POSTGRES_ERROR: return DIAMOND_CLASS_POSTGRES_ERROR;
        case DIAMOND_VM_MYSQL_ERROR: return DIAMOND_CLASS_MYSQL_ERROR;
        case DIAMOND_VM_NO_METHOD_ERROR: return DIAMOND_CLASS_NO_METHOD_ERROR;
        case DIAMOND_VM_JSON_ERROR: return DIAMOND_CLASS_JSON_ERROR;
        case DIAMOND_VM_SUPERVISOR_ERROR: return DIAMOND_CLASS_SUPERVISOR_ERROR;
        case DIAMOND_VM_SANDBOX_ERROR: return DIAMOND_CLASS_SANDBOX_ERROR;
        case DIAMOND_VM_RESOURCE_LIMIT_ERROR: return DIAMOND_CLASS_RESOURCE_LIMIT_ERROR;
        case DIAMOND_VM_FROZEN_ERROR: return DIAMOND_CLASS_FROZEN_ERROR;
        default: return DIAMOND_NO_CLASS;
    }
}

bool catch_runtime_error(DiamondVm *vm,const DiamondChunk *chunk,
                                DiamondVmStatus status,UnwindHandler *handlers,
                                size_t *handler_count,PendingUnwind *pending,
                                DiamondValue *registers,size_t *ip) {
    const uint16_t class_index=exception_class_for_status(vm,status);
    if(class_index==DIAMOND_NO_CLASS || (size_t)class_index>=chunk->class_count)return false;
    char message[sizeof vm->error];
    (void)snprintf(message,sizeof message,"%s",vm->error[0]!='\0'?vm->error:
                   diamond_vm_status_name(status));
    /* vm->error collects an "\n  at name:line:col" line from every frame
     * the failure unwound through (RECORD_ERROR), for the uncaught-error
     * report. The exception's message is just the first part, the same
     * text a raised Exception.new(message) carries. */
    char *location=strstr(message,"\n  at ");
    if(location!=nullptr)*location='\0';
    DiamondInstance *exception=allocate_instance(vm,chunk->classes[class_index],nullptr);
    if(exception==nullptr)return false;
    vm->exception=DIAMOND_OBJECT(exception);vm->has_exception=true;
    DiamondString *text=allocate_string(vm,message,strlen(message));
    if(text==nullptr)return false;
    if(exception->field_count>0) {
        exception->fields[0]=DIAMOND_OBJECT(text);
        /* allocate_string above can itself have triggered a minor
         * collection that promoted `exception` (reachable via
         * vm->exception, set just above) before this assignment ran --
         * this raw field write bypasses DIAMOND_OP_SET_IVAR entirely, so
         * it needs its own barrier call rather than relying on the
         * opcode-level one. */
        if(!gc_write_barrier(vm,(DiamondObject *)exception))return false;
    }
    /* Its backtrace, like a raised exception's: the frames the failure
     * already unwound through (their "at" lines, innermost first), then
     * the frames still live, starting with this rescuing one. */
    DiamondArray *backtrace=install_backtrace(vm,exception);
    if(backtrace!=nullptr) {
        const char *line=location!=nullptr?strstr(vm->error,"\n  at "):nullptr;
        while(line!=nullptr) {
            line+=6;
            const char *end=strchr(line,'\n');
            const size_t length=end!=nullptr?(size_t)(end-line):strlen(line);
            DiamondString *entry=allocate_string(vm,line,length);
            if(entry==nullptr||!array_push(vm,backtrace,DIAMOND_OBJECT(entry)))break;
            line=end!=nullptr&&strncmp(end,"\n  at ",6)==0?end:nullptr;
        }
        backtrace_append_live_frames(vm,backtrace);
    }
    /* Only fill in a generic header when vm->error is still empty (no
     * RECORD_ERROR has run yet for this failure, e.g. the origin frame
     * itself is the one with a handler) -- if it already holds the
     * detailed message + origin "at file:line:col" that RECORD_ERROR
     * wrote at the true raise site, leave it alone. Overwriting it
     * unconditionally here used to discard that detail the moment an
     * exception got caught by an *ensure* handler (which re-raises
     * rather than truly handling it, see DIAMOND_OP_END_ENSURE): the
     * final uncaught-exception message would then start over from
     * wherever the ensure block re-raised, several frames away from
     * the real cause, even though this exact detail survives intact on
     * the exception instance's own .message field above. Leaving
     * vm->error as-is here lets RECORD_ERROR keep appending later "at
     * ..." frames onto the *original* message instead of a fresh one. */
    if(vm->error[0]=='\0')
        (void)snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                       exception->class->name);
    return catch_exception(vm,chunk,handlers,handler_count,pending,registers,ip);
}

/* Did this INVOKE pass a `do ... end` block to an instance method with
 * no &block parameter? Ruby passes blocks implicitly; Diamond doesn't. */
static bool invoke_passes_unaccepted_block(const DiamondChunk *chunk,
        DiamondValue receiver,const DiamondStringConstant *method_name,
        const DiamondValue *arguments,uint8_t argc) {
    if(argc==0)return false;
    const DiamondValue last=arguments[argc-1];
    if(last.kind!=DIAMOND_VALUE_OBJECT||
       last.as.object->kind!=DIAMOND_OBJECT_CLOSURE||
       !((const DiamondClosure *)last.as.object)->is_block)return false;
    if(receiver.kind!=DIAMOND_VALUE_OBJECT||
       receiver.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return false;
    const DiamondInstance *instance=(const DiamondInstance *)receiver.as.object;
    const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:chunk;
    const DiamondMethod *method=lookup_method(owner,instance->class,
        method_name->chars,method_name->length);
    if(method==nullptr||method->has_variadic||argc!=method->arity+1)return false;
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner;
    if(method->function_index>=function_chunk->function_count)return false;
    return !function_chunk->functions[method->function_index]->has_block_parameter;
}

/* A bare "wrong number of arguments" doesn't say which call. When the
 * failing instruction is a method call, name the method, the receiver's
 * type, and how many arguments it got. Returns the message length, or 0
 * when the instruction isn't a method call. */
size_t describe_invoke_arity_error(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondValue *registers,size_t offset) {
    if(offset+10>chunk->code_count)return 0;
    const DiamondOpCode opcode=(DiamondOpCode)chunk->code[offset];
    if(opcode!=DIAMOND_OP_INVOKE&&opcode!=DIAMOND_OP_INVOKE_MONO&&
       opcode!=DIAMOND_OP_INVOKE_TYPED)return 0;
    const uint16_t recv=(uint16_t)((chunk->code[offset+3]<<8)|chunk->code[offset+4]);
    const uint16_t name=(uint16_t)((chunk->code[offset+5]<<8)|chunk->code[offset+6]);
    const uint16_t base=(uint16_t)((chunk->code[offset+7]<<8)|chunk->code[offset+8]);
    const uint8_t argc=chunk->code[offset+9];
    if(name>=chunk->string_count)return 0;
    const DiamondStringConstant *method_name=&chunk->strings[name];
    char receiver[80];
    diamond_format_value_type(receiver,sizeof receiver,registers[recv]);
    const int written=invoke_passes_unaccepted_block(chunk,registers[recv],
            method_name,&registers[base],argc)?
        snprintf(vm->error,sizeof vm->error,
            "'%.*s' on %s does not take a block; add a &block parameter to accept one",
            (int)method_name->length,method_name->chars,receiver):
        snprintf(vm->error,sizeof vm->error,
        "wrong number of arguments for '%.*s' on %s (given %u)",
        (int)method_name->length,method_name->chars,receiver,argc);
    return written<0?0:strlen(vm->error);
}

/* Is `opcode` a call whose first operand is its destination register and
 * which reads every operand before invoking anything? A block's break
 * lands in exactly such an instruction: the one that passed the block. */
static bool nonlocal_break_can_land(uint8_t opcode) {
    switch((DiamondOpCode)opcode) {
        case DIAMOND_OP_CALL: case DIAMOND_OP_CALL_TYPED: case DIAMOND_OP_CALL_CLOSURE:
        case DIAMOND_OP_NEW: case DIAMOND_OP_INVOKE: case DIAMOND_OP_INVOKE_MONO:
        case DIAMOND_OP_INVOKE_TYPED: case DIAMOND_OP_SUPER:
        case DIAMOND_OP_INVOKE_SELF_METHOD: case DIAMOND_OP_CALL_SPREAD:
        case DIAMOND_OP_INVOKE_SPREAD: case DIAMOND_OP_CALL_CLOSURE_SPREAD:
        case DIAMOND_OP_NEW_SPREAD: case DIAMOND_OP_CALL_SINGLETON_SPREAD:
        case DIAMOND_OP_CALL_TYPED_SPREAD: case DIAMOND_OP_INVOKE_TYPED_SPREAD:
        case DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD: case DIAMOND_OP_CALL_KEYWORD_SPREAD:
        case DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD: case DIAMOND_OP_INVOKE_KEYWORDS:
        case DIAMOND_OP_INVOKE_TYPED_KEYWORDS: case DIAMOND_OP_CALL_CLOSURE_KEYWORDS:
        case DIAMOND_OP_NEW_KEYWORDS: case DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
        case DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS:
            return true;
        default:
            return false;
    }
}

static void clear_nonlocal_exit(DiamondVm *vm) {
    vm->nonlocal_value=DIAMOND_NIL;
    vm->nonlocal_target=0;
}

/* A DIAMOND_VM_NONLOCAL_EXIT reaching `frame` on its way out. The target
 * frame either lands a break -- the call instruction it's inside receives
 * the value and execution continues after it, since every call opcode has
 * read all its operands before invoking anything -- or returns the value
 * as `return` would, running ensure handlers first. Any other frame runs
 * its own ensure handlers and passes the exit on. */
NonlocalOutcome nonlocal_exit_arrives(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondFrame *frame,DiamondValue *registers,UnwindHandler *handlers,
        size_t *handler_count,PendingUnwind *pending,size_t *ip,
        size_t instruction_offset,DiamondValue *result) {
    const bool here=vm->nonlocal_target==frame->serial;
    if(here&&vm->nonlocal_is_break) {
        const uint8_t opcode=chunk->code[instruction_offset];
        if(!nonlocal_break_can_land(opcode)||instruction_offset+3>chunk->code_count) {
            snprintf(vm->error,sizeof vm->error,
                "break from a block that wasn't passed to a method call here");
            return NONLOCAL_INVALID;
        }
        const uint16_t destination=(uint16_t)(
            ((unsigned)chunk->code[instruction_offset+1]<<8)|
            chunk->code[instruction_offset+2]);
        if(destination>=frame->register_count) {
            snprintf(vm->error,sizeof vm->error,"invalid break destination");
            return NONLOCAL_INVALID;
        }
        registers[destination]=vm->nonlocal_value;
        clear_nonlocal_exit(vm);
        return NONLOCAL_CONTINUE;
    }
    while(*handler_count>0&&handlers[*handler_count-1].kind!=HANDLER_ENSURE)
        (void)pop_unwind_handler(handlers,handler_count,pending);
    if(*handler_count>0) {
        const PendingUnwind next=here?
            (PendingUnwind){.kind=PENDING_RETURN,.value=vm->nonlocal_value}:
            (PendingUnwind){.kind=PENDING_NONLOCAL,.value=vm->nonlocal_value,
                .nonlocal_target=vm->nonlocal_target,
                .nonlocal_is_break=vm->nonlocal_is_break};
        *ip=enter_ensure(&handlers[*handler_count-1],pending,next);
        if(here) {clear_nonlocal_exit(vm);}
        return NONLOCAL_CONTINUE;
    }
    if(here) {
        *result=vm->nonlocal_value;
        clear_nonlocal_exit(vm);
        return NONLOCAL_RETURNED;
    }
    if(frame->previous==nullptr) {
        clear_nonlocal_exit(vm);
        snprintf(vm->error,sizeof vm->error,vm->nonlocal_is_break?
            "break from a block outside the call it was passed to":
            "return from a block outside the method it was written in");
        return NONLOCAL_INVALID;
    }
    return NONLOCAL_PASS_ON;
}
