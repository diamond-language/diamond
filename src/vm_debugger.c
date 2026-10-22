#include "vm_internal.h"
#include "disassemble.h"
#include "message_length.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Writes `length` bytes of `data` to `fd`, retrying on a short write and
 * on EINTR. Returns false on any other write error or on a peer that's
 * gone (write returning 0 forever isn't a real possibility for a pipe,
 * but is treated as failure rather than looping if it ever happened). */
static bool debug_pipe_write_all(int fd,const char *data,size_t length) {
    size_t written=0;
    while(written<length) {
        const ssize_t result=write(fd,data+written,length-written);
        if(result<0) { if(errno==EINTR)continue; return false; }
        if(result==0)return false;
        written+=(size_t)result;
    }
    return true;
}

static bool debug_pipe_read_exact(int fd,char *data,size_t length) {
    size_t total=0;
    while(total<length) {
        const ssize_t result=read(fd,data+total,length-total);
        if(result<0) { if(errno==EINTR)continue; return false; }
        if(result==0)return false; /* EOF: the DAP client/dap process is gone. */
        total+=(size_t)result;
    }
    return true;
}

/* Reads one \r\n-terminated header line (without the terminator) off
 * `fd`, one byte at a time -- mirrors lsp/rpc.c's read_header_line, but
 * over a raw fd instead of a FILE*, and without linking lsp/rpc.c/json.c
 * into the core VM (see debugger_structured_helper's own comment on
 * why). Returns false on EOF, a read error, or a line too long for
 * `capacity`. */
static bool debug_pipe_read_header_line(int fd,char *buffer,size_t capacity) {
    size_t length=0;
    while(true) {
        char next=0;
        if(!debug_pipe_read_exact(fd,&next,1))return false;
        if(next=='\n') {
            if(length>0&&buffer[length-1]=='\r')length--;
            buffer[length]='\0';
            return true;
        }
        if(length+1>=capacity)return false;
        buffer[length++]=next;
    }
}

/* Hand-scans exactly the fixed command shapes dap/main.c ever sends over
 * the debug control channel -- not a JSON parser (see
 * debugger_structured_helper's own comment on why the core VM doesn't
 * link one): `{"command":"continue"}`, `{"command":"setBreakpoints",
 * "lines":[1,2,3]}`, and the stepping commands `{"command":"next"}`/
 * `"stepIn"`/`"stepOut"` (no other fields). Plain substring/number
 * scanning is safe here specifically because this wire format is
 * internal (dap/main.c is the only possible sender, never exposed to a
 * DAP client's own, genuinely untrusted JSON) and fixed-shape -- no
 * nesting, no escaping, and no whitespace variance beyond what's scanned
 * for explicitly. `body` must be nul-terminated (debug_pipe_read_
 * command's own caller already needs to null-terminate it to bound the
 * scan); `lines`/`line_count` are left untouched for anything but
 * DIAMOND_DEBUG_COMMAND_SET_BREAKPOINTS. A `lines` array longer than
 * `capacity` still reports the real total in `*line_count` (matching
 * add_string_range's own "count what's there, cap what's stored"
 * convention elsewhere in this codebase) -- the caller only ever copies
 * min(*line_count,capacity) entries out. */
