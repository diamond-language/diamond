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

enum { DIAMOND_PROCESS_MAX_ARGV = 65536 };

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

/* File.join(*parts) -- joins every String in `parts` with '/', collapsing
 * a redundant separator at each seam (a trailing '/' on the left side,
 * a leading '/' on the right side, or both) into exactly one, and
 * skipping empty-string parts entirely. Deliberately not bug-for-bug
 * identical to Ruby's own File.join (which treats a leading "" part as
 * contributing a separator) -- this package has no other File.join
 * caller to match, and "skip empty parts" is the more predictable rule
 * for path-building code that assembles parts conditionally. `parts`
 * are `count` contiguous registers starting at `base`, the same shape
 * DIAMOND_OP_THREAD_NEW's own variadic argument list already uses. */
DiamondVmStatus file_path_join_helper(DiamondVm *vm,const DiamondValue *parts,
        size_t count,DiamondValue *out) {
    StringBuilder builder={};
    for(size_t index=0;index<count;index++) {
        const DiamondValue value=parts[index];
        if(value.kind!=DIAMOND_VALUE_OBJECT||value.as.object->kind!=DIAMOND_OBJECT_STRING) {
            free(builder.chars);
            snprintf(vm->error,sizeof vm->error,"File.join arguments must be Strings");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *part=(const DiamondString *)value.as.object;
        if(part->length==0)continue;
        size_t start=0;
        bool ok=true;
        if(builder.length>0) {
            while(start<part->length&&part->chars[start]=='/')start++;
            while(builder.length>0&&builder.chars[builder.length-1]=='/')builder.length--;
            ok=builder_append(&builder,"/",1);
        }
        if(ok&&start<part->length)
            ok=builder_append(&builder,part->chars+start,part->length-start);
        if(!ok) {free(builder.chars);return DIAMOND_VM_OUT_OF_MEMORY;}
    }
    DiamondString *joined=allocate_string(vm,builder.chars?builder.chars:"",builder.length);
    free(builder.chars);
    if(joined==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(joined);
    return DIAMOND_VM_OK;
}

/* Recovery barrier for an already published file and its directory entry. */
DiamondVmStatus file_sync_helper(DiamondVm *vm,const DiamondString *path) {
    if(path->length==0||memchr(path->chars,'\0',path->length)!=nullptr) {
        snprintf(vm->error,sizeof vm->error,"File.sync requires a nonempty path without NUL");
        return DIAMOND_VM_TYPE_ERROR;
    }
    char *parent=strdup(path->chars);
    if(parent==nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
    char *slash=strrchr(parent,'/');
    if(slash==nullptr) strcpy(parent,".");
    else if(slash==parent) slash[1]='\0';
    else *slash='\0';
    int fd=open(path->chars,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    int saved=fd<0?errno:0;
    struct stat status;
    if(saved==0) {
        if(fstat(fd,&status)!=0) saved=errno;
        else if(!S_ISREG(status.st_mode)) saved=EINVAL;
        else if(fsync(fd)!=0) saved=errno;
    }
    if(fd>=0) close(fd);
    if(saved==0) {
        int directory=open(parent,O_RDONLY|O_DIRECTORY);
        if(directory<0) saved=errno;
        else {
            if(fsync(directory)!=0) saved=errno;
            close(directory);
        }
    }
    free(parent);
    if(saved!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot sync '%.*s': %s",
                 (int)path->length,path->chars,strerror(saved));
        return DIAMOND_VM_IO_ERROR;
    }
    return DIAMOND_VM_OK;
}

/* File.rename(from, to): rename(2), so an existing `to` is replaced
 * atomically -- a reader sees the old file or the new one, never a
 * partial write. Both paths must be on the same filesystem. */
DiamondVmStatus file_read_helper(DiamondVm *vm,const DiamondString *path,
                                        DiamondValue *result) {
    if(memchr(path->chars,'\0',path->length)!=nullptr) {
        snprintf(vm->error,sizeof vm->error,"File.read path must not contain NUL");
        return DIAMOND_VM_TYPE_ERROR;
    }
    FILE *file=fopen(path->chars,"rb");
    if(file==nullptr) {
        snprintf(vm->error,sizeof vm->error,"cannot open '%.*s': %s",
            (int)path->length,path->chars,strerror(errno));
        return DIAMOND_VM_IO_ERROR;
    }
    ByteBuffer contents={0};
    char chunk[65536];
    bool ok=true;
    size_t got=0;
    while(ok&&(got=fread(chunk,1,sizeof chunk,file))>0)
        ok=byte_buffer_append(&contents,chunk,got);
    const bool read_failed=ferror(file)!=0;
    const int saved_errno=errno;
    fclose(file);
    if(!ok) {free(contents.data);return DIAMOND_VM_OUT_OF_MEMORY;}
    if(read_failed) {
        free(contents.data);
        snprintf(vm->error,sizeof vm->error,"cannot read '%.*s': %s",
            (int)path->length,path->chars,strerror(saved_errno));
        return DIAMOND_VM_IO_ERROR;
    }
    DiamondString *string=allocate_string(vm,
        contents.data!=nullptr?contents.data:"",contents.length);
    free(contents.data);
    if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *result=DIAMOND_OBJECT(string);
    return DIAMOND_VM_OK;
}

DiamondVmStatus file_write_helper(DiamondVm *vm,const DiamondString *path,
                                         const DiamondString *data,DiamondValue *result) {
    if(data==nullptr||memchr(path->chars,'\0',path->length)!=nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "File.write requires a path without NUL and String data");
        return DIAMOND_VM_TYPE_ERROR;
    }
    FILE *file=fopen(path->chars,"wb");
    if(file==nullptr) {
        snprintf(vm->error,sizeof vm->error,"cannot open '%.*s': %s",
            (int)path->length,path->chars,strerror(errno));
        return DIAMOND_VM_IO_ERROR;
    }
    const size_t written=fwrite(data->chars,1,data->length,file);
    const int saved_errno=errno;
    if(fclose(file)!=0||written!=data->length) {
        snprintf(vm->error,sizeof vm->error,"cannot write '%.*s': %s",
            (int)path->length,path->chars,strerror(saved_errno));
        return DIAMOND_VM_IO_ERROR;
    }
    *result=DIAMOND_INT((int64_t)written);
    return DIAMOND_VM_OK;
}

DiamondVmStatus file_rename_helper(DiamondVm *vm,const DiamondString *from,
                                         const DiamondString *to) {
    if(to==nullptr||from->length==0||to->length==0||
       memchr(from->chars,'\0',from->length)!=nullptr||
       memchr(to->chars,'\0',to->length)!=nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "File.rename requires two nonempty paths without NUL");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(rename(from->chars,to->chars)!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot rename '%.*s' to '%.*s': %s",
            (int)from->length,from->chars,(int)to->length,to->chars,strerror(errno));
        return DIAMOND_VM_IO_ERROR;
    }
    return DIAMOND_VM_OK;
}

DiamondVmStatus file_publish_helper(DiamondVm *vm,const DiamondString *path,
                                          const DiamondString *bytes) {
    if(bytes==nullptr||path->length==0||
       memchr(path->chars,'\0',path->length)!=nullptr) {
        snprintf(vm->error,sizeof vm->error,"File.publish requires a nonempty path without NUL and String bytes");
        return DIAMOND_VM_TYPE_ERROR;
    }
    char *parent=strdup(path->chars);
    if(parent==nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
    char *slash=strrchr(parent,'/');
    if(slash==nullptr) strcpy(parent,".");
    else if(slash==parent) slash[1]='\0';
    else *slash='\0';
    size_t capacity=strlen(parent)+32;
    char *temporary=malloc(capacity);
    if(temporary==nullptr) { free(parent); return DIAMOND_VM_OUT_OF_MEMORY; }
    snprintf(temporary,capacity,"%s/.diamond-publish-XXXXXX",parent);
    int directory=open(parent,O_RDONLY|O_DIRECTORY);
    int saved=directory<0?errno:0;
    int fd=-1;
    bool created=false;
    if(saved==0) {
        fd=mkstemp(temporary);
        if(fd<0) saved=errno;
        else created=true;
    }
    size_t offset=0;
    while(saved==0&&offset<bytes->length) {
        size_t remaining=bytes->length-offset;
        if(remaining>(size_t)SSIZE_MAX) remaining=(size_t)SSIZE_MAX;
        ssize_t written=write(fd,bytes->chars+offset,remaining);
        if(written<0&&errno==EINTR) continue;
        if(written<=0) { saved=written<0?errno:EIO; break; }
        offset+=(size_t)written;
    }
    if(saved==0&&fsync(fd)!=0) saved=errno;
    if(fd>=0&&close(fd)!=0&&saved==0) saved=errno;
    if(saved==0&&link(temporary,path->chars)!=0) saved=errno;
    if(created&&unlink(temporary)!=0&&saved==0) saved=errno;
    if(saved==0&&fsync(directory)!=0) saved=errno;
    if(directory>=0) close(directory);
    free(temporary);
    free(parent);
    if(saved!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot publish '%.*s': %s",
                 (int)path->length,path->chars,strerror(saved));
        return DIAMOND_VM_IO_ERROR;
    }
    return DIAMOND_VM_OK;
}

/* File.dirname(path) -- everything before the last real path separator,
 * matching Ruby: no separator at all -> ".", a single leading separator
 * -> "/" (never truncated away, so the root stays meaningful), trailing
 * separators on the input ignored first. Pure string manipulation, no
 * filesystem access -- `path` need not exist. */
DiamondVmStatus file_path_dirname_helper(DiamondVm *vm,const DiamondString *path,
        DiamondValue *out) {
    size_t length=path->length;
    while(length>1&&path->chars[length-1]=='/')length--;
    size_t last_slash=SIZE_MAX;
    for(size_t index=length;index>0;index--)
        if(path->chars[index-1]=='/') {last_slash=index-1;break;}
    const char *result_chars;size_t result_length;
    if(last_slash==SIZE_MAX) {result_chars=".";result_length=1;}
    else if(last_slash==0) {result_chars="/";result_length=1;}
    else {
        result_chars=path->chars;result_length=last_slash;
        while(result_length>1&&result_chars[result_length-1]=='/')result_length--;
    }
    DiamondString *dirname=allocate_string(vm,result_chars,result_length);
    if(dirname==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(dirname);
    return DIAMOND_VM_OK;
}

/* File.basename(path, suffix = nil) -- the last path component, with
 * trailing separators on the input ignored first. `path` == "" ->    "",
 * `path` made entirely of separators (e.g. "/") -> "/". `suffix`, when
 * given and non-empty, is stripped from the end of the result if it
 * matches exactly (an exact-literal match, unlike Ruby's own ".*"
 * wildcard suffix support -- not needed here, kept simple). */
DiamondVmStatus file_path_basename_helper(DiamondVm *vm,const DiamondString *path,
        const DiamondString *suffix,DiamondValue *out) {
    if(path->length==0) {
        DiamondString *basename=allocate_string(vm,"",0);
        if(basename==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out=DIAMOND_OBJECT(basename);return DIAMOND_VM_OK;
    }
    size_t length=path->length;
    while(length>1&&path->chars[length-1]=='/')length--;
    size_t start=0;
    for(size_t index=length;index>0;index--)
        if(path->chars[index-1]=='/') {start=index;break;}
    const char *result_chars=path->chars+start;
    size_t result_length=length-start;
    if(result_length==0) {result_chars="/";result_length=1;}
    else if(suffix!=nullptr&&suffix->length>0&&suffix->length<result_length&&
            memcmp(result_chars+result_length-suffix->length,suffix->chars,suffix->length)==0) {
        result_length-=suffix->length;
    }
    DiamondString *basename=allocate_string(vm,result_chars,result_length);
    if(basename==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(basename);
    return DIAMOND_VM_OK;
}

/* File.extname(path) -- from the last '.' in the *basename* onward
 * (a '.' inside a directory component doesn't count), matching Ruby:
 * a dotfile's own leading dot(s) never start an extension
 * (".bashrc" -> "", "..bashrc" -> ""), and a dot with nothing after it
 * has no extension either ("a." -> ""). "" when there is none. */
DiamondVmStatus file_path_extname_helper(DiamondVm *vm,const DiamondString *path,
        DiamondValue *out) {
    size_t length=path->length;
    while(length>1&&path->chars[length-1]=='/')length--;
    size_t start=0;
    for(size_t index=length;index>0;index--)
        if(path->chars[index-1]=='/') {start=index;break;}
    const char *base=path->chars+start;
    const size_t base_length=length-start;
    size_t leading_dots=0;
    while(leading_dots<base_length&&base[leading_dots]=='.')leading_dots++;
    size_t last_dot=SIZE_MAX;
    for(size_t index=base_length;index>leading_dots;index--)
        if(base[index-1]=='.') {last_dot=index-1;break;}
    const char *result_chars="";size_t result_length=0;
    if(last_dot!=SIZE_MAX&&last_dot+1<base_length) {
        result_chars=base+last_dot;result_length=base_length-last_dot;
    }
    DiamondString *extname=allocate_string(vm,result_chars,result_length);
    if(extname==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(extname);
    return DIAMOND_VM_OK;
}

/* File.expand_path(path, base = nil) -- resolves `path` to an absolute,
 * lexically-normalized path (no filesystem access beyond `getcwd()`;
 * none of the intermediate components need to exist). An already-
 * absolute `path` is normalized as-is (`base` is then irrelevant, same
 * as Ruby). Otherwise `path` is joined onto `base` -- itself resolved
 * against the current working directory first if `base` is relative --
 * or onto the current working directory directly when `base` is nil.
 * Normalization then walks the combined path splitting on '/', dropping
 * empty and "." segments, and popping the previous real segment on
 * ".." (kept literally only when there's nothing left to pop -- this
 * never climbs above the root, matching Ruby). */
DiamondVmStatus file_path_expand_helper(DiamondVm *vm,const DiamondString *path,
        const DiamondString *base,DiamondValue *out) {
    StringBuilder combined={};
    bool ok=true;
    if(path->length>0&&path->chars[0]=='/') {
        ok=builder_append(&combined,path->chars,path->length);
    } else {
        char cwd_buffer[4096];
        if((base==nullptr||base->length==0||base->chars[0]!='/')&&
           getcwd(cwd_buffer,sizeof cwd_buffer)==nullptr) {
            free(combined.chars);
            snprintf(vm->error,sizeof vm->error,
                "cannot determine current directory: %s",strerror(errno));
            return DIAMOND_VM_IO_ERROR;
        }
        if(base!=nullptr&&base->length>0) {
            if(base->chars[0]=='/') ok=builder_append(&combined,base->chars,base->length);
            else ok=builder_append(&combined,cwd_buffer,strlen(cwd_buffer))&&
                    builder_append(&combined,"/",1)&&
                    builder_append(&combined,base->chars,base->length);
        } else ok=builder_append(&combined,cwd_buffer,strlen(cwd_buffer));
        if(ok&&path->length>0)
            ok=builder_append(&combined,"/",1)&&
               builder_append(&combined,path->chars,path->length);
    }
    if(!ok) {free(combined.chars);return DIAMOND_VM_OUT_OF_MEMORY;}
    size_t *segment_starts=malloc((combined.length+1)*sizeof(size_t));
    if(segment_starts==nullptr) {free(combined.chars);return DIAMOND_VM_OUT_OF_MEMORY;}
    StringBuilder normalized={};
    ok=builder_append(&normalized,"/",1);
    size_t segment_count=0,index=0;
    while(ok&&index<combined.length) {
        while(index<combined.length&&combined.chars[index]=='/')index++;
        const size_t segment_start=index;
        while(index<combined.length&&combined.chars[index]!='/')index++;
        const size_t segment_length=index-segment_start;
        if(segment_length==0||(segment_length==1&&combined.chars[segment_start]=='.'))
            continue;
        if(segment_length==2&&combined.chars[segment_start]=='.'&&
           combined.chars[segment_start+1]=='.') {
            if(segment_count>0) {
                normalized.length=segment_starts[--segment_count];
                normalized.chars[normalized.length]='\0';
            }
            continue;
        }
        const size_t mark=normalized.length;
        if(normalized.length>1)ok=builder_append(&normalized,"/",1);
        if(!ok)break;
        segment_starts[segment_count++]=mark;
        ok=builder_append(&normalized,combined.chars+segment_start,segment_length);
    }
    free(combined.chars);free(segment_starts);
    if(!ok) {free(normalized.chars);return DIAMOND_VM_OUT_OF_MEMORY;}
    DiamondString *expanded=allocate_string(vm,normalized.chars,normalized.length);
    free(normalized.chars);
    if(expanded==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(expanded);
    return DIAMOND_VM_OK;
}

/* Shared argv validation/marshaling behind Process.run and Process.spawn:
 * `owner` is the exact caller name ("Process.run"/"Process.spawn") for
 * error message prefixing, matching each call site's own existing
 * message wording. `*out_argv` points directly at each DiamondString's
 * own null-terminated buffer -- no copying needed, `argv_array` stays
 * reachable (still live in the caller's own register) for as long as
 * the resulting argv is used, and posix_spawn/execve never write
 * through argv despite the non-const `char *const []` signature (a
 * C89-main-signature-compatibility artifact, not a real mutation
 * contract). Caller owns freeing `*out_argv` (a plain malloc'd array of
 * borrowed pointers, not the strings themselves) once done with it. */
static DiamondVmStatus process_build_argv_helper(DiamondVm *vm,const char *owner,
        DiamondArray *argv_array,char ***out_argv,const char **out_command_name) {
    if(argv_array->count==0) {
        snprintf(vm->error,sizeof vm->error,"%s: argv must not be empty",owner);
        return DIAMOND_VM_ARITY_ERROR;
    }
    if(argv_array->count>DIAMOND_PROCESS_MAX_ARGV) {
        snprintf(vm->error,sizeof vm->error,
            "%s: argv has too many elements (max %d)",owner,DIAMOND_PROCESS_MAX_ARGV);
        return DIAMOND_VM_ARITY_ERROR;
    }
    for(size_t index=0;index<argv_array->count;index++) {
        const DiamondValue element=argv_array->values[index];
        if(element.kind!=DIAMOND_VALUE_OBJECT||
           element.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,"%s: argv must be an Array of Strings",owner);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *piece=(const DiamondString *)element.as.object;
        if(strlen(piece->chars)!=piece->length) {
            snprintf(vm->error,sizeof vm->error,
                "%s: argv strings must not contain a NUL byte",owner);
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    char **argv=malloc((argv_array->count+1)*sizeof(char *));
    if(argv==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    for(size_t index=0;index<argv_array->count;index++)
        argv[index]=((DiamondString *)argv_array->values[index].as.object)->chars;
    argv[argv_array->count]=nullptr;
    *out_argv=argv;
    *out_command_name=argv[0];
    return DIAMOND_VM_OK;
}

/* Process.run(argv): argv-array-only (never a shell string -- there is no
 * injection surface to guard against, by construction, matching the
 * design settled with the user before building this), blocking, full
 * stdout/stderr capture via a pipe pair and posix_spawnp. Child's stdin
 * is /dev/null (v1 deliberately has no way to feed it data -- see
 * docs/syntax.md). Reads both pipes with poll() rather than reading one
 * to EOF and then the other, specifically to avoid the classic deadlock
 * (child fills the stdout pipe buffer while blocked writing it, parent
 * is still blocked reading stderr, neither side ever makes progress).
 *
 * EINTR on poll()/read()/waitpid() just retries the syscall -- unlike
 * IO.poll's own EINTR handling, this does NOT run pending Signal.trap
 * handlers mid-wait (dispatch_pending_signals needs chunk/depth/ip from
 * run_chunk's own dispatch loop, which this helper -- deliberately kept
 * outside run_chunk's switch, per this file's stack-frame-budget
 * convention -- doesn't have). A signal trapped while a Process.run call
 * is blocked runs once the child exits and this call returns, not
 * immediately. Acceptable for a "minimal blocking capture" v1; a
 * non-blocking Process.spawn with a live handle (a real future feature,
 * not this one) would be the natural place to fix that. */
DiamondVmStatus process_run_helper(DiamondVm *vm,
        DiamondArray *argv_array,DiamondProcessResult *result) {
    char **argv=nullptr;const char *command_name=nullptr;
    const DiamondVmStatus argv_status=process_build_argv_helper(vm,"Process.run",
        argv_array,&argv,&command_name);
    if(argv_status!=DIAMOND_VM_OK)return argv_status;

    int stdout_pipe[2]={-1,-1};
    int stderr_pipe[2]={-1,-1};
    if(pipe(stdout_pipe)!=0||pipe(stderr_pipe)!=0) {
        const int saved_errno=errno;
        if(stdout_pipe[0]>=0)close(stdout_pipe[0]);
        if(stdout_pipe[1]>=0)close(stdout_pipe[1]);
        if(stderr_pipe[0]>=0)close(stderr_pipe[0]);
        if(stderr_pipe[1]>=0)close(stderr_pipe[1]);
        free(argv);
        snprintf(vm->error,sizeof vm->error,"Process.run: pipe: %s",
            strerror(saved_errno));
        return DIAMOND_VM_IO_ERROR;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_adddup2(&actions,stdout_pipe[1],STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions,stderr_pipe[1],STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions,stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&actions,stdout_pipe[1]);
    posix_spawn_file_actions_addclose(&actions,stderr_pipe[0]);
    posix_spawn_file_actions_addclose(&actions,stderr_pipe[1]);

    pid_t pid=0;
    const int spawn_status=posix_spawnp(&pid,command_name,&actions,
        nullptr,argv,environ);
    posix_spawn_file_actions_destroy(&actions);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    if(spawn_status!=0) {
        close(stdout_pipe[0]);close(stderr_pipe[0]);
        snprintf(vm->error,sizeof vm->error,"Process.run: %s: %s",
            command_name,strerror(spawn_status));
        free(argv);
        return DIAMOND_VM_IO_ERROR;
    }
    free(argv);

    StringBuilder stdout_builder={};
    StringBuilder stderr_builder={};
    bool stdout_open=true,stderr_open=true,out_of_memory=false;
    while((stdout_open||stderr_open)&&!out_of_memory) {
        struct pollfd fds[2];
        nfds_t fd_count=0;
        int stdout_slot=-1,stderr_slot=-1;
        if(stdout_open) {
            stdout_slot=(int)fd_count;
            fds[fd_count++]=(struct pollfd){.fd=stdout_pipe[0],.events=POLLIN};
        }
        if(stderr_open) {
            stderr_slot=(int)fd_count;
            fds[fd_count++]=(struct pollfd){.fd=stderr_pipe[0],.events=POLLIN};
        }
        errno=0;
        const int poll_result=poll(fds,fd_count,-1);
        if(poll_result<0) {
            if(errno==EINTR)continue;
            break;
        }
        char chunk_buffer[4096];
        if(stdout_slot>=0&&fds[stdout_slot].revents!=0) {
            const ssize_t bytes_read=read(stdout_pipe[0],chunk_buffer,sizeof chunk_buffer);
            if(bytes_read>0) {
                if(!builder_append(&stdout_builder,chunk_buffer,(size_t)bytes_read))
                    out_of_memory=true;
            } else if(bytes_read==0||errno!=EINTR) {
                close(stdout_pipe[0]);stdout_open=false;
            }
        }
        if(stderr_slot>=0&&fds[stderr_slot].revents!=0) {
            const ssize_t bytes_read=read(stderr_pipe[0],chunk_buffer,sizeof chunk_buffer);
            if(bytes_read>0) {
                if(!builder_append(&stderr_builder,chunk_buffer,(size_t)bytes_read))
                    out_of_memory=true;
            } else if(bytes_read==0||errno!=EINTR) {
                close(stderr_pipe[0]);stderr_open=false;
            }
        }
    }
    if(stdout_open)close(stdout_pipe[0]);
    if(stderr_open)close(stderr_pipe[0]);

    int wait_status=0;
    pid_t wait_result=0;
    do { wait_result=waitpid(pid,&wait_status,0); }
    while(wait_result<0&&errno==EINTR);

    if(out_of_memory) {
        free(stdout_builder.chars);free(stderr_builder.chars);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    int64_t exit_code=255;
    if(wait_result==pid) {
        if(WIFEXITED(wait_status))exit_code=WEXITSTATUS(wait_status);
        else if(WIFSIGNALED(wait_status))exit_code=128+WTERMSIG(wait_status);
    }
    /* result is already rooted (assigned to the caller's dest register
     * before this helper runs) -- see allocate_process_result's own
     * comment -- so each field write below is safe the instant it
     * happens, same as String#split rooting its array before pushing. */
    DiamondString *stdout_string=allocate_string(vm,
        stdout_builder.chars?stdout_builder.chars:"",stdout_builder.length);
    free(stdout_builder.chars);
    if(stdout_string==nullptr){free(stderr_builder.chars);return DIAMOND_VM_OUT_OF_MEMORY;}
    result->stdout_value=DIAMOND_OBJECT(stdout_string);
    /* result may have been promoted by a minor collection at any point
     * while this whole (potentially slow) helper ran with it already
     * rooted but its fields still nil -- these raw field writes bypass
     * DIAMOND_OP_SET_IVAR entirely, so each needs its own barrier call. */
    if(!gc_write_barrier(vm,(DiamondObject *)result)) {
        free(stderr_builder.chars);return DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondString *stderr_string=allocate_string(vm,
        stderr_builder.chars?stderr_builder.chars:"",stderr_builder.length);
    free(stderr_builder.chars);
    if(stderr_string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    result->stderr_value=DIAMOND_OBJECT(stderr_string);
    if(!gc_write_barrier(vm,(DiamondObject *)result))return DIAMOND_VM_OUT_OF_MEMORY;
    result->exit_code=exit_code;
    return DIAMOND_VM_OK;
}

/* Process.spawn(argv): like Process.run, but doesn't wait -- returns a
 * live DiamondProcessHandle immediately with two O_NONBLOCK stdout/
 * stderr streams (DiamondProcessStream, each independently pollable via
 * IO.poll -- see pollable_fd) and the child's pid, rather than blocking
 * to capture output and an exit code. No output capture, no reaping
 * here: the caller drains the streams (optionally via IO.poll) and
 * calls #wait themselves, on their own schedule. Child's stdin is
 * /dev/null, the same v1 scope cut Process.run's own comment documents
 * -- a writable stdin is a real, separate future slice, not this one. */
DiamondVmStatus process_spawn_helper(DiamondVm *vm,
        DiamondArray *argv_array,DiamondProcessHandle *handle) {
    char **argv=nullptr;const char *command_name=nullptr;
    const DiamondVmStatus argv_status=process_build_argv_helper(vm,"Process.spawn",
        argv_array,&argv,&command_name);
    if(argv_status!=DIAMOND_VM_OK)return argv_status;

    int stdout_pipe[2]={-1,-1};
    int stderr_pipe[2]={-1,-1};
    if(pipe(stdout_pipe)!=0||pipe(stderr_pipe)!=0) {
        const int saved_errno=errno;
        if(stdout_pipe[0]>=0)close(stdout_pipe[0]);
        if(stdout_pipe[1]>=0)close(stdout_pipe[1]);
        if(stderr_pipe[0]>=0)close(stderr_pipe[0]);
        if(stderr_pipe[1]>=0)close(stderr_pipe[1]);
        free(argv);
        snprintf(vm->error,sizeof vm->error,"Process.spawn: pipe: %s",strerror(saved_errno));
        return DIAMOND_VM_IO_ERROR;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_adddup2(&actions,stdout_pipe[1],STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions,stderr_pipe[1],STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions,stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&actions,stdout_pipe[1]);
    posix_spawn_file_actions_addclose(&actions,stderr_pipe[0]);
    posix_spawn_file_actions_addclose(&actions,stderr_pipe[1]);

    pid_t pid=0;
    const int spawn_status=posix_spawnp(&pid,command_name,&actions,nullptr,argv,environ);
    posix_spawn_file_actions_destroy(&actions);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    if(spawn_status!=0) {
        close(stdout_pipe[0]);close(stderr_pipe[0]);
        snprintf(vm->error,sizeof vm->error,"Process.spawn: %s: %s",
            command_name,strerror(spawn_status));
        free(argv);
        return DIAMOND_VM_IO_ERROR;
    }
    free(argv);

    /* Neither read end inherits O_NONBLOCK from anywhere -- pipe(2)
     * always creates blocking fds -- so both are set explicitly here,
     * the same "accept() never inherits it either" reasoning
     * TCPServer.listen_nonblocking's own accept() handler already
     * documents. */
    int read_fds[2]={stdout_pipe[0],stderr_pipe[0]};
    for(size_t index=0;index<2;index++) {
        const int flags=fcntl(read_fds[index],F_GETFL,0);
        if(flags<0||fcntl(read_fds[index],F_SETFL,flags|O_NONBLOCK)<0) {
            const int saved_errno=errno;
            close(stdout_pipe[0]);close(stderr_pipe[0]);
            snprintf(vm->error,sizeof vm->error,"Process.spawn: %s",strerror(saved_errno));
            return DIAMOND_VM_IO_ERROR;
        }
    }

    handle->pid=pid;
    /* `handle` is already rooted (assigned to the caller's dest register
     * before this helper runs -- see DIAMOND_OP_PROCESS_SPAWN), so each
     * stream field write below needs its own gc_write_barrier call, the
     * same requirement allocate_process_result's own two field writes
     * have. */
    DiamondProcessStream *stdout_stream=allocate_process_stream(vm,stdout_pipe[0]);
    if(stdout_stream==nullptr) {
        close(stdout_pipe[0]);close(stderr_pipe[0]);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    handle->stdout_stream=DIAMOND_OBJECT(stdout_stream);
    if(!gc_write_barrier(vm,(DiamondObject *)handle)) {
        close(stderr_pipe[0]);return DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondProcessStream *stderr_stream=allocate_process_stream(vm,stderr_pipe[0]);
    if(stderr_stream==nullptr) {close(stderr_pipe[0]);return DIAMOND_VM_OUT_OF_MEMORY;}
    handle->stderr_stream=DIAMOND_OBJECT(stderr_stream);
    if(!gc_write_barrier(vm,(DiamondObject *)handle))return DIAMOND_VM_OUT_OF_MEMORY;
    return DIAMOND_VM_OK;
}

/* Reaps `handle`'s child exactly once -- every dispatch path that needs
 * to know whether the child has exited (#wait, #running?, and the
 * kill/terminate guard against a recycled pid) funnels through this
 * rather than calling waitpid directly, so `reaped`/`exit_code` can
 * never be set twice. `blocking` chooses waitpid's WNOHANG: #wait wants
 * to actually block until the child exits (blocking=true); #running?
 * wants an instant answer either way (blocking=false, WNOHANG). Sets
 * *out_still_running only when !blocking and the child hasn't exited
 * yet. */
static DiamondVmStatus process_wait_helper(DiamondVm *vm,DiamondProcessHandle *handle,
        bool blocking,bool *out_still_running) {
    if(out_still_running!=nullptr)*out_still_running=false;
    if(handle->reaped)return DIAMOND_VM_OK;
    int wait_status=0;
    pid_t wait_result=0;
    do {
        wait_result=waitpid(handle->pid,&wait_status,blocking?0:WNOHANG);
    } while(wait_result<0&&errno==EINTR);
    if(wait_result==0) {
        /* WNOHANG and still running -- not reaped, nothing to cache. */
        if(out_still_running!=nullptr)*out_still_running=true;
        return DIAMOND_VM_OK;
    }
    if(wait_result<0) {
        snprintf(vm->error,sizeof vm->error,"Process::Handle#wait: %s",strerror(errno));
        return DIAMOND_VM_IO_ERROR;
    }
    int64_t exit_code=255;
    if(WIFEXITED(wait_status))exit_code=WEXITSTATUS(wait_status);
    else if(WIFSIGNALED(wait_status))exit_code=128+WTERMSIG(wait_status);
    handle->exit_code=exit_code;
    handle->reaped=true;
    return DIAMOND_VM_OK;
}

DiamondVmStatus process_handle_dispatch_helper(DiamondVm *vm,
        DiamondProcessHandle *target,const DiamondStringConstant *method_name,
        DiamondValue *registers,uint8_t argc,uint16_t dest) {
    if(method_name->length==3&&memcmp(method_name->chars,"pid",3)==0) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_INT((int64_t)target->pid);return DIAMOND_VM_OK;
    }
    if(method_name->length==6&&memcmp(method_name->chars,"stdout",6)==0) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=target->stdout_stream;return DIAMOND_VM_OK;
    }
    if(method_name->length==6&&memcmp(method_name->chars,"stderr",6)==0) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=target->stderr_stream;return DIAMOND_VM_OK;
    }
    if(method_name->length==4&&memcmp(method_name->chars,"wait",4)==0) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        /* Deliberately does not drain stdout_stream/stderr_stream first
         * -- a child that fills the OS pipe buffer while nobody reads
         * it can deadlock right here, the same well-documented gotcha
         * every language's own "wait without draining" API has (e.g.
         * Python's subprocess.Popen.wait()). A caller that cares about
         * output either drains the streams itself (optionally via
         * IO.poll) while the child runs, or accepts that #wait can hang
         * for a chatty child -- see docs/io.md. */
        const DiamondVmStatus wait_status=process_wait_helper(vm,target,true,nullptr);
        if(wait_status!=DIAMOND_VM_OK)return wait_status;
        registers[dest]=DIAMOND_INT(target->exit_code);return DIAMOND_VM_OK;
    }
    if(method_name->length==8&&memcmp(method_name->chars,"running?",8)==0) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        bool still_running=false;
        const DiamondVmStatus poll_status=
            process_wait_helper(vm,target,false,&still_running);
        if(poll_status!=DIAMOND_VM_OK)return poll_status;
        registers[dest]=DIAMOND_BOOL(still_running);return DIAMOND_VM_OK;
    }
    if((method_name->length==9&&memcmp(method_name->chars,"terminate",9)==0)||
       (method_name->length==4&&memcmp(method_name->chars,"kill",4)==0)) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        const bool terminate=method_name->length==9;
        /* Guarded by `reaped`, not just "did waitpid ever run": once a
         * pid has actually been reaped, the OS is free to recycle it for
         * an unrelated process, and signaling a stale pid at that point
         * would hit whatever that pid means now, not this child. A child
         * that has exited but hasn't been reaped *yet* (a zombie) is
         * still safe to signal -- POSIX guarantees a zombie's pid stays
         * reserved until reaped, so this is only a mistake, never a
         * race, past that point. */
        if(target->reaped) {
            snprintf(vm->error,sizeof vm->error,
                "Process::Handle#%s: process has already exited",
                terminate?"terminate":"kill");
            return DIAMOND_VM_IO_ERROR;
        }
        if(kill(target->pid,terminate?SIGTERM:SIGKILL)!=0) {
            snprintf(vm->error,sizeof vm->error,"Process::Handle#%s: %s",
                terminate?"terminate":"kill",strerror(errno));
            return DIAMOND_VM_IO_ERROR;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for Process::Handle",
        (int)method_name->length,method_name->chars);
    return DIAMOND_VM_TYPE_ERROR;
}

/* Process::Stream (Process.spawn's #stdout/#stderr) -- read(2) directly
 * against the raw, already-O_NONBLOCK fd, the exact same shape and
 * error taxonomy DIAMOND_OBJECT_SOCKET's own read dispatch uses (EAGAIN
 * -> WouldBlockError, a clean 0-byte read -> nil/EOF), since a
 * poll-driven caller needs the identical "nothing yet" vs. "nothing
 * ever again" distinction here too. No #write -- this is a read-only
 * pipe end; the child's stdin is /dev/null (see process_spawn_helper),
 * so there is nothing to write to. */
DiamondVmStatus process_stream_dispatch_helper(DiamondVm *vm,
        DiamondProcessStream *target,const DiamondStringConstant *method_name,
        DiamondValue *registers,uint8_t argc,uint16_t base,uint16_t dest) {
    const bool read_method=method_name->length==4&&memcmp(method_name->chars,"read",4)==0;
    const bool close_method=method_name->length==5&&memcmp(method_name->chars,"close",5)==0;
    if(!read_method&&!close_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for Process::Stream",
            (int)method_name->length,method_name->chars);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(target->fd>=0) {close(target->fd);target->fd=-1;}
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(target->fd<0) {
        snprintf(vm->error,sizeof vm->error,"process stream is closed");
        return DIAMOND_VM_IO_ERROR;
    }
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||registers[base].as.integer<0) {
        snprintf(vm->error,sizeof vm->error,
            "Process::Stream#read argument must be a non-negative Int");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const size_t want=(size_t)registers[base].as.integer;
    if(want==0) {
        DiamondString *empty=allocate_string(vm,"",0);
        if(empty==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(empty);return DIAMOND_VM_OK;
    }
    char *buffer=malloc(want);
    if(buffer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    errno=0;
    const ssize_t read_count=read(target->fd,buffer,want);
    if(read_count<0) {
        const int saved_errno=errno;
        free(buffer);
        if(saved_errno==EAGAIN||saved_errno==EWOULDBLOCK) {
            snprintf(vm->error,sizeof vm->error,"read would block");
            return DIAMOND_VM_WOULD_BLOCK;
        }
        snprintf(vm->error,sizeof vm->error,"read error: %s",strerror(saved_errno));
        return DIAMOND_VM_IO_ERROR;
    }
    if(read_count==0) {
        free(buffer);
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    DiamondString *string=allocate_string(vm,buffer,(size_t)read_count);
    free(buffer);
    if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
}

DiamondVmStatus process_result_dispatch_helper(DiamondVm *vm,
        DiamondProcessResult *target,const DiamondStringConstant *method_name,
        DiamondValue *registers,uint8_t argc,uint16_t dest) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    if(method_name->length==6&&memcmp(method_name->chars,"stdout",6)==0) {
        registers[dest]=target->stdout_value;return DIAMOND_VM_OK;
    }
    if(method_name->length==6&&memcmp(method_name->chars,"stderr",6)==0) {
        registers[dest]=target->stderr_value;return DIAMOND_VM_OK;
    }
    if(method_name->length==9&&memcmp(method_name->chars,"exit_code",9)==0) {
        registers[dest]=DIAMOND_INT(target->exit_code);return DIAMOND_VM_OK;
    }
    if(method_name->length==8&&memcmp(method_name->chars,"success?",8)==0) {
        registers[dest]=DIAMOND_BOOL(target->exit_code==0);return DIAMOND_VM_OK;
    }
    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for Process::Result",
        (int)method_name->length,method_name->chars);
    return DIAMOND_VM_TYPE_ERROR;
}
