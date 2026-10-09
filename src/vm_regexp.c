#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
/* _GNU_SOURCE (a superset of _DEFAULT_SOURCE): only for pthread_getattr_np,
 * used to learn a thread's own native stack bounds for the ASan
 * fiber-switch annotations below. */
#define _GNU_SOURCE

#include "vm.h"
#include "jit.h"
#include "bignum.h"
#include "compiler.h"
#include "disassemble.h"
#include "loader.h"
#include "prelude.h"
#include "vm_internal.h"

/* <crypt.h> exists on glibc (libxcrypt) and musl (see BCrypt.hash's own
 * comment below for what musl's version lacks), declaring crypt_r/
 * struct crypt_data/CRYPT_GENSALT_* -- but not on FreeBSD, which declares
 * plain crypt()/crypt_r() directly in <unistd.h> (already included below)
 * instead, with no separate header at all. __has_include, not an
 * __APPLE__/__FreeBSD__-style OS check (see docs/portability.md's own
 * "What hasn't been found" on why this codebase avoids those): this is a
 * feature test, and the same reasoning applies wherever else a libc omits
 * this header. */
#if __has_include(<crypt.h>)
#include <crypt.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include <math.h>
#include <stdckdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <spawn.h>
#include <sqlite3.h>
#include <libpq-fe.h>
#include <mysql.h>
#include <zlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vm_internal.h"

DiamondRegexp *allocate_regexp_handle(DiamondVm *vm,reginold_regex *compiled,
        const char *source,size_t source_length,unsigned int options) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondRegexp *regexp=malloc(sizeof(DiamondRegexp));
    if(regexp==nullptr)return nullptr;
    char *source_copy=malloc(source_length+1);
    if(source_copy==nullptr) {free(regexp);return nullptr;}
    memcpy(source_copy,source,source_length);source_copy[source_length]='\0';
    *regexp=(DiamondRegexp){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_REGEXP},
        .handle=compiled,.source=source_copy,.source_length=source_length,.options=options};
    vm->young_objects=&regexp->object;vm->bytes_allocated+=sizeof(DiamondRegexp);return regexp;
}

/* DIAMOND_OP_REGEXP_NEW's real body, factored out of run_chunk's own
 * switch statement deliberately, not just for readability: every local
 * variable declared anywhere in that switch contributes to run_chunk's
 * one stack frame regardless of which case actually runs (a -O0 build,
 * which this project's debug/sanitize builds both are, does not reuse
 * stack slots across sibling blocks), and DIAMOND_MAX_CALL_DEPTH is
 * calibrated against that frame's worst-case size to stay safe under
 * AddressSanitizer's redzone-inflated frames (see docs/design.md). A
 * reginold_error alone is ~112 bytes (its message buffer is
 * REGINOLD_ERROR_MSG_MAX=90); adding it and reginold_match's fields
 * directly into run_chunk's frame regressed depth(5000)-style recursion
 * into a genuine ASan stack-overflow crash before the depth counter ever
 * tripped -- caught by make test-sanitize, not by hand-testing, since the
 * regex feature itself worked perfectly right up until deep recursion
 * was exercised. Splitting this into its own function moves those locals
 * into a separate, transient frame that only exists while regex code is
 * actually running, not on every recursive run_chunk level. */