static DiamondDebugCommandKind parse_debug_command(const char *body,
        size_t *lines,size_t *line_count,size_t capacity,bool *are_offsets,
        char *step_target) {
    if(strstr(body,"\"setBreakpoints\"")!=nullptr) {
        size_t count=0;
        const char *lines_key=strstr(body,"\"offsets\"");
        *are_offsets=lines_key!=nullptr;
        if(lines_key==nullptr)lines_key=strstr(body,"\"lines\"");
        const char *cursor=lines_key!=nullptr?strchr(lines_key,'['):nullptr;
        if(cursor!=nullptr) {
            cursor++;
            while(*cursor!='\0'&&*cursor!=']') {
                while(*cursor==' '||*cursor==',')cursor++;
                if(*cursor=='\0'||*cursor==']')break;
                char *end=nullptr;
                const unsigned long long parsed=strtoull(cursor,&end,10);
                if(end==cursor)break;
                if(count<capacity)lines[count]=(size_t)parsed;
                count++;
                cursor=end;
            }
        }
        *line_count=count<capacity?count:capacity;
        return DIAMOND_DEBUG_COMMAND_SET_BREAKPOINTS;
    }
    if(strstr(body,"\"continue\"")!=nullptr)return DIAMOND_DEBUG_COMMAND_CONTINUE;
    if(strstr(body,"\"next\"")!=nullptr)return DIAMOND_DEBUG_COMMAND_NEXT;
    if(strstr(body,"\"stepIn\"")!=nullptr) {
        /* An optional `"target":"name"` (stepInTargets): at most 63 bytes. */
        step_target[0]='\0';
        const char *key=strstr(body,"\"target\"");
        const char *open=key!=nullptr?strchr(key+8,'"'):nullptr;
        if(open!=nullptr) {
            size_t length=0;
            for(const char *cursor=open+1;*cursor!='\0'&&*cursor!='"'&&length<63;cursor++)
                step_target[length++]=*cursor;
            step_target[length]='\0';
        }
        return DIAMOND_DEBUG_COMMAND_STEP_IN;
    }
    if(strstr(body,"\"stepOut\"")!=nullptr)return DIAMOND_DEBUG_COMMAND_STEP_OUT;
    return DIAMOND_DEBUG_COMMAND_NONE;
}

/* Blocks reading one Content-Length-framed command off `fd`. v1 only
 * ever discarded the body here (exactly one command shape existed,
 * `{"command":"continue"}`, so there was nothing to branch on) -- live
 * breakpoints (docs/debugging.md) added a second shape, so the body now
 * gets copied into a bounded stack buffer and handed to
 * parse_debug_command instead. A command larger than the buffer is
 * still fully drained (so the pipe lands correctly at the *next* frame
 * boundary either way) but reports DIAMOND_DEBUG_COMMAND_NONE -- best-
 * effort, matching every other malformed-input case here. Returns false
 * on any framing/read problem, in which case `*kind` is left at
 * DIAMOND_DEBUG_COMMAND_NONE; the caller treats that the same as an
 * ordinary "continue" (see its own comment). */
bool debug_pipe_read_command(int fd,DiamondDebugCommandKind *kind,
        size_t *lines,size_t *line_count,size_t capacity,bool *are_offsets,
        char *step_target) {
    *kind=DIAMOND_DEBUG_COMMAND_NONE;
    step_target[0]='\0';
    *line_count=0;
    size_t content_length=0;
    bool have_content_length=false;
    char line[256];
    while(true) {
        if(!debug_pipe_read_header_line(fd,line,sizeof line))return false;
        if(line[0]=='\0')break;
        static constexpr char prefix[]="Content-Length:";
        static constexpr size_t prefix_length=sizeof(prefix)-1;
        if(strncmp(line,prefix,prefix_length)==0) {
            const char *value=line+prefix_length;
            if(have_content_length||!diamond_parse_message_length(value,&content_length))
                return false;
            have_content_length=true;
        }
    }
    if(!have_content_length)return false;
    char body[16384];
    const size_t body_capacity=sizeof body-1;
    const size_t body_length=
        (size_t)content_length<body_capacity?(size_t)content_length:body_capacity;
    if(!debug_pipe_read_exact(fd,body,body_length))return false;
    body[body_length]='\0';
    char discard[256];
    size_t remaining=(size_t)content_length-body_length;
    while(remaining>0) {
        const size_t next=remaining<sizeof discard?remaining:sizeof discard;
        if(!debug_pipe_read_exact(fd,discard,next))return false;
        remaining-=next;
    }
    if(body_length==(size_t)content_length)
        *kind=parse_debug_command(body,lines,line_count,capacity,are_offsets,step_target);
    return true;
}

/* DIAMOND_OP_DEBUGGER/DIAMOND_OP_BREAKPOINT_CHECK's shared structured,
 * DAP-facing pause path -- see DiamondVm.debug_fd's own comment (src/
 * vm.h) for when debugger_helper takes this branch instead of its
 * ordinary print-to-stdout/getchar() path. Hand-formats a JSON "paused"
 * payload (`{"event":"paused","reason":"breakpoint"|"step","stack":
 * [{"name","line","column"},...],"locals":[{"name","value"},...]}`) --
 * `reason` lets dap/main.c's own `stopped` event distinguish a real
 * breakpoint from a step for the DAP client's own UI (docs/debugging.md's
 * "Stepping" section) -- with snprintf/GrowBuffer rather than linking
 * lsp/json.c into the core VM/`diamond` binary -- dap/main.c (which
 * already links lsp/json.c and lsp/rpc.c for its own DAP-client-facing
 * transport) is the one side of this pipe that ever needs a real JSON
 * parser; the VM only ever produces this one payload shape and only ever
 * reads back a small, fixed set of command shapes (see
 * parse_debug_command), so it never needs to parse general JSON at all.
 * Content-Length-frames the payload onto vm->debug_fd the same way
 * lsp/rpc.c's rpc_write_message frames a message onto a FILE*, then
 * blocks reading commands back in a loop -- exactly where the ordinary
 * path calls getchar() -- applying any `setBreakpoints` in place
 * (letting a user edit breakpoints *while stopped*, the more common
 * real workflow, before resuming) and only returning once a `continue`
 * arrives. vm->frames (walked the same way raise_capture_backtrace_
 * helper already does) gives the full call stack with zero new state;
 * only the innermost, paused frame gets locals attached (see docs/
 * debugging.md's own "outer frames have no locals" note -- no per-chunk
 * local-debug table exists for any frame but the exact call site's own
 * baked-in operand list this opcode already carries). A write or read
 * failure here (the DAP client vanished, the pipe broke) ends the loop
 * the same as a clean "continue": best-effort, not a reason to fail the
 * debuggee's own execution over a detached debugger. */