DiamondVmStatus regexp_new_helper(DiamondVm *vm, const DiamondString *pattern,
        int64_t options, DiamondValue *result) {
    reginold_regex *compiled=nullptr;
    reginold_error compile_error={0};
    const reginold_status compile_status=reginold_compile(pattern->chars,
        pattern->length,(unsigned int)options,&compiled,&compile_error);
    if(compile_status!=REGINOLD_OK) {
        snprintf(vm->error,sizeof vm->error,"%.*s",
            (int)compile_error.message_len,compile_error.message);
        return DIAMOND_VM_REGEXP_ERROR;
    }
    DiamondRegexp *regexp=allocate_regexp_handle(vm,compiled,pattern->chars,pattern->length,(unsigned int)options);
    if(regexp==nullptr) {
        reginold_regex_free(compiled);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *result=DIAMOND_OBJECT(regexp);
    return DIAMOND_VM_OK;
}

/* Root through registers[dest] directly, not an out-param -- same real
 * bug, and same fix, as regexp_scan_helper above (see that function's
 * own comment for the full story: *result pointed into the caller's C
 * stack, never a real GC root, and the `groups` malloc'd buffer this
 * used to build had zero GC visibility of its own between one capture
 * group's String allocation and the next). */
DiamondVmStatus regexp_match_helper(DiamondVm *vm, const DiamondRegexp *regexp,
        const DiamondString *subject, bool test_only,
        DiamondValue *registers, uint16_t dest) {
    if(test_only) {
        const reginold_status search_status=reginold_search(regexp->handle,
            subject->chars,subject->length,0,nullptr);
        if(search_status==REGINOLD_ERROR) {
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            return DIAMOND_VM_REGEXP_ERROR;
        }
        registers[dest]=DIAMOND_BOOL(search_status==REGINOLD_OK);
        return DIAMOND_VM_OK;
    }
    reginold_match match_result={0};
    const reginold_status search_status=reginold_search(regexp->handle,
        subject->chars,subject->length,0,&match_result);
    if(search_status==REGINOLD_ERROR) {
        snprintf(vm->error,sizeof vm->error,"regexp match failed");
        return DIAMOND_VM_REGEXP_ERROR;
    }
    if(search_status==REGINOLD_MISMATCH) {
        registers[dest]=DIAMOND_NIL;
        return DIAMOND_VM_OK;
    }
    DiamondArray *result_array=allocate_array(vm,nullptr,0);
    if(result_array==nullptr) {
        reginold_match_free(&match_result);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    registers[dest]=DIAMOND_OBJECT(result_array);
    const size_t group_count=1+match_result.capture_count;
    for(size_t index=0;index<group_count;index++) {
        const reginold_span span=index==0?match_result.overall:
            match_result.captures[index-1];
        DiamondValue group_value=DIAMOND_NIL;
        if(span.beg>=0&&span.end>=0) {
            DiamondString *group_string=allocate_string(vm,
                subject->chars+span.beg,(size_t)(span.end-span.beg));
            if(group_string==nullptr) {
                reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
            }
            group_value=DIAMOND_OBJECT(group_string);
        }
        if(!array_push(vm,result_array,group_value)) {
            reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    reginold_match_free(&match_result);
    return DIAMOND_VM_OK;
}

/* Expands a String#sub/String#gsub replacement into `out`, honoring Ruby's
 * backslash escapes: `\0`/`\&` is the whole match, `\1`-`\9` is that capture
 * group (empty if the group didn't participate, e.g. an unmatched `(x)?`),
 * `\\` is a literal backslash, and a backslash before anything else (or a
 * group number past the pattern's actual capture count) is dropped and the
 * following byte copied as-is -- no named (`\k<name>`) backreferences,
 * a scope cut nothing here exercises. */
static bool regexp_append_replacement(ByteBuffer *out,const DiamondString *subject,
        const DiamondString *replacement,const reginold_match *match) {
    size_t index=0;
    bool ok=true;
    while(ok&&index<replacement->length) {
        const char ch=replacement->chars[index];
        if(ch=='\\'&&index+1<replacement->length) {
            const char next=replacement->chars[index+1];
            if(next=='\\') {
                ok=byte_buffer_append(out,"\\",1);index+=2;continue;
            }
            if(next=='&'||next=='0') {
                const size_t begin=(size_t)match->overall.beg;
                const size_t end=(size_t)match->overall.end;
                ok=byte_buffer_append(out,subject->chars+begin,end-begin);
                index+=2;continue;
            }
            if(next>='1'&&next<='9') {
                const size_t group=(size_t)(next-'0');
                if(group<=match->capture_count) {
                    const reginold_span span=match->captures[group-1];
                    if(span.beg>=0&&span.end>=0)
                        ok=byte_buffer_append(out,subject->chars+span.beg,
                            (size_t)(span.end-span.beg));
                }
                index+=2;continue;
            }
            index++;continue;
        }
        ok=byte_buffer_append(out,&ch,1);index++;
    }
    return ok;
}

/* String#sub/String#gsub's shared body (replace_all toggles first-only vs
 * every match). Zero-length matches (a pattern that can match an empty
 * string, e.g. an empty pattern or "x zero-or-more-times") copy one
 * source byte forward after
 * inserting the replacement, the same way Ruby's own gsub avoids looping
 * forever on one -- without that, `search_status` would report the exact
 * same empty match at the exact same offset indefinitely. */
DiamondVmStatus regexp_replace_helper(DiamondVm *vm,const DiamondRegexp *regexp,
        const DiamondString *subject,const DiamondString *replacement,
        bool replace_all,DiamondValue *result) {
    ByteBuffer output={0};
    size_t cursor=0;
    bool ok=true;
    while(ok&&cursor<=subject->length) {
        reginold_match match_result={0};
        const reginold_status search_status=reginold_search(regexp->handle,
            subject->chars,subject->length,cursor,&match_result);
        if(search_status==REGINOLD_ERROR) {
            free(output.data);
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            return DIAMOND_VM_REGEXP_ERROR;
        }
        if(search_status==REGINOLD_MISMATCH)break;
        const size_t match_begin=(size_t)match_result.overall.beg;
        const size_t match_end=(size_t)match_result.overall.end;
        ok=byte_buffer_append(&output,subject->chars+cursor,match_begin-cursor)&&
           regexp_append_replacement(&output,subject,replacement,&match_result);
        reginold_match_free(&match_result);
        if(match_end==match_begin) {
            if(match_end<subject->length)
                ok=ok&&byte_buffer_append(&output,subject->chars+match_end,1);
            cursor=match_end+1;
        } else {
            cursor=match_end;
        }
        if(!replace_all)break;
    }
    if(ok&&cursor<subject->length)
        ok=byte_buffer_append(&output,subject->chars+cursor,subject->length-cursor);
    if(!ok) {free(output.data);return DIAMOND_VM_OUT_OF_MEMORY;}
    DiamondString *replaced=allocate_string(vm,
        output.data!=nullptr?output.data:"",output.length);
    free(output.data);
    if(replaced==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *result=DIAMOND_OBJECT(replaced);
    return DIAMOND_VM_OK;
}

/* String#sub/#gsub with a block: each match's text is passed to `block`,
 * and what it returns (converted with to_s) replaces the match, as in
 * Ruby. `subject`, `regexp`, and `block` are rooted by the caller's
 * registers; each match String is protected while the block runs. */
DiamondVmStatus regexp_replace_block_helper(DiamondVm *vm,
        const DiamondChunk *chunk,size_t depth,const DiamondRegexp *regexp,
        const DiamondString *subject,const DiamondClosure *block,
        bool replace_all,DiamondValue *result) {
    if(block->foreign_chunk!=nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "a compile_method callable can only be passed to define_method");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(block->function_index>=chunk->function_count)return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondFunction *fn=chunk->functions[block->function_index];
    ByteBuffer output={0};
    size_t cursor=0;
    DiamondVmStatus status=DIAMOND_VM_OK;
    while(cursor<=subject->length) {
        reginold_match match_result={0};
        const reginold_status search_status=reginold_search(regexp->handle,
            subject->chars,subject->length,cursor,&match_result);
        if(search_status==REGINOLD_ERROR) {
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            status=DIAMOND_VM_REGEXP_ERROR;break;
        }
        if(search_status==REGINOLD_MISMATCH)break;
        const size_t match_begin=(size_t)match_result.overall.beg;
        const size_t match_end=(size_t)match_result.overall.end;
        reginold_match_free(&match_result);
        if(!byte_buffer_append(&output,subject->chars+cursor,match_begin-cursor)) {
            status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
        const size_t protect_mark=vm->gc_protected_count;
        DiamondString *found=allocate_string(vm,subject->chars+match_begin,
            match_end-match_begin);
        if(found==nullptr||!gc_protect(vm,DIAMOND_OBJECT(found))) {
            gc_unprotect(vm,protect_mark);status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
        DiamondValue argument[1]={DIAMOND_OBJECT(found)};
        DiamondValue replacement=DIAMOND_NIL;
        status=call_closure_helper(vm,chunk,fn,block,argument,0,1,depth,&replacement);
        if(status==DIAMOND_VM_OK&&(replacement.kind!=DIAMOND_VALUE_OBJECT||
           replacement.as.object->kind!=DIAMOND_OBJECT_STRING)) {
            if(!gc_protect(vm,replacement))status=DIAMOND_VM_OUT_OF_MEMORY;
            else status=stringify_value(vm,chunk,depth,replacement,&replacement);
        }
        gc_unprotect(vm,protect_mark);
        if(status!=DIAMOND_VM_OK)break;
        const DiamondString *text=(const DiamondString *)replacement.as.object;
        if(!byte_buffer_append(&output,text->chars,text->length)) {
            status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
        if(match_end==match_begin) {
            if(match_end<subject->length&&
               !byte_buffer_append(&output,subject->chars+match_end,1)) {
                status=DIAMOND_VM_OUT_OF_MEMORY;break;
            }
            cursor=match_end+1;
        } else {
            cursor=match_end;
        }
        if(!replace_all)break;
    }
    if(status==DIAMOND_VM_OK&&cursor<subject->length&&
       !byte_buffer_append(&output,subject->chars+cursor,subject->length-cursor))
        status=DIAMOND_VM_OUT_OF_MEMORY;
    if(status!=DIAMOND_VM_OK) {free(output.data);return status;}
    DiamondString *replaced=allocate_string(vm,
        output.data!=nullptr?output.data:"",output.length);
    free(output.data);
    if(replaced==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *result=DIAMOND_OBJECT(replaced);
    return DIAMOND_VM_OK;
}

/* Root through registers[dest] directly, not an out-param -- a real bug
 * found while investigating an unrelated crash (a self-referential-
 * looking array from String#scan, reproduced with DIAMOND_STRESS_GC=1
 * even on code well before this session's own changes). The previous
 * shape wrote the result array into *result, a plain DiamondValue
 * sitting in the *caller's* C stack frame -- never a real GC root, so
 * every allocation after the first (each whole-match/capture-group
 * String, each per-match capture Array) risked a GC pass collecting the
 * result array, or an already-built capture Array, out from under this
 * function while it was still building it. The inner capture-group loop
 * had the same bug twice over: `groups`, a bare malloc'd C array, held
 * DiamondValues with zero GC visibility at all between allocating one
 * group String and the next. Fixed by rooting `matches` immediately via
 * the caller's own dest register (the same register the caller was
 * always going to assign it to anyway, just done at the start instead
 * of the end) and pushing each capture Array into it -- and each group
 * String into that capture Array -- the instant it exists, so nothing
 * is ever unreachable between one allocation and the next. */
DiamondVmStatus regexp_scan_helper(DiamondVm *vm,const DiamondRegexp *regexp,
        const DiamondString *subject,DiamondValue *registers,uint16_t dest) {
    DiamondArray *matches=allocate_array(vm,nullptr,0);
    if(matches==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(matches);
    size_t cursor=0;
    while(cursor<=subject->length) {
        reginold_match match_result={0};
        const reginold_status search_status=reginold_search(regexp->handle,
            subject->chars,subject->length,cursor,&match_result);
        if(search_status==REGINOLD_ERROR) {
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            return DIAMOND_VM_REGEXP_ERROR;
        }
        if(search_status==REGINOLD_MISMATCH)break;
        const size_t match_begin=(size_t)match_result.overall.beg;
        const size_t match_end=(size_t)match_result.overall.end;
        if(match_result.capture_count==0) {
            DiamondString *whole=allocate_string(vm,
                subject->chars+match_begin,match_end-match_begin);
            if(whole==nullptr) {reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;}
            if(!array_push(vm,matches,DIAMOND_OBJECT(whole))) {
                reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
            }
        } else {
            DiamondArray *group_array=allocate_array(vm,nullptr,0);
            if(group_array==nullptr) {reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;}
            /* Pushed into the already-rooted `matches` before it has any
             * elements of its own, so it (and everything pushed into it
             * below) stays reachable transitively through matches for
             * the rest of this match's construction. */
            if(!array_push(vm,matches,DIAMOND_OBJECT(group_array))) {
                reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
            }
            for(size_t index=0;index<match_result.capture_count;index++) {
                const reginold_span span=match_result.captures[index];
                DiamondValue group_value=DIAMOND_NIL;
                if(span.beg>=0&&span.end>=0) {
                    DiamondString *group_string=allocate_string(vm,
                        subject->chars+span.beg,(size_t)(span.end-span.beg));
                    if(group_string==nullptr) {
                        reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
                    }
                    group_value=DIAMOND_OBJECT(group_string);
                }
                if(!array_push(vm,group_array,group_value)) {
                    reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
                }
            }
        }
        reginold_match_free(&match_result);
        cursor=match_end==match_begin?match_end+1:match_end;
    }
    return DIAMOND_VM_OK;
}

/* String#split(Regexp) -- the counterpart to regexp_scan_helper just
 * above: same reginold_search loop, but collects the text *between*
 * matches instead of the matches themselves, then pushes whatever's
 * left after the last match (or the whole subject, if there was no
 * match at all) as the final piece. A zero-width match (an empty-
 * string-matching pattern) is skipped rather than splitting on it --
 * matching regexp_scan_helper's own zero-width handling (advance past
 * it by one byte rather than looping forever), not an attempt at
 * Ruby's own more elaborate zero-width-match split semantics, which
 * this codebase's one real caller (skindicate.dia's Winamp ingester,
 * splitting on `\band\b`) never needs. */
DiamondVmStatus regexp_split_helper(DiamondVm *vm,const DiamondRegexp *regexp,
        const DiamondString *subject,DiamondValue *registers,uint16_t dest) {
    DiamondArray *pieces=allocate_array(vm,nullptr,0);
    if(pieces==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(pieces);
    size_t piece_start=0;
    size_t cursor=0;
    while(cursor<=subject->length) {
        reginold_match match_result={0};
        const reginold_status search_status=reginold_search(regexp->handle,
            subject->chars,subject->length,cursor,&match_result);
        if(search_status==REGINOLD_ERROR) {
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            return DIAMOND_VM_REGEXP_ERROR;
        }
        if(search_status==REGINOLD_MISMATCH)break;
        const size_t match_begin=(size_t)match_result.overall.beg;
        const size_t match_end=(size_t)match_result.overall.end;
        reginold_match_free(&match_result);
        if(match_end==match_begin) {
            cursor=match_end+1;
            continue;
        }
        DiamondString *piece=allocate_string(vm,
            subject->chars+piece_start,match_begin-piece_start);
        if(piece==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
            return DIAMOND_VM_OUT_OF_MEMORY;
        piece_start=match_end;
        cursor=match_end;
    }
    DiamondString *tail=allocate_string(vm,
        subject->chars+piece_start,subject->length-piece_start);
    if(tail==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!array_push(vm,pieces,DIAMOND_OBJECT(tail)))
        return DIAMOND_VM_OUT_OF_MEMORY;
    return DIAMOND_VM_OK;
}