static DiamondVmStatus debugger_structured_helper(DiamondVm *vm,
        const DiamondChunk *chunk,size_t depth,size_t instruction_offset,
        size_t source_line_offset,
        DiamondValue *registers,const uint16_t *name_indices,
        const uint16_t *local_registers,uint8_t local_count,const char *reason) {
    GrowBuffer body={};
    bool ok=GROW_BUFFER_APPEND_LITERAL(&body,"{\"event\":\"paused\",\"reason\":");
    if(ok)ok=debug_json_append_escaped_string(&body,reason,strlen(reason));
    if(ok)ok=GROW_BUFFER_APPEND_LITERAL(&body,",\"stack\":[");
    size_t frame_index=0;
    for(const DiamondFrame *frame=vm->frames;ok&&frame!=nullptr;
            frame=frame->previous,frame_index++) {
        if(frame_index>0&&!(ok=GROW_BUFFER_APPEND_LITERAL(&body,",")))break;
        if(frame->chunk==nullptr||frame->instruction_offset==nullptr)continue;
        const char *frame_name=frame->chunk->name!=nullptr?frame->chunk->name:"<chunk>";
        const size_t offset=*frame->instruction_offset;
        const bool in_bounds=offset<frame->chunk->code_count;
        const uint32_t frame_line=in_bounds&&frame->chunk->lines!=nullptr?
            frame->chunk->lines[offset]:0;
        const uint32_t frame_column=in_bounds&&frame->chunk->columns!=nullptr?
            frame->chunk->columns[offset]:0;
        ok=GROW_BUFFER_APPEND_LITERAL(&body,"{\"name\":");
        if(ok)ok=debug_json_append_escaped_string(&body,frame_name,strlen(frame_name));
        char numbers[96];
        const int written=frame_index==0&&source_line_offset!=SIZE_MAX?
            snprintf(numbers,sizeof numbers,
                ",\"line\":%u,\"column\":%u,\"sourceOffset\":%zu}",
                frame_line,frame_column,source_line_offset):
            snprintf(numbers,sizeof numbers,
                ",\"line\":%u,\"column\":%u}",frame_line,frame_column);
        if(ok&&written>0)ok=grow_buffer_append(&body,numbers,(size_t)written);
        else if(written<0)ok=false;
    }
    /* The calls the statement about to run makes, in order, for DAP's
     * stepInTargets: `{"name","line","column"}` each. */
    if(ok)ok=GROW_BUFFER_APPEND_LITERAL(&body,"],\"targets\":[");
    {
        DiamondCallSite sites[DIAMOND_MAX_STEP_IN_TARGETS];
        const size_t site_count=diamond_collect_call_sites(chunk,instruction_offset,sites,
            DIAMOND_MAX_STEP_IN_TARGETS);
        for(size_t index=0;ok&&index<site_count;index++) {
            if(index>0&&!(ok=GROW_BUFFER_APPEND_LITERAL(&body,",")))break;
            ok=GROW_BUFFER_APPEND_LITERAL(&body,"{\"name\":");
            if(ok)ok=debug_json_append_escaped_string(&body,sites[index].name,
                strlen(sites[index].name));
            char position[64];
            const int written=snprintf(position,sizeof position,
                ",\"line\":%u,\"column\":%u}",sites[index].line,sites[index].column);
            if(ok&&written>0)ok=grow_buffer_append(&body,position,(size_t)written);
            else if(written<0)ok=false;
        }
    }
    if(ok)ok=GROW_BUFFER_APPEND_LITERAL(&body,"],\"locals\":[");
    size_t locals_emitted=0;
    for(size_t index=0;ok&&index<local_count;index++) {
        if((size_t)name_indices[index]>=chunk->string_count)continue;
        if(locals_emitted>0&&!(ok=GROW_BUFFER_APPEND_LITERAL(&body,",")))break;
        const DiamondStringConstant *name=&chunk->strings[name_indices[index]];
        DiamondValue value=registers[local_registers[index]];
        if(value.kind==DIAMOND_VALUE_OBJECT&&
           value.as.object->kind==DIAMOND_OBJECT_CELL)
            value=((DiamondCell *)value.as.object)->value;
        DiamondValue stringified=DIAMOND_NIL;
        const DiamondVmStatus status=stringify_value(vm,chunk,depth,value,&stringified);
        if(status!=DIAMOND_VM_OK) { free(body.data);return status; }
        const DiamondString *text=(const DiamondString *)stringified.as.object;
        ok=GROW_BUFFER_APPEND_LITERAL(&body,"{\"name\":");
        if(ok)ok=debug_json_append_escaped_string(&body,name->chars,name->length);
        if(ok)ok=GROW_BUFFER_APPEND_LITERAL(&body,",\"value\":");
        if(ok)ok=debug_json_append_escaped_string(&body,text->chars,text->length);
        if(ok)ok=GROW_BUFFER_APPEND_LITERAL(&body,"}");
        locals_emitted++;
    }
    if(ok)ok=GROW_BUFFER_APPEND_LITERAL(&body,"]}");
    if(!ok) { free(body.data);return DIAMOND_VM_OUT_OF_MEMORY; }
    char header[64];
    const int header_length=snprintf(header,sizeof header,
        "Content-Length: %zu\r\n\r\n",body.length);
    if(header_length>0&&debug_pipe_write_all(vm->debug_fd,header,(size_t)header_length))
        debug_pipe_write_all(vm->debug_fd,body.data,body.length);
    free(body.data);
    while(true) {
        DiamondDebugCommandKind kind=DIAMOND_DEBUG_COMMAND_NONE;
        size_t lines[DIAMOND_MAX_ACTIVE_BREAKPOINTS];size_t line_count=0;
        bool are_offsets=false;
        char step_target[64];
        if(!debug_pipe_read_command(vm->debug_fd,&kind,lines,&line_count,
                DIAMOND_MAX_ACTIVE_BREAKPOINTS,&are_offsets,step_target))
            break;
        if(kind==DIAMOND_DEBUG_COMMAND_SET_BREAKPOINTS) {
            memcpy(vm->debug_active_lines,lines,line_count*sizeof lines[0]);
            vm->debug_active_line_count=line_count;
            vm->debug_breakpoints_are_offsets=are_offsets;
            continue;
        }
        /* next/stepIn/stepOut (docs/debugging.md's own "Stepping"
         * section): arm the target depth from `depth`, this exact
         * pause's own already-known run_chunk recursion depth -- no
         * argument needs to travel from dap/main.c at all -- then resume
         * the same way `continue` does. DIAMOND_OP_BREAKPOINT_CHECK's own
         * case in vm_run_chunk.c is what actually consumes this
         * on the next qualifying checkpoint hit. */
        vm->debug_step_in_target[0]='\0';
        if(kind==DIAMOND_DEBUG_COMMAND_NEXT)vm->debug_step_mode=DIAMOND_STEP_OVER;
        else if(kind==DIAMOND_DEBUG_COMMAND_STEP_IN) {
            vm->debug_step_mode=DIAMOND_STEP_IN;
            memcpy(vm->debug_step_in_target,step_target,sizeof vm->debug_step_in_target);
        }
        else if(kind==DIAMOND_DEBUG_COMMAND_STEP_OUT)vm->debug_step_mode=DIAMOND_STEP_OUT;
        if(vm->debug_step_mode!=DIAMOND_STEP_NONE)vm->debug_step_target_depth=depth;
        break;
    }
    return DIAMOND_VM_OK;
}

/* The shared runtime pause helper for both DIAMOND_OP_DEBUGGER (an
 * explicit debugger()/breakpoint() call, always unconditional -- see
 * parse_debugger_call's own comment in compiler_builtins.c for the compile-time
 * half (name, register) pairs come from) and an armed
 * DIAMOND_OP_BREAKPOINT_CHECK. `reason` ("breakpoint" or "step") is
 * purely for the DAP-facing structured payload below -- see its own
 * comment. Under DIAMOND_DEBUG_FD (see DiamondVm.debug_fd's own
 * comment, src/vm.h), delegates to debugger_structured_helper's DAP-
 * facing pause instead of this ordinary path. Otherwise prints
 * "chunk:line:column" matching the exact format RECORD_ERROR/
 * raise_capture_backtrace_helper already use in vm.c,
 * then each local's name and stringified value (unwrapping a
 * captured local's Cell box first -- BOX_LOCAL replaces a captured
 * local's own register contents with a DiamondCell wrapper in place, so
 * this checks the *runtime* value kind rather than trusting any
 * compile-time "captured" bookkeeping passed through), then blocks on
 * one line of stdin -- `reason` is unused on this plain path, which has
 * no stepping concept at all (a DAP control channel is what stepping
 * commands travel over, and this path only ever runs without one). Kept
 * as its own helper (not inlined into DIAMOND_OP_DEBUGGER's own case
 * block) both for this file's usual stack-frame-budget reasons and
 * because it may recurse into run_chunk itself once per local, through
 * stringify_value calling a user-defined to_s. */
DiamondVmStatus debugger_helper(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,size_t instruction_offset,size_t source_line_offset,DiamondValue *registers,
        const uint16_t *name_indices,const uint16_t *local_registers,uint8_t local_count,
        const char *reason) {
    if(vm->debug_fd>=0)
        return debugger_structured_helper(vm,chunk,depth,instruction_offset,source_line_offset,
            registers,name_indices,local_registers,local_count,reason);
    const char *frame_name=chunk->name!=nullptr?chunk->name:"<chunk>";
    const bool in_bounds=instruction_offset<chunk->code_count;
    const uint32_t line=in_bounds&&chunk->lines!=nullptr?
        chunk->lines[instruction_offset]:0;
    const uint32_t column=in_bounds&&chunk->columns!=nullptr?
        chunk->columns[instruction_offset]:0;
    fprintf(stdout,"--- paused at %s:%u:%u ---\n",frame_name,line,column);
    if(local_count>0) {
        fprintf(stdout,"locals:\n");
        for(size_t index=0;index<local_count;index++) {
            if((size_t)name_indices[index]>=chunk->string_count)continue;
            const DiamondStringConstant *name=&chunk->strings[name_indices[index]];
            DiamondValue value=registers[local_registers[index]];
            if(value.kind==DIAMOND_VALUE_OBJECT&&
               value.as.object->kind==DIAMOND_OBJECT_CELL)
                value=((DiamondCell *)value.as.object)->value;
            DiamondValue stringified=DIAMOND_NIL;
            const DiamondVmStatus status=
                stringify_value(vm,chunk,depth,value,&stringified);
            if(status!=DIAMOND_VM_OK)return status;
            const DiamondString *text=(const DiamondString *)stringified.as.object;
            fprintf(stdout,"  %.*s = %.*s\n",(int)name->length,name->chars,
                (int)text->length,text->chars);
        }
    }
    fprintf(stdout,"(press Enter to continue)\n");
    fflush(stdout);
    StringBuilder input={};
    bool saw_any=false;
    const DiamondVmStatus status=read_line(vm,stdin,&input,&saw_any);
    free(input.chars);
    return status;
}
