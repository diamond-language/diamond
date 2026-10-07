#ifndef DIAMOND_VM_INTERNAL_H
#define DIAMOND_VM_INTERNAL_H

/* Declarations shared between src/vm.c and the vm_*.c files split out of it.
 *
 * vm.c grew past 26,000 lines, one function (run_chunk) of them over 8,000, and an edit anywhere
 * recompiled all of it: about six minutes under clang's ASan+UBSan. The subsystems that run_chunk
 * reaches only through out-of-line helpers (ProgramBuilder, the database drivers, network, time,
 * tensors, JSON, regexps, processes and files) moved into their own files, and so did run_chunk
 * itself (vm_run_chunk.c). What they share with the interpreter is declared here.
 *
 * This is not the embedding API (that is vm.h). Nothing here is a stable interface, and the
 * symbols are hidden, so they do not widen what a shared build of the runtime exports. */

#include "vm.h"
#include "compiler.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>

#define DIAMOND_INTERNAL __attribute__((visibility("hidden")))

/* Fibers hand-switch the C stack, which ASan needs told about (see vm.c): the macro and the
 * annotation header are shared by vm.c and src/vm_run_chunk.c, which switch fibers on resume and
 * yield. */
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
#define DIAMOND_ASAN_FIBERS 1
#include <sanitizer/common_interface_defs.h>
#endif

/* Types shared by the files split out of vm.c. */
/* Native backing struct for DiamondChannelHandle (object.h) -- see docs/
 * threads.md's Channels section. Unlike DiamondThread (owned one-to-one
 * by exactly one DiamondThreadHandle), a DiamondChannel is genuinely
 * shared: `refcount` counts every live DiamondChannelHandle referencing
 * it, possibly across several independent VM heaps at once (a Channel
 * passed as a Thread.new argument, sent through another Channel, or
 * copied inside an Array/Hash/Instance all bump this via copy_value_
 * into_vm's own DIAMOND_OBJECT_CHANNEL case) -- freed only once the last
 * one is swept (free_channel_reference).
 *
 * `private_vm`/`private_program` exist purely as GC-managed storage for
 * queued values, never to run bytecode: nothing ever calls run_chunk
 * against private_vm. private_program is a clone_program_from_chunk
 * clone of whatever program was ambient at Channel.new time (identical
 * shape to how Thread.new clones one for a spawned thread's own use) --
 * send() rebases an incoming value from the sender's own ambient classes
 * into private_program's classes (via copy_value_into_vm, exactly like
 * Thread.new's own argument copy); receive() rebases the other direction
 * (exactly like Thread#join's own result copy). Every value queued is
 * therefore always a value private_vm itself owns -- queue[] doubles as
 * private_vm->extra_roots (see that field's own comment, src/vm.h) so
 * private_vm's own collections can find them.
 *
 * `queue` is a flat, non-ring `malloc`'d DiamondValue[capacity] buffer:
 * receive() takes queue[0] and memmoves the remainder down rather than
 * tracking a separate head index -- simpler than ring-buffer index math,
 * and keeps the extra_roots hook a trivial flat pointer+count. Expected
 * capacities (tens to low thousands) make the memmove cost a non-issue.
 *
 * `lock` serializes every access to this struct, including every
 * allocation on private_vm -- since private_vm is never touched by more
 * than one OS thread at a time (always under this same lock), this
 * satisfies the real invariant GC safety needs (see diamond_vm_collect's
 * own contract) without needing private_vm to be pinned to one thread
 * for its whole lifetime the way a spawned Thread's own child_vm is.
 * `not_empty`/`not_full` are this codebase's first condition variables
 * -- see send/receive's own dispatch comments (DIAMOND_OP_INVOKE) for
 * the exact wait/signal protocol. */
typedef struct ChannelWaitLink {
    struct ChannelWaitLink *next;
    int write_fd;
} ChannelWaitLink;

/* Types shared by the files split out of vm.c. */
typedef struct DiamondChannel {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    ChannelWaitLink *waiters;
    DiamondVm *private_vm;
    DiamondProgram *private_program;
    DiamondValue *queue;
    size_t capacity;
    size_t count;
    bool closed;
    atomic_size_t refcount;
} DiamondChannel;

enum { TIME_BEGINNING_OF_DAY,TIME_END_OF_DAY,TIME_BEGINNING_OF_MONTH,
    TIME_END_OF_MONTH,TIME_BEGINNING_OF_WEEK,TIME_END_OF_WEEK,
    TIME_BEGINNING_OF_YEAR,TIME_END_OF_YEAR,TIME_BEGINNING_OF_QUARTER,
    TIME_END_OF_QUARTER };

enum { TIME_SHIFT_DAYS,TIME_SHIFT_WEEKS,TIME_SHIFT_MONTHS,TIME_SHIFT_YEARS };

enum { DIAMOND_TIME_LOCAL,DIAMOND_TIME_UTC,DIAMOND_TIME_FIXED_OFFSET };

/* Types shared by the files split out of vm.c. */
typedef struct GrowBuffer {
    char *data;
    size_t length;
    size_t capacity;
} GrowBuffer;

/* Each run_chunk activation unconditionally allocates DiamondValue
 * registers[256] (4KB), DiamondTypeBinding bindings[8] (~3.2KB), and
 * UnwindHandler handlers[16] (~0.5KB) as C-stack locals, regardless of the
 * called function's actual complexity. Measured against this machine's
 * default 8MB stack, real (non-instrumented) recursion segfaults around
 * depth ~210-220 in a debug (-O0) build and ~150-160 under AddressSanitizer's
 * redzone-inflated frames -- both well below the depth this counter used to
 * allow, so the guard never had a chance to trip before the native stack
 * actually overflowed.
 *
 * Recalibrated 100 -> 95 when adding native Time support: `-fstack-usage`
 * showed only ~240 bytes of *reported* growth from the new opcodes/
 * arithmetic-and-comparison fallback branches, but that was enough to
 * flip `depth(5000)` (tests/run.sh) from a clean guard trip to a real
 * ASan stack-overflow -- exactly the redzone-per-named-local amplification
 * the READ_SHORT incident just below already documents, not a byte-count
 * story. Empirically, every value from 91 through 99 passed cleanly
 * against the post-Time frame size (100 was the only one that didn't).
 *
 * Recalibrated 95 -> 92 (2026-09-27) when Ubuntu 26.04's own apt-packaged
 * GCC moved to 15.2.0: at 95, a plain `make release` (-O3, no
 * instrumentation at all) segfaulted on `depth(5000)` under that specific
 * compiler -- this project's own GCC (Fedora, 16.2.1) still passed at 95,
 * so this was invisible until CI's `ubuntu:26.04` image actually ran it
 * (found via a local Docker container matching that image and toolchain
 * exactly, not a hunch). The safe window this time was only {92, 93} --
 * confirmed against `main` at the commit before this recalibration too,
 * so this was newly exposed by the toolchain, not by anything this
 * project changed -- much narrower than the 91-99 window above, so
 * there is very little margin left for the next opcode/local addition to
 * this function; a future recalibration may need to shrink run_chunk's
 * own stack footprint directly rather than only retuning this constant
 * again. 92 sits at the low end of that narrow window rather than the
 * middle, since a smaller value only ever makes a real overflow *less*
 * likely to be reached before this guard trips -- the one thing that
 * must still hold is `legacy_0091.di`'s own `depth(90)` succeeding (a
 * hard floor, confirmed at 92). If a future change reopens this margin
 * again, re-run the same empirical sweep -- across every build variant
 * this project ships (debug, release, sanitize, and a container image
 * matching whatever CI target regressed), not just the one you're
 * sitting at -- rebuild at a range of candidate values, check
 * `depth(5000)` at each, rather than guessing.
 *
 * run_chunk's own `registers` array (below) stays a fixed
 * DIAMOND_INLINE_REGISTER_COUNT-wide C-stack array, at the same 256 this
 * comment's own measurement was taken against, rather than a VLA sized to
 * DIAMOND_REGISTER_COUNT (4096, see vm.h) -- a function whose
 * live_register_count exceeds it (rare -- see DIAMOND_REGISTER_COUNT's
 * own comment) heap-allocates instead, which doesn't consume C stack at
 * all and so can't affect this depth calibration regardless of how large
 * it gets. That register-count widening's first pass also nearly broke
 * this guard for a completely different reason, worth remembering: the
 * new READ_SHORT (below) originally declared its own `high_`/`low_`
 * uint8_t locals to assemble each 16-bit operand, and with ~90 opcodes
 * now reading 1-4 such operands apiece, that put several hundred extra
 * named locals into this one function -- at -O0, under ASan, each got
 * its own padded/redzoned stack slot, which dwarfed the cost of the
 * registers array itself (shrinking it 256->64 barely moved the
 * measured crash depth) and pushed the real crash below depth 90,
 * *under* this very call-depth guard, silently defeating it exactly
 * like an oversized array would have. Rewriting READ_SHORT to read
 * straight out of chunk->code[] into `target_` without any named
 * intermediate restored the original margin. */
enum { DIAMOND_MAX_CALL_DEPTH = 92 };

/* Types shared by the files split out of vm.c. */
/* A growable byte buffer for regexp_replace_helper's own output, the only
 * place in this file that needs to build a string of unknown final length
 * incrementally rather than in one allocate_string call. */
typedef struct ByteBuffer {
    char *data;
    size_t length;
    size_t capacity;
} ByteBuffer;

/* Set on a StringBuilder by stringify_value so builder_format_value can
 * call a user-defined to_s on instances nested in an Array or Hash. Without
 * one (every other caller), nested instances print as #<ClassName>. */
typedef struct FormatContext {
    DiamondVm *vm;
    const DiamondChunk *chunk;
    size_t depth;
    /* Why formatting stopped, when builder_format_value returns false. */
    DiamondVmStatus status;
} FormatContext;

typedef struct StringBuilder {
    char *chars;
    size_t length;
    size_t capacity;
    const DiamondObject *active[32];
    size_t active_count;
    FormatContext *format_context;
    /* inspect() rather than to_s(): Strings are quoted and escaped, Symbols
     * keep their colon, and an Instance without its own inspect shows its
     * fields, so `["a", "b"]` and `["a, b"]` no longer print alike. */
    bool inspect;
} StringBuilder;

/* Allocation and shared primitives that stay in vm.c (the garbage collector lives there). */
DIAMOND_INTERNAL DiamondString *allocate_string(DiamondVm *vm, const char *chars, size_t length);
DIAMOND_INTERNAL bool copy_value_into_vm(DiamondVm *dest_vm, DiamondValue value,
                                         DiamondProgram *source_program,
                                         const DiamondClass *rebase_source_classes,
                                         const DiamondClass *rebase_dest_classes,
                                         const DiamondChunk **adopted_owner,
                                         DiamondValue *out);
DIAMOND_INTERNAL bool methods_have_own_named(const DiamondMethod *methods, size_t count,
                                             const char *name);
DIAMOND_INTERNAL bool sandbox_category_allowed(const char *category);

/* vm_program_builder.c: the ProgramBuilder native bridge. */
DIAMOND_INTERNAL DiamondProgramBuilder *allocate_program_builder(DiamondVm *vm);
DIAMOND_INTERNAL DiamondVmStatus program_builder_run_helper(DiamondVm *vm,
        DiamondProgramBuilder *builder, DiamondValue *result);
DIAMOND_INTERNAL DiamondVmStatus program_builder_invoke_helper(DiamondVm *vm,
        DiamondProgramBuilder *builder, const DiamondStringConstant *method_name,
        DiamondValue *registers, uint16_t base, uint8_t argc, size_t depth,
        DiamondValue *result);

/* vm_tensor.c: entry points called from run_chunk */
DIAMOND_INTERNAL DiamondTensor *allocate_tensor(DiamondVm *vm,size_t rows,size_t cols);
DIAMOND_INTERNAL DiamondVmStatus tensor_dispatch_helper(DiamondVm *vm,DiamondTensor *tensor, const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base, uint8_t argc,uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus tensor_from_array_helper(DiamondVm *vm,const DiamondArray *outer, DiamondValue *out);
DIAMOND_INTERNAL void tensor_random_helper(DiamondTensor *tensor,int64_t seed);

/* Shared primitives that stay in vm.c (needed by tensor code) */
DIAMOND_INTERNAL DiamondArray *allocate_array(DiamondVm *vm,const DiamondValue *values, size_t count);
DIAMOND_INTERNAL bool array_push(DiamondVm *vm,DiamondArray *array,DiamondValue value);
DIAMOND_INTERNAL bool numeric_as_double(DiamondValue value, double *out);

/* vm_regexp.c: entry points called from run_chunk and the rest of vm.c */
DIAMOND_INTERNAL DiamondVmStatus regexp_match_helper(DiamondVm *vm, const DiamondRegexp *regexp, const DiamondString *subject, bool test_only, DiamondValue *registers, uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus regexp_new_helper(DiamondVm *vm, const DiamondString *pattern, int64_t options, DiamondValue *result);
DIAMOND_INTERNAL DiamondVmStatus regexp_replace_block_helper(DiamondVm *vm, const DiamondChunk *chunk,size_t depth,const DiamondRegexp *regexp, const DiamondString *subject,const DiamondClosure *block, bool replace_all,DiamondValue *result);
DIAMOND_INTERNAL DiamondVmStatus regexp_replace_helper(DiamondVm *vm,const DiamondRegexp *regexp, const DiamondString *subject,const DiamondString *replacement, bool replace_all,DiamondValue *result);
DIAMOND_INTERNAL DiamondVmStatus regexp_scan_helper(DiamondVm *vm,const DiamondRegexp *regexp, const DiamondString *subject,DiamondValue *registers,uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus regexp_split_helper(DiamondVm *vm,const DiamondRegexp *regexp, const DiamondString *subject,DiamondValue *registers,uint16_t dest);

/* Shared primitives that stay in vm.c (needed by vm_regexp.c) */
DIAMOND_INTERNAL bool byte_buffer_append(ByteBuffer *buffer,const char *bytes,size_t count);
DIAMOND_INTERNAL DiamondVmStatus call_closure_helper(DiamondVm *vm,const DiamondChunk *chunk, const DiamondFunction *fn,const DiamondClosure *called, const DiamondValue *registers,uint16_t base,uint8_t argc,size_t depth, DiamondValue *result);
DIAMOND_INTERNAL bool gc_protect(DiamondVm *vm, DiamondValue value);
DIAMOND_INTERNAL DiamondVmStatus stringify_value(DiamondVm *vm,const DiamondChunk *chunk, size_t depth,DiamondValue value, DiamondValue *out);

/* vm_json.c: entry points called from run_chunk and the rest of vm.c */
DIAMOND_INTERNAL bool debug_json_append_escaped_string(GrowBuffer *buffer, const char *chars,size_t length);
DIAMOND_INTERNAL DiamondVmStatus json_parse_document(DiamondVm *vm,const char *source, size_t length,DiamondValue *out);
DIAMOND_INTERNAL DiamondVmStatus json_stringify_document(DiamondVm *vm,const DiamondChunk *chunk, size_t depth,DiamondValue value,DiamondValue *result);

/* Shared primitives that stay in vm.c (needed by vm_json.c) */
DIAMOND_INTERNAL DiamondHash *allocate_hash(DiamondVm *vm);
DIAMOND_INTERNAL bool builder_append(StringBuilder *builder,const char *chars,size_t length);
DIAMOND_INTERNAL bool builder_format_value(StringBuilder *builder,DiamondValue value);
DIAMOND_INTERNAL bool grow_buffer_append(GrowBuffer *buffer,const char *text,size_t text_length);
#define GROW_BUFFER_APPEND_LITERAL(buffer_,literal_) \
    grow_buffer_append((buffer_),(literal_),sizeof(literal_)-1)
DIAMOND_INTERNAL bool hash_set(DiamondVm *vm,DiamondHash *hash,DiamondValue key, DiamondValue value);

/* vm_time.c: entry points called from run_chunk and the rest of vm.c */
DIAMOND_INTERNAL DiamondVmStatus time_at_helper(DiamondVm *vm,DiamondValue epoch_value, DiamondValue *out_result);
DIAMOND_INTERNAL DiamondVmStatus time_build_helper(DiamondVm *vm,const DiamondValue *arguments, uint8_t mode,DiamondValue *out_result);
DIAMOND_INTERNAL bool time_comparison_fallback(DiamondValue left_value,DiamondValue right_value, DiamondOpCode opcode,DiamondValue *out_result);
DIAMOND_INTERNAL DiamondVmStatus time_dispatch_helper(DiamondVm *vm,DiamondTime *target, const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base, uint8_t argc,uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus time_now_helper(DiamondVm *vm,bool utc,DiamondValue *out_result);
DIAMOND_INTERNAL DiamondVmStatus time_parse_helper(DiamondVm *vm,DiamondValue input, DiamondValue *out_result);
DIAMOND_INTERNAL DiamondVmStatus time_relative_now_helper(DiamondVm *vm,DiamondValue duration_value, bool future,DiamondValue *out_result);
DIAMOND_INTERNAL bool time_struct_tm(const DiamondTime *target,struct tm *out);
DIAMOND_INTERNAL DiamondVmStatus time_subtract_fallback(DiamondVm *vm, DiamondValue left_value,DiamondValue right_value,DiamondValue *out_result);

/* Shared primitives that stay in vm.c (needed by vm_time.c) */
DIAMOND_INTERNAL DiamondTime *allocate_time(DiamondVm *vm,double epoch,uint8_t zone_mode, int32_t utc_offset);
DIAMOND_INTERNAL int days_in_calendar_month(int year,int month);
DIAMOND_INTERNAL bool format_time_default(const DiamondTime *target,StringBuilder *builder);
DIAMOND_INTERNAL bool format_time_iso8601(const DiamondTime *target,int precision, StringBuilder *builder);
DIAMOND_INTERNAL int parse_decimal_digits(const char *chars,size_t start,size_t count);
DIAMOND_INTERNAL bool parse_time_utc_offset(const DiamondString *string,int32_t *out_offset);
DIAMOND_INTERNAL bool parse_time_utc_offset_chars(const char *chars,size_t length, int32_t *out_offset);
DIAMOND_INTERNAL char *substitute_fixed_offset_z(const char *format,int64_t offset);
DIAMOND_INTERNAL int64_t weekday_calendar_distance(int wday,int64_t count,bool future);

/* vm_database.c: entry points called from run_chunk and the rest of vm.c */
DIAMOND_INTERNAL DiamondVmStatus mysql_dispatch_helper(DiamondVm *vm,DiamondMysqlHandle *target_db, const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base, uint8_t argc,uint16_t dest);
DIAMOND_INTERNAL void mysql_library_init_once_fn(void);
DIAMOND_INTERNAL DiamondVmStatus postgres_dispatch_helper(DiamondVm *vm,DiamondPostgresHandle *target_db, const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base, uint8_t argc,uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus sqlite3_dispatch_helper(DiamondVm *vm,DiamondSqlite3Handle *target_db, const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base, uint8_t argc,uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus sqlite3_open_helper(DiamondVm *vm,const DiamondString *path, DiamondValue mode,sqlite3 **out_db);
DIAMOND_INTERNAL DiamondVmStatus sqlite3_statement_dispatch_helper(DiamondVm *vm, DiamondSqlite3StatementHandle *target_stmt,const DiamondStringConstant *method_name, DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest);

/* Shared primitives that stay in vm.c (needed by vm_database.c) */
DIAMOND_INTERNAL DiamondSqlite3StatementHandle *allocate_sqlite3_statement_handle( DiamondVm *vm,sqlite3_stmt *stmt);

/* vm_process_io.c: entry points called from run_chunk and the rest of vm.c */
DIAMOND_INTERNAL DiamondVmStatus file_path_basename_helper(DiamondVm *vm,const DiamondString *path, const DiamondString *suffix,DiamondValue *out);
DIAMOND_INTERNAL DiamondVmStatus file_path_dirname_helper(DiamondVm *vm,const DiamondString *path, DiamondValue *out);
DIAMOND_INTERNAL DiamondVmStatus file_path_expand_helper(DiamondVm *vm,const DiamondString *path, const DiamondString *base,DiamondValue *out);
DIAMOND_INTERNAL DiamondVmStatus file_path_extname_helper(DiamondVm *vm,const DiamondString *path, DiamondValue *out);
DIAMOND_INTERNAL DiamondVmStatus file_path_join_helper(DiamondVm *vm,const DiamondValue *parts, size_t count,DiamondValue *out);
DIAMOND_INTERNAL DiamondVmStatus file_publish_helper(DiamondVm *vm,const DiamondString *path, const DiamondString *bytes);
DIAMOND_INTERNAL DiamondVmStatus file_read_helper(DiamondVm *vm,const DiamondString *path, DiamondValue *result);
DIAMOND_INTERNAL DiamondVmStatus file_rename_helper(DiamondVm *vm,const DiamondString *from, const DiamondString *to);
DIAMOND_INTERNAL DiamondVmStatus file_sync_helper(DiamondVm *vm,const DiamondString *path);
DIAMOND_INTERNAL DiamondVmStatus file_write_helper(DiamondVm *vm,const DiamondString *path, const DiamondString *data,DiamondValue *result);
DIAMOND_INTERNAL DiamondVmStatus process_handle_dispatch_helper(DiamondVm *vm, DiamondProcessHandle *target,const DiamondStringConstant *method_name, DiamondValue *registers,uint8_t argc,uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus process_result_dispatch_helper(DiamondVm *vm, DiamondProcessResult *target,const DiamondStringConstant *method_name, DiamondValue *registers,uint8_t argc,uint16_t dest);
DIAMOND_INTERNAL DiamondVmStatus process_run_helper(DiamondVm *vm, DiamondArray *argv_array,DiamondProcessResult *result);
DIAMOND_INTERNAL DiamondVmStatus process_spawn_helper(DiamondVm *vm, DiamondArray *argv_array,DiamondProcessHandle *handle);
DIAMOND_INTERNAL DiamondVmStatus process_stream_dispatch_helper(DiamondVm *vm, DiamondProcessStream *target,const DiamondStringConstant *method_name, DiamondValue *registers,uint8_t argc,uint16_t base,uint16_t dest);

/* Shared primitives that stay in vm.c (needed by vm_process_io.c) */
DIAMOND_INTERNAL DiamondProcessStream *allocate_process_stream(DiamondVm *vm,int fd);
DIAMOND_INTERNAL bool gc_write_barrier(DiamondVm *vm, DiamondObject *owner);

/* vm_network.c: entry points called from run_chunk and the rest of vm.c */
DIAMOND_INTERNAL DiamondVmStatus dns_resolve_helper(DiamondVm *vm,const DiamondChunk *chunk, size_t depth,DiamondValue host_value,DiamondValue channels,DiamondValue deadline, DiamondValue *out);
DIAMOND_INTERNAL DiamondVmStatus socket_finish_connect_helper(DiamondVm *vm,DiamondSocketHandle *handle);
DIAMOND_INTERNAL DiamondVmStatus tcp_connect_helper(DiamondVm *vm,const DiamondString *host, int64_t port,int64_t connect_timeout_ms,int *out_fd);
DIAMOND_INTERNAL DiamondVmStatus tcp_connect_nonblocking_helper(DiamondVm *vm, const DiamondString *address,int64_t port,DiamondSocketHandle **out_handle);
DIAMOND_INTERNAL DiamondVmStatus tcp_listen_helper(DiamondVm *vm,int64_t port, bool nonblocking,bool reuse_port,DiamondListenerHandle **out_handle);
DIAMOND_INTERNAL void tls_abort_handshake(DiamondTlsSocketHandle *handle);
DIAMOND_INTERNAL DiamondVmStatus tls_encode_alpn_protocols(DiamondVm *vm,const char *owner, const DiamondArray *protocols,unsigned char **out_buffer,unsigned int *out_length);
DIAMOND_INTERNAL DiamondVmStatus tls_finish_handshake(DiamondVm *vm, DiamondTlsSocketHandle *handle,const char **direction);
DIAMOND_INTERNAL void tls_format_error(char *buffer,size_t buffer_size);
DIAMOND_INTERNAL DiamondVmStatus tls_listen_helper(DiamondVm *vm,int64_t port, const char *cert_path,const char *key_path,const DiamondArray *alpn_protocols, const char *client_ca_path,DiamondListenerHandle **out_handle);
DIAMOND_INTERNAL int tls_new_session_callback(SSL *ssl,SSL_SESSION *session);
DIAMOND_INTERNAL DiamondVmStatus tls_read_chunk(DiamondVm *vm,SSL *ssl,void *buffer,size_t want, size_t *out_read,bool *out_eof);
DIAMOND_INTERNAL DiamondVmStatus tls_read_line(DiamondVm *vm,SSL *ssl,StringBuilder *builder, bool *saw_any);
DIAMOND_INTERNAL DiamondVmStatus tls_validate_alpn_protocols(DiamondVm *vm,const char *owner, const DiamondArray *protocols);
DIAMOND_INTERNAL DiamondVmStatus tls_write_all(DiamondVm *vm,SSL *ssl,const char *data,size_t length);
DIAMOND_INTERNAL DiamondVmStatus udp_socket_helper(DiamondVm *vm,bool bind_socket, int64_t port,DiamondUdpSocketHandle **out_handle);

/* Shared primitives that stay in vm.c (needed by vm_network.c) */
DIAMOND_INTERNAL DiamondListenerHandle *allocate_listener_handle(DiamondVm *vm,int fd, bool nonblocking);
DIAMOND_INTERNAL DiamondSocketHandle *allocate_socket_handle(DiamondVm *vm,int fd);
DIAMOND_INTERNAL DiamondUdpSocketHandle *allocate_udp_socket_handle(DiamondVm *vm,int fd);
DIAMOND_INTERNAL DiamondVmStatus cancellable_wait_helper(DiamondVm *vm,DiamondChannel *target, bool writable,DiamondValue cancellations,DiamondValue deadline_value, struct pollfd *fds,nfds_t fd_count,const bool *select_drained);
DIAMOND_INTERNAL int connect_with_timeout(int fd,const struct addrinfo *candidate, int64_t timeout_ms);
DIAMOND_INTERNAL DiamondVmStatus dispatch_pending_signals(DiamondVm *vm,const DiamondChunk *chunk, size_t depth,bool *any_invoked);

/* What run_chunk (src/vm_run_chunk.c) needs from the rest of the interpreter: the frame, thread and
 * supervisor types, the unwinding types, the small functions it must keep inlining, and a prototype
 * for every function it calls that vm.c defines. */
enum { DIAMOND_INLINE_REGISTER_COUNT = 256 };

/* How often run_chunk's own dispatch loop actually calls clock_gettime to
 * check a configured DIAMOND_MAX_WALL_MILLISECONDS budget (docs/sandbox.md's
 * own "Resource limits" section) -- masked against vm->instructions_executed
 * rather than checked every dispatch, since the clock read itself (unlike
 * the instruction-count comparison right next to it) is real, non-trivial
 * cost. Bounds the worst-case overshoot past the configured budget to
 * "however long this many opcodes take," negligible next to any
 * millisecond-scale budget someone would actually configure. Must be a
 * power of two minus one for the `&` mask below to work. */
enum { DIAMOND_RESOURCE_LIMIT_CLOCK_CHECK_MASK = 4095 };

/* How much further a program may run after a budget trips and its catchable
 * ResourceLimitError is raised, before DIAMOND_VM_RESOURCE_EXHAUSTED ends it.
 * Enough for a rescue/ensure clause to log and release what it holds; not
 * enough to keep working. Instructions and milliseconds past the configured
 * budget; the memory ceiling is the budget plus a quarter of it, but at least
 * DIAMOND_MEMORY_GRACE_MINIMUM_BYTES. */
enum { DIAMOND_INSTRUCTION_GRACE = 1000000, DIAMOND_WALL_GRACE_MILLISECONDS = 1000,
       DIAMOND_MEMORY_GRACE_MINIMUM_BYTES = 4 * 1024 * 1024 };

typedef enum PendingKind : uint8_t {
    PENDING_NONE,
    PENDING_NORMAL,
    PENDING_RETURN,
    PENDING_EXCEPTION,
    /* An ensure block run while a block's non-local return/break unwinds
     * through this frame; END_ENSURE resumes the unwinding. */
    PENDING_NONLOCAL,
} PendingKind;

typedef struct PendingUnwind {
    PendingKind kind;
    DiamondValue value;
    size_t continuation;
    uint64_t nonlocal_target;
    bool nonlocal_is_break;
} PendingUnwind;

typedef enum HandlerKind : uint8_t { HANDLER_RESCUE, HANDLER_ENSURE, HANDLER_ACTIVE_ENSURE } HandlerKind;

typedef struct UnwindHandler {
    HandlerKind kind;
    size_t target;
    uint16_t destination;
    uint8_t type_count;
    uint8_t types[8];
    bool enabled;
    PendingUnwind saved_pending;
} UnwindHandler;

/* An active ensure remains on the handler stack as an unwind boundary.
 * Its saved continuation survives nested cleanup, but is discarded when a
 * new exception or return escapes that cleanup. */
static inline UnwindHandler pop_unwind_handler(UnwindHandler *handlers,
        size_t *count,PendingUnwind *pending) {
    const UnwindHandler handler=handlers[--*count];
    if(handler.kind==HANDLER_ACTIVE_ENSURE)*pending=handler.saved_pending;
    return handler;
}

static inline size_t enter_ensure(UnwindHandler *handler,PendingUnwind *pending,
        PendingUnwind next) {
    handler->kind=HANDLER_ACTIVE_ENSURE;
    handler->saved_pending=*pending;
    *pending=next;
    return handler->target;
}

typedef struct DiamondFrame {
    struct DiamondFrame *previous;
    DiamondValue *registers;
    PendingUnwind *pending;
    UnwindHandler *handlers;
    size_t *handler_count;
    size_t register_count;
    /* Backtrace support (Exception#backtrace): the owning chunk plus a
     * pointer to run_chunk's own `instruction_offset` local, not a copied
     * value -- a suspended ancestor frame is a real, live C stack frame
     * blocked inside a nested call, so reading through the pointer always
     * reflects its current instruction (the call site) with no need to
     * sync a copy on every dispatch iteration. Valid for exactly the
     * frame's own lifetime, same as the frame struct itself. */
    const DiamondChunk *chunk;
    const size_t *instruction_offset;
    /* This activation's serial, and its method's (its own, unless it's a
     * do-block, whose method is the one its closure was created in). */
    uint64_t serial;
    uint64_t home;
} DiamondFrame;

/* Native backing struct for DiamondThreadHandle (object.h) -- see
 * docs/threads.md. `child_vm`/`child_program` are this thread's own,
 * fully independent heap/GC and a byte-for-byte memcpy clone of whatever
 * DiamondProgram was ambient at Thread.new time (see thread_new_helper),
 * never shared with the spawning thread's own program or any other
 * thread's clone. `function_index`/`args`/`arg_count` are captured at
 * spawn time so the OS-thread entry point (thread_entry_trampoline) has
 * everything it needs with no further reference back into the spawning
 * VM's own state. `result`/`raised` are only meaningful once `finished`
 * is true; `finished` is the only field the spawning thread may read
 * without holding `join_lock` (a plain atomic flag, set exactly once,
 * read-only afterward -- .alive?() polls it without blocking).
 * `join_lock` guards the joined/not-yet-joined transition so .join() is
 * safely callable more than once (pthread_join itself is not). */
typedef struct DiamondThread {
    pthread_t handle;
    /* True only once pthread_create actually succeeded for `handle` --
     * distinguishes "there's a real OS thread that must be pthread_join'd"
     * from the synchronous-stub path (Thread.new runs the callable inline,
     * no OS thread ever created) and from a failed pthread_create (also no
     * real thread to join). free_thread only calls pthread_join when this
     * is true. */
    bool spawned;
    DiamondVm *child_vm;
    DiamondProgram *child_program;
    uint16_t function_index;
    DiamondValue args[DIAMOND_MAX_ARGUMENTS];
    uint8_t arg_count;
    /* Three, mutually exclusive outcomes once `finished` is true:
     * (1) plain return -- `raised`/`internal_failure` both false, `result`
     *     is the returned value; (2) the target raised, uncaught -- only
     *     `raised` true, `result` is the actual Diamond exception instance
     *     to re-raise (still living in child_vm's own heap; .join() deep-
     *     copies it back, same as an ordinary return value); (3) the
     *     child hit an internal VM failure that was never a clean Diamond-
     *     level raise (stack overflow, invalid bytecode, ...) -- only
     *     `internal_failure` true, `result` unused, .join() reads
     *     child_vm->error directly (still alive at that point) to build a
     *     fresh ThreadError. */
    DiamondValue result;
    bool raised;
    bool internal_failure;
    atomic_bool finished;
    bool joined;
    pthread_mutex_t join_lock;
} DiamondThread;

DIAMOND_INTERNAL void notify_channel_waiters(DiamondChannel *channel);

/* A fixed cap on children per Supervisor, matching DIAMOND_MAX_THREADS/
 * DIAMOND_MAX_ARGUMENTS's own fixed-array style rather than dynamic
 * growth -- see docs/threads.md's Supervisors section. Each child still
 * separately counts against the process-wide DIAMOND_MAX_THREADS budget
 * (one real OS thread per child, for its entire supervised lifetime), so
 * this cap exists to bound one DiamondSupervisor's own fixed-size
 * children[] array, not as an independent resource budget. */
enum { DIAMOND_MAX_SUPERVISOR_CHILDREN = 32 };

typedef struct DiamondSupervisor DiamondSupervisor;

/* One supervised worker slot. `program_template` is cloned exactly once,
 * at add_child time (clone_program_from_chunk -- the same call Thread.new
 * and Channel.new already make), and reused unmodified across every
 * restart of this child: a program's functions/classes/interfaces tables
 * never change once compiled, only the heap data a run against them
 * produces, so there is no need to reclone on every crash the way
 * Thread.new reclones per spawn (a supervised child, unlike a plain
 * Thread, may be spawned/restarted arbitrarily many times over its
 * lifetime -- cloning once amortizes that cost across all of them).
 *
 * `args_vm` exists purely as GC-managed storage for `args[]`, exactly
 * Channel's own private_vm-for-storage trick (src/vm.c's DiamondChannel
 * comment above) -- but write-once, never mutated again after add_child,
 * since supervised args don't change across restarts. `args_vm->
 * extra_roots` is pointed at `args` so args_vm's own occasional GC cycle
 * (triggered only by add_child's own initial copy_value_into_vm calls)
 * keeps them alive. Every restart re-copies from args_vm into that
 * attempt's own fresh run_vm using program_template's classes on both
 * sides -- args_vm and every run_vm are structurally identical clones of
 * the same template, so this is always a same-layout rebase, never a
 * cross-program adopt.
 *
 * `last_error`/`restart_count`/`done` are guarded by the
 * owning DiamondSupervisor's own `lock` (not a per-child lock -- these
 * fields are read rarely, from the one calling thread's own restart_
 * count()/last_error()/alive?() calls, never on any hot path), and
 * written from exactly one place: this child's own dedicated retry-loop
 * OS thread (supervisor_child_entry_trampoline). */
typedef struct DiamondSupervisorChild {
    DiamondProgram *program_template;
    uint16_t function_index;
    DiamondVm *args_vm;
    DiamondValue args[DIAMOND_MAX_ARGUMENTS];
    uint8_t arg_count;
    pthread_t handle;
    DiamondSupervisor *supervisor;
    size_t restart_count;
    /* Same size as DiamondVm.error (src/vm.h) -- last_error is always
     * populated by copying either run_vm->error or format_uncaught_
     * exception_message's own output into it verbatim (supervisor_child_
     * entry_trampoline), so matching that buffer's own size exactly
     * avoids ever truncating it. */
    char last_error[1024];
    /* True once this child's retry loop has permanently stopped running
     * -- either a clean, non-raising return (v1 never restarts on a
     * normal return) or a crash noticed after stop() was called
     * (supervisor_child_entry_trampoline checks stop_requested right
     * after recording a crash, before the next attempt). alive?() is
     * exactly !done. */
    bool done;
    /* Guards against a double pthread_join on this child's own `handle`
     * (undefined behavior per POSIX) -- stop()/join()/free_supervisor_
     * reference are three independent call sites that each join every
     * child, and any combination of them may run against the same
     * Supervisor over its lifetime (stop() then join(), join() called
     * twice, ...). Set under `supervisor->lock` immediately before the
     * actual (unlocked) pthread_join call, mirroring DiamondThread's own
     * `joined` flag/join_lock pairing for the identical reason. */
    bool joined;
    /* Slot index in supervisor->children[], so a crashing child can tell
     * which siblings its strategy reaches. */
    size_t index;
    /* Set (under supervisor->lock) by a crashing sibling when the
     * supervisor's strategy says this child must restart too; polled by
     * this child's own attempt VM (DiamondVm.interrupt_flag) and
     * consumed by its retry loop. Cleared at the start of each attempt,
     * since a fresh attempt is already the restart being asked for. */
    atomic_bool interrupt;
} DiamondSupervisorChild;

/* Restart strategy, chosen once by Supervisor.new(:strategy). Mirrors
 * Erlang's three: only the crashed child (default), every child, or the
 * crashed child and every child added after it. */
typedef enum DiamondSupervisorStrategy {
    DIAMOND_SUPERVISOR_ONE_FOR_ONE,
    DIAMOND_SUPERVISOR_ONE_FOR_ALL,
    DIAMOND_SUPERVISOR_REST_FOR_ONE,
} DiamondSupervisorStrategy;

/* Native backing struct for DiamondSupervisorHandle (object.h) -- see
 * docs/threads.md's Supervisors section and docs/internal/concurrency-
 * internals.md for the full design. Unlike DiamondThread (owned one-to-
 * one) but like DiamondChannel (genuinely shared, refcounted), a
 * Supervisor is refcounted for free-safety consistency even though --
 * see docs/threads.md -- a Supervisor is deliberately not one of
 * copy_value_into_vm's handled kinds, so in practice no second real OS
 * thread can ever obtain a handle to the same Supervisor: `refcount`
 * only ever reaches more than 1 via an ordinary same-heap copy (e.g.
 * storing the same handle in two Array slots), never a cross-thread one.
 *
 * `lock` guards every mutable field below plus each child's own
 * restart_count/last_error/done (DiamondSupervisorChild's
 * own comment). `stop_requested` is a separate lock-free atomic,
 * deliberately not behind `lock`, since every child's retry loop checks
 * it on every single iteration (mirrors DiamondThread's own atomic
 * `finished` -- a hot, lock-free poll is the whole point). */
typedef struct DiamondSupervisor {
    pthread_mutex_t lock;
    atomic_bool stop_requested;
    DiamondSupervisorChild children[DIAMOND_MAX_SUPERVISOR_CHILDREN];
    size_t child_count;
    bool stopped;
    DiamondSupervisorStrategy strategy;
    atomic_size_t refcount;
} DiamondSupervisor;

DIAMOND_INTERNAL bool maybe_collect(DiamondVm *vm);
DIAMOND_INTERNAL void resource_limit_trip(DiamondVm *vm);
DIAMOND_INTERNAL DiamondVmStatus resource_hard_stop(DiamondVm *vm,const char *budget);

DIAMOND_INTERNAL void diamond_vm_init(DiamondVm *vm);

DIAMOND_INTERNAL void diamond_vm_free(DiamondVm *vm);

DIAMOND_INTERNAL void diamond_vm_invalidate_method_caches(DiamondVm *vm);

DIAMOND_INTERNAL DiamondFiber *diamond_fiber_new_for_closure(
        const DiamondChunk *chunk, const DiamondClosure *closure);

DIAMOND_INTERNAL void diamond_fiber_free(DiamondFiber *fiber);

DIAMOND_INTERNAL DiamondFiberStatus diamond_fiber_prepare(DiamondFiber *fiber);

DIAMOND_INTERNAL DiamondFiberStatus diamond_fiber_bind_vm(DiamondFiber *fiber, DiamondVm *vm);

DIAMOND_INTERNAL DiamondSymbol *allocate_symbol(DiamondVm *vm, const char *chars,
                                      size_t length);

DIAMOND_INTERNAL DiamondInstance *allocate_instance(DiamondVm *vm,const DiamondClass *class,
                                          const DiamondChunk *chunk);

static inline DiamondClosure *allocate_closure(DiamondVm *vm,uint16_t function_index,
                                        const DiamondValue *captures,size_t count) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondClosure *closure=malloc(sizeof(DiamondClosure));if(closure==nullptr)return nullptr;
    *closure=(DiamondClosure){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_CLOSURE},
      .function_index=function_index,.capture_count=(uint8_t)count};
    for(size_t i=0;i<count;i++)closure->captures[i]=captures[i];
    vm->young_objects=&closure->object;vm->bytes_allocated+=sizeof(DiamondClosure);return closure;
}

static inline DiamondCell *allocate_cell(DiamondVm *vm,DiamondValue value) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondCell *cell=malloc(sizeof(DiamondCell));if(cell==nullptr)return nullptr;
    *cell=(DiamondCell){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_CELL},.value=value};
    vm->young_objects=&cell->object;vm->bytes_allocated+=sizeof(DiamondCell);return cell;
}

DIAMOND_INTERNAL DiamondFiberHandle *allocate_fiber_handle(DiamondVm *vm,DiamondFiber *fiber);

/* A raw OS-thread limit, not a language-level tuning knob: real pthreads
 * are a genuinely limited, comparatively expensive OS resource (unlike
 * Fibers, cheap userspace stacks) and each one commits a cloned program --
 * 64 bounds resource use while still comfortably
 * covering the "a handful of coarse-grained parallel workers" use case
 * this primitive targets, and turns a runaway recursive Thread.new bug
 * into a prompt ThreadError instead of exhausting the host. Process-wide
 * (not per-DiamondVm) since threads spawned from unrelated VMs still
 * compete for the same real OS/memory resources. */
enum { DIAMOND_MAX_THREADS = 64 };

extern DIAMOND_INTERNAL atomic_size_t diamond_active_thread_count;

DIAMOND_INTERNAL int create_vm_thread(pthread_t *handle,
        void *(*entry)(void *), void *argument);

DIAMOND_INTERNAL DiamondProgram *clone_program_from_chunk(const DiamondChunk *chunk);

DIAMOND_INTERNAL void *thread_entry_trampoline(void *argument);

DIAMOND_INTERNAL DiamondThreadHandle *allocate_thread_handle(DiamondVm *vm,DiamondThread *thread);

DIAMOND_INTERNAL void free_thread(DiamondThread *thread);

DIAMOND_INTERNAL DiamondChannelHandle *allocate_channel_handle(DiamondVm *vm,DiamondChannel *channel);

DIAMOND_INTERNAL void free_channel_reference(DiamondChannel *channel);

DIAMOND_INTERNAL DiamondSupervisorHandle *allocate_supervisor_handle(DiamondVm *vm,
        DiamondSupervisor *supervisor);

DIAMOND_INTERNAL void free_supervisor_reference(DiamondSupervisor *supervisor);

DIAMOND_INTERNAL DiamondFileHandle *allocate_file_handle(DiamondVm *vm,FILE *stream);

DIAMOND_INTERNAL DiamondTlsSocketHandle *allocate_tls_socket_handle(DiamondVm *vm,SSL *ssl,int fd);

extern DIAMOND_INTERNAL const int diamond_signal_numbers[DIAMOND_SIGNAL_COUNT];

extern DIAMOND_INTERNAL const char *const diamond_signal_names[DIAMOND_SIGNAL_COUNT];

extern DIAMOND_INTERNAL atomic_uint diamond_pending_signals;

DIAMOND_INTERNAL void diamond_signal_handler(int signal_number);

DIAMOND_INTERNAL DiamondVmStatus apply_socket_timeouts_helper(DiamondVm *vm,int fd,
        int64_t read_timeout_ms,int64_t write_timeout_ms);

typedef struct {
    int64_t connect_timeout_ms;
    int64_t read_timeout_ms;
    int64_t write_timeout_ms;
    const DiamondString *ca_file;
    const DiamondString *ca_path;
    const DiamondString *cert;
    const DiamondString *key;
    const DiamondString *session;
    const DiamondArray *alpn;
} DiamondSocketConnectOptions;

DIAMOND_INTERNAL DiamondVmStatus parse_socket_connect_options_helper(DiamondVm *vm,
        DiamondValue options_value,bool allow_tls_fields,
        DiamondSocketConnectOptions *out);

DIAMOND_INTERNAL DiamondVmStatus parse_tls_listen_options_helper(DiamondVm *vm,
        DiamondValue options_value,const DiamondArray **out_alpn,
        const DiamondString **out_client_ca);

DIAMOND_INTERNAL DiamondVmStatus pollable_fd(DiamondVm *vm,DiamondValue value,int *out_fd);

DIAMOND_INTERNAL bool poll_register_fd(struct pollfd *fds,nfds_t *fd_count,size_t max_fds,
        int fd,short want_events,size_t *out_slot);

DIAMOND_INTERNAL DiamondSqlite3Handle *allocate_sqlite3_handle(DiamondVm *vm,sqlite3 *db);

DIAMOND_INTERNAL DiamondPostgresHandle *allocate_postgres_handle(DiamondVm *vm,PGconn *conn);

DIAMOND_INTERNAL DiamondMysqlHandle *allocate_mysql_handle(DiamondVm *vm,MYSQL *conn);

DIAMOND_INTERNAL DiamondProcessResult *allocate_process_result(DiamondVm *vm);

DIAMOND_INTERNAL DiamondProcessHandle *allocate_process_handle(DiamondVm *vm,pid_t pid);

DIAMOND_INTERNAL DiamondVmStatus compile_method_helper(DiamondVm *vm,const DiamondClass *target_class,
        const DiamondString *name_string,const DiamondArray *params_array,
        const DiamondString *body_string,const DiamondHash *bound_values_hash,
        DiamondValue *out_result);

/* Unwinds vm->gc_protected back to a mark saved from gc_protected_count
 * before a matching run of gc_protect calls -- strict LIFO, mirroring the
 * nested lifetime of copy_value_into_vm's own recursion. Never shrinks
 * the backing allocation, same as array/hash never shrinking on removal;
 * it's freed for real in diamond_vm_free. */
static inline void gc_unprotect(DiamondVm *vm, size_t saved_count) {
    vm->gc_protected_count=saved_count;
}

DIAMOND_INTERNAL const char *copy_failure_reason(void);

static inline bool value_is_bignum(DiamondValue value) {
    return value.kind==DIAMOND_VALUE_OBJECT&&
        value.as.object->kind==DIAMOND_OBJECT_BIGNUM;
}

/* True for both representations an Int can have -- a plain inline
 * int64_t (kind==DIAMOND_VALUE_INT) or a promoted DiamondBignum. Used
 * everywhere "is this conceptually an Int" matters (type checks,
 * cross-type dispatch); arithmetic fast paths that specifically need
 * "is this a small int64_t I can compute on directly" still check
 * kind==DIAMOND_VALUE_INT alone, unchanged. */
static inline bool is_int_value(DiamondValue value) {
    return value.kind==DIAMOND_VALUE_INT||value_is_bignum(value);
}

DIAMOND_INTERNAL bool values_equal(DiamondValue left, DiamondValue right);

DIAMOND_INTERNAL ptrdiff_t hash_find(const DiamondHash *hash,DiamondValue key);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_new_hash(DiamondVm *vm, DiamondValue *registers,
        uint16_t base, uint16_t count, DiamondValue *out);

static inline bool is_truthy(DiamondValue value) {
    return value.kind != DIAMOND_VALUE_NIL &&
           !(value.kind == DIAMOND_VALUE_BOOL && !value.as.boolean);
}

DIAMOND_INTERNAL const DiamondMethod *lookup_method(const DiamondChunk *chunk,
                                          const DiamondClass *class,
                                          const char *name, size_t length);

static inline const DiamondClass *method_declaring_class(const DiamondChunk *chunk,
        const DiamondClass *receiver_class,const DiamondMethod *method) {
    const DiamondClass *current=receiver_class;
    while(current!=nullptr) {
        for(size_t index=0;index<current->method_count;index++)
            if(method==&current->methods[index])return current;
        current=current->superclass==UINT8_MAX?nullptr:
            &chunk->classes[current->superclass];
    }
    return nullptr;
}

static inline bool class_is_a(const DiamondChunk *chunk,const DiamondClass *class,
        const DiamondClass *ancestor) {
    for(const DiamondClass *current=class;current!=nullptr;
        current=current->superclass==UINT8_MAX?nullptr:
            &chunk->classes[current->superclass])
        if(current==ancestor)return true;
    return false;
}

/* lookup_method's exact algorithm, over singleton_methods[] instead of
 * methods[] -- the runtime half of DIAMOND_OP_INVOKE_SELF_METHOD's
 * `self.foo(...)` dispatch: walks from the actual receiver class (read
 * out of a DIAMOND_VALUE_CLASS value at the call site, not the literal
 * class the calling method happens to be lexically defined in) up its
 * superclass chain, first match wins -- same MRO ordinary instance
 * dispatch already uses. */
static inline const DiamondMethod *lookup_singleton_method(const DiamondChunk *chunk,
                                          const DiamondClass *class,
                                          const char *name, size_t length) {
    const DiamondClass *current = class;
    while (current != nullptr) {
        for(size_t index=current->singleton_method_count;index>0;index--) {
            const DiamondMethod *method=&current->singleton_methods[index-1];
            if (strlen(method->name) == length &&
                memcmp(method->name, name, length) == 0) {
                return method;
            }
        }
        current = current->superclass == UINT8_MAX
            ? nullptr : &chunk->classes[current->superclass];
    }
    return nullptr;
}

DIAMOND_INTERNAL DiamondVmStatus invoke_operator_method(DiamondVm *vm,
        const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondInstance *receiver,
        const char *name, size_t name_length, const DiamondValue *arguments,
        size_t explicit_argument_count, DiamondValue *result, bool *found);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_equal_general(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *left,
        const DiamondValue *right, bool negate, DiamondValue *out);

DIAMOND_INTERNAL DiamondVmStatus case_match_value(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,const uint8_t *site,DiamondValue pattern,DiamondValue subject,
        bool *matched);

DIAMOND_INTERNAL DiamondVmStatus method_missing_helper(DiamondVm *vm,const DiamondChunk *owner,
        const DiamondInstance *receiver,const char *attempted_name,size_t attempted_length,
        const DiamondValue *call_args,size_t call_arg_count,size_t depth,
        DiamondValue *result,bool *found);

DIAMOND_INTERNAL bool operator_prelude_fallback(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,DiamondValue left,DiamondValue right,const char *word,
        DiamondValue *out,DiamondVmStatus *status);

DIAMOND_INTERNAL DiamondVmStatus add_fallback(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,
        size_t instruction_offset,DiamondValue left_value,DiamondValue right_value,
        DiamondValue *out_result);

DIAMOND_INTERNAL int diamond_string_compare(const DiamondString *a,const DiamondString *b);

DIAMOND_INTERNAL DiamondVmStatus int_arith_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, size_t instruction_offset, DiamondOpCode opcode,
        DiamondValue left_value, DiamondValue right_value, DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus compare_int_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, size_t instruction_offset, DiamondOpCode opcode,
        DiamondValue left_value, DiamondValue right_value, DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus bcrypt_hash_helper(DiamondVm *vm,DiamondValue password_value,
        DiamondValue cost_value,DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus bcrypt_verify_helper(DiamondVm *vm,DiamondValue password_value,
        DiamondValue digest_value,DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus secure_random_bytes_helper(DiamondVm *vm,DiamondValue count_value,
        DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus secure_random_hex_helper(DiamondVm *vm,DiamondValue count_value,
        DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus sha256_hex_helper(DiamondVm *vm,DiamondValue key_value,
        DiamondValue data_value,bool keyed,DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus sha1_hex_helper(DiamondVm *vm,DiamondValue key_value,
        DiamondValue data_value,bool keyed,DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus hmac_verify_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue key_value,DiamondValue signature_value,DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus aes_gcm_encrypt_helper(DiamondVm *vm,DiamondValue key_value,
        DiamondValue plaintext_value,DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus aes_gcm_decrypt_helper(DiamondVm *vm,DiamondValue key_value,
        DiamondValue blob_value,DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus gzip_compress_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus gzip_decompress_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue max_size_value,DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus base64_encode_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue *out_result);

DIAMOND_INTERNAL DiamondVmStatus base64_decode_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue *out_result);

DIAMOND_INTERNAL bool resolve_class_operand(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondValue *registers,const char *operation,uint8_t *class_operand);

DIAMOND_INTERNAL DiamondVmStatus exit_helper(DiamondVm *vm,DiamondValue code_value);

/* Cache lookup/store for find_collection_extension/find_value_extension --
 * see DiamondExtensionCache's own comment (src/vm.h). `kind` is whatever
 * distinguishes the receiver at that one call site: a DiamondObjectKind
 * for the collection protocol (DIAMOND_OBJECT_ARRAY/_HASH), or a small
 * fixed tag the three value-extension call sites each pick for their own
 * receiver type (Int/Float/String can never collide at the same site,
 * so any distinct values work). Mirrors lookup_method_cached's hashing
 * and replacement policy exactly, just against a value/bool pair instead
 * of a DiamondMethod pointer. */
static inline bool cached_extension_lookup(DiamondVm *vm,const uint8_t *site,
        uint8_t kind,const DiamondFunction **out_function) {
    const size_t slot=((size_t)(uintptr_t)site>>2)%DIAMOND_INLINE_CACHE_COUNT;
    DiamondExtensionCache *cache=&vm->extension_caches[slot];
    if(cache->site!=site) {
        *cache=(DiamondExtensionCache){.site=site};
        return false;
    }
    for(size_t index=0;index<cache->entry_count;index++)
        if(cache->entries[index].found&&cache->entries[index].kind==kind) {
            *out_function=cache->entries[index].function;
            return true;
        }
    return false;
}

DIAMOND_INTERNAL DiamondFieldCacheEntry *lookup_field_cached(
    DiamondVm *vm, const uint8_t *site, const DiamondInstance *instance,
    uint8_t field, bool write);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_set_ivar(DiamondVm *vm, const uint8_t *site,
        const DiamondValue *receiver, uint8_t field, const DiamondValue *value);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_get_ivar(DiamondVm *vm, const uint8_t *site,
        const DiamondValue *receiver, uint8_t field, DiamondValue *out);

static inline int named_field_index(const DiamondInstance *instance,
                             const DiamondStringConstant *name) {
    for(size_t field=0;field<instance->class->field_count;field++)
        if(strlen(instance->class->fields[field])==name->length&&
           memcmp(instance->class->fields[field],name->chars,name->length)==0)
            return (int)field;
    return -1;
}

DIAMOND_INTERNAL bool value_matches_type(const DiamondChunk *chunk, DiamondValue value,
                               uint8_t type);

DIAMOND_INTERNAL bool value_matches_set(const DiamondChunk *chunk,DiamondValue value,
                              uint16_t set_index,bool attach);

DIAMOND_INTERNAL bool array_value_satisfies_constraints(DiamondArray *array,
                                               DiamondValue value);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_index_get(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *receiver,
        const DiamondValue *index, DiamondValue *out);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_index_set(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *receiver,
        const DiamondValue *index, const DiamondValue *source);

DIAMOND_INTERNAL bool catch_exception(DiamondVm *vm,const DiamondChunk *chunk,
                            UnwindHandler *handlers,size_t *handler_count,
                            PendingUnwind *pending,DiamondValue *registers,
                            size_t *ip);

DIAMOND_INTERNAL bool catch_runtime_error(DiamondVm *vm,const DiamondChunk *chunk,
                                DiamondVmStatus status,UnwindHandler *handlers,
                                size_t *handler_count,PendingUnwind *pending,
                                DiamondValue *registers,size_t *ip);

DIAMOND_INTERNAL void format_type_set_index(char *buffer,size_t capacity,
                                  const DiamondChunk *chunk,uint16_t set_index);

static inline bool value_is_native_kind(DiamondValue value,uint8_t kind) {
    return value.kind==DIAMOND_VALUE_OBJECT&&(uint8_t)value.as.object->kind==kind;
}

DIAMOND_INTERNAL void diamond_format_value_type(char *buffer, size_t capacity,
                              DiamondValue value);

DIAMOND_INTERNAL void format_value_type_detailed(char *buffer,size_t capacity,
        DiamondValue value);

DIAMOND_INTERNAL void format_operator_type_error(DiamondVm *vm,DiamondValue left_value,
        DiamondValue right_value,const char *op_name);

DIAMOND_INTERNAL DiamondVmStatus read_line(DiamondVm *vm,FILE *stream,StringBuilder *builder,
                                 bool *saw_any);

DIAMOND_INTERNAL DiamondVmStatus float_to_int(DiamondVm *vm,double whole,const char *what,
                                    DiamondValue *out);

DIAMOND_INTERNAL void format_uncaught_exception_message(DiamondVm *vm,DiamondValue exception,
                                              bool show_origin);

DIAMOND_INTERNAL DiamondVmStatus inspect_value(DiamondVm *vm,const DiamondChunk *chunk,
                                     size_t depth,DiamondValue value,
                                     DiamondValue *out);

static inline uint8_t binding_node(DiamondTypeBinding *binding) {
    if(binding->node_count==DIAMOND_BOUND_TYPE_NODES)return UINT8_MAX;
    return binding->node_count++;
}

DIAMOND_INTERNAL void bind_context_set(DiamondTypeBinding *binding,uint8_t node,
    const DiamondChunk *context,const DiamondTypeSet *sets,uint16_t set_index);

DIAMOND_INTERNAL void infer_from_value(const DiamondChunk *chunk,DiamondValue value,
    const DiamondTypeSet *sets,uint16_t set_index,
    DiamondTypeBinding bindings[8]);

/* Split out of run_chunk's own switch (unlike GET_IVAR/GET_NAMESPACE_
 * CONSTANT and friends, which stay inline) specifically to keep run_chunk's
 * own per-call C stack frame from growing: run_chunk recurses in C for
 * every ordinary Diamond function call (DIAMOND_OP_CALL below), and
 * DIAMOND_MAX_CALL_DEPTH's whole guarantee -- catching runaway Diamond-
 * level recursion with a clean error before the real C stack does --
 * depends on that per-frame size times DIAMOND_MAX_CALL_DEPTH staying
 * safely under the OS stack limit. Confirmed empirically while adding
 * this: even a few bytes of extra locals declared directly in run_chunk's
 * own switch shifted a stack-depth regression test (tests/cases/
 * program_builder_call_declared_function.di's sibling deep-recursion
 * case) from a clean "call stack overflow" to a real ASan-caught
 * stack-overflow segfault -- that margin is thinner than it looks.
 * Locals declared inside a called helper live on the *helper's* frame,
 * popped the moment it returns, so they never accumulate across
 * DIAMOND_MAX_CALL_DEPTH levels of recursion the way a case-local do. */
static inline DiamondVmStatus get_cvar_helper(DiamondVm *vm,
        const DiamondChunk *chunk, uint16_t class_index, uint16_t slot,
        DiamondValue *out) {
    if(class_index>=chunk->class_count||
       slot>=chunk->classes[class_index].class_variable_count)
        return DIAMOND_VM_INVALID_BYTECODE;
    /* A read before any write anywhere in this VM: no slot has ever been
     * allocated, so the value is definitionally the class variable
     * default (nil) -- allocating here just to immediately read back nil
     * would be pure waste, so skip it and return the default directly. */
    if(vm->class_variables==nullptr) {
        *out=DIAMOND_NIL;
        return DIAMOND_VM_OK;
    }
    *out=vm->class_variables[(size_t)class_index*DIAMOND_MAX_FIELDS+slot];
    return DIAMOND_VM_OK;
}

static inline DiamondVmStatus set_cvar_helper(DiamondVm *vm,const DiamondChunk *chunk,
        uint16_t class_index,uint16_t slot,DiamondValue value) {
    if(class_index>=chunk->class_count||
       slot>=chunk->classes[class_index].class_variable_count)
        return DIAMOND_VM_INVALID_BYTECODE;
    if(vm->class_variables==nullptr) {
        vm->class_variables=calloc((size_t)DIAMOND_MAX_CLASSES*DIAMOND_MAX_FIELDS,
            sizeof(DiamondValue));
        if(vm->class_variables==nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
    }
    vm->class_variables[(size_t)class_index*DIAMOND_MAX_FIELDS+slot]=value;
    return DIAMOND_VM_OK;
}

DIAMOND_INTERNAL DiamondVmStatus jit_call_or_interpret(DiamondVm *vm, const DiamondFunction *function,
        const DiamondChunk *chunk_to_interpret, const DiamondValue *arguments,
        size_t argument_count, size_t depth, const DiamondClosure *closure,
        DiamondValue *result);

DIAMOND_INTERNAL DiamondVmStatus invoke_resolved_method_helper(DiamondVm *vm,
        const DiamondChunk *owner_chunk,const DiamondMethod *method,
        DiamondValue self_value,const DiamondValue *registers,uint16_t base,
        uint8_t argc,bool typed,uint8_t type_argument_count,
        const uint16_t *type_arguments,const DiamondChunk *caller_chunk,
        size_t depth,DiamondValue *result);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_super_call(DiamondVm *vm, const DiamondChunk *chunk,
        uint8_t owner_index, uint16_t name, DiamondValue *registers, uint16_t base,
        uint8_t argc, size_t depth, DiamondValue *out);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_invoke_instance(DiamondVm *vm, const DiamondChunk *chunk,
        const uint8_t *site, DiamondOpCode instruction, DiamondValue *registers,
        uint16_t recv, uint16_t name, uint16_t base, uint8_t argc,
        uint8_t type_argument_count, const uint16_t *type_arguments,
        size_t depth, DiamondValue *out);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_new_instance(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, uint16_t dest, uint8_t class_index,
        uint16_t base, uint8_t argc, size_t depth);

DIAMOND_INTERNAL DiamondVmStatus call_closure_spread_helper(DiamondVm *vm,
        const DiamondChunk *chunk,const DiamondFunction *fn,
        const DiamondClosure *called,const DiamondArray *spread,size_t depth,
        DiamondValue *result);

DIAMOND_INTERNAL bool raise_capture_backtrace_helper(DiamondVm *vm,const DiamondChunk *chunk);

typedef enum DiamondDebugCommandKind {
    DIAMOND_DEBUG_COMMAND_NONE,
    DIAMOND_DEBUG_COMMAND_CONTINUE,
    DIAMOND_DEBUG_COMMAND_SET_BREAKPOINTS,
    /* Real stepping (docs/debugging.md's own "Stepping" section) --
     * named after DAP's own request names directly (`next` is DAP's own
     * step-over request), so dap/main.c's handlers need no translation
     * table: they just write the same literal command string here
     * expects. Only ever meaningful from within debugger_structured_
     * helper's own pause/resume loop (a real DAP client only ever sends
     * these against an already-stopped debuggee, unlike setBreakpoints,
     * which legitimately arrives while running too). */
    DIAMOND_DEBUG_COMMAND_NEXT,
    DIAMOND_DEBUG_COMMAND_STEP_IN,
    DIAMOND_DEBUG_COMMAND_STEP_OUT,
} DiamondDebugCommandKind;

DIAMOND_INTERNAL bool debug_pipe_read_command(int fd,DiamondDebugCommandKind *kind,
        size_t *lines,size_t *line_count,size_t capacity,bool *are_offsets);

DIAMOND_INTERNAL DiamondVmStatus debugger_helper(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,size_t instruction_offset,size_t source_line_offset,DiamondValue *registers,
        const uint16_t *name_indices,const uint16_t *local_registers,uint8_t local_count,
        const char *reason);

typedef struct NativeKeywordSignature {
    const char *method;
    const char *parameters[3];
    uint8_t parameter_count;
} NativeKeywordSignature;

DIAMOND_INTERNAL const NativeKeywordSignature *native_keyword_signature(
        const DiamondStringConstant *method);

DIAMOND_INTERNAL DiamondVmStatus public_send_helper(DiamondVm *vm,
        const DiamondChunk *chunk,DiamondValue receiver,
        const DiamondValue *arguments,size_t argument_count,size_t depth,
        DiamondValue *result);

DIAMOND_INTERNAL DiamondVmStatus merge_native_keyword_arguments(DiamondVm *vm,
        const DiamondChunk *caller,const NativeKeywordSignature *signature,
        const DiamondArray *positional,const uint16_t *keyword_names,
        const uint16_t *keyword_registers,size_t keyword_count,
        const DiamondValue *registers,DiamondValue **merged,size_t *merged_count);

DIAMOND_INTERNAL DiamondVmStatus merge_keyword_arguments(DiamondVm *vm,
        const DiamondChunk *caller,const DiamondFunction *function,
        const DiamondArray *positional,const uint16_t *keyword_names,
        const uint16_t *keyword_registers,size_t keyword_count,
        const DiamondValue *registers,size_t public_arity,
        const DiamondValue *block,
        DiamondValue **merged,size_t *merged_count);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_dup(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_freeze(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out);

DIAMOND_INTERNAL DiamondVmStatus diamond_deep_freeze(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out);

DIAMOND_INTERNAL DiamondVmStatus diamond_jit_frozen(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out);

DIAMOND_INTERNAL size_t describe_invoke_arity_error(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondValue *registers,size_t offset);

typedef enum NonlocalOutcome {
    NONLOCAL_CONTINUE,   /* landed at a call (break) or entered an ensure block */
    NONLOCAL_RETURNED,   /* this frame returns the value (*result set) */
    NONLOCAL_PASS_ON,    /* not for this frame: keep unwinding */
    NONLOCAL_INVALID,    /* can't land here; vm->error explains */
} NonlocalOutcome;

DIAMOND_INTERNAL NonlocalOutcome nonlocal_exit_arrives(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondFrame *frame,DiamondValue *registers,UnwindHandler *handlers,
        size_t *handler_count,PendingUnwind *pending,size_t *ip,
        size_t instruction_offset,DiamondValue *result);

DIAMOND_INTERNAL DiamondVmStatus file_invoke_helper(DiamondVm *vm,
                                          const DiamondChunk *chunk, size_t depth,
                                          DiamondValue *registers, uint16_t recv, uint16_t base,
                                          uint8_t argc, uint16_t dest,
                                          const DiamondStringConstant *method_name);

DIAMOND_INTERNAL DiamondVmStatus socket_invoke_helper(DiamondVm *vm,
                                            const DiamondChunk *chunk, size_t depth,
                                            DiamondValue *registers, uint16_t recv,
                                            uint16_t base, uint8_t argc, uint16_t dest,
                                            const DiamondStringConstant *method_name);

DIAMOND_INTERNAL DiamondVmStatus fiber_invoke_helper(DiamondVm *vm,
                                           DiamondValue *registers, uint16_t recv,
                                           uint16_t base, uint8_t argc, uint16_t dest,
                                           const DiamondStringConstant *method_name);

DIAMOND_INTERNAL void set_uncaught_exception_error(DiamondVm *vm);

DIAMOND_INTERNAL DiamondVmStatus thread_invoke_helper(DiamondVm *vm,
                                            const DiamondChunk *chunk, DiamondValue *registers,
                                            uint16_t recv, uint8_t argc, uint16_t dest,
                                            const DiamondStringConstant *method_name);

DIAMOND_INTERNAL DiamondVmStatus channel_invoke_helper(DiamondVm *vm,
                                             const DiamondChunk *chunk, DiamondValue *registers,
                                             uint16_t recv, uint16_t base, uint8_t argc,
                                             uint16_t dest,
                                             const DiamondStringConstant *method_name);

DIAMOND_INTERNAL DiamondVmStatus supervisor_invoke_helper(DiamondVm *vm,
                                                const DiamondChunk *chunk,
                                                DiamondValue *registers, uint16_t recv,
                                                uint16_t base, uint8_t argc, uint16_t dest,
                                                const DiamondStringConstant *method_name);

DIAMOND_INTERNAL DiamondVmStatus listener_invoke_helper(DiamondVm *vm,
                                              const DiamondChunk *chunk, size_t depth,
                                              DiamondValue *registers, uint16_t recv,
                                              uint8_t argc, uint16_t dest,
                                              const DiamondStringConstant *method_name);

DIAMOND_INTERNAL DiamondVmStatus udp_socket_invoke_helper(DiamondVm *vm,
                                                const DiamondChunk *chunk, size_t depth,
                                                DiamondValue *registers, uint16_t recv,
                                                uint16_t base, uint8_t argc, uint16_t dest,
                                                const DiamondStringConstant *method_name);

DIAMOND_INTERNAL DiamondVmStatus tls_socket_invoke_helper(DiamondVm *vm,
                                                const DiamondChunk *chunk, size_t depth,
                                                DiamondValue *registers, uint16_t recv,
                                                uint16_t base, uint8_t argc, uint16_t dest,
                                                const DiamondStringConstant *method_name);

DIAMOND_INTERNAL DiamondVmStatus regexp_invoke_helper(DiamondVm *vm,
                                            DiamondValue *registers, uint16_t recv,
                                            uint16_t base, uint8_t argc, uint16_t dest,
                                            const DiamondStringConstant *method_name);

DIAMOND_INTERNAL DiamondVmStatus collection_invoke_helper(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,const DiamondStringConstant *method_name);

DIAMOND_INTERNAL DiamondVmStatus numeric_invoke_helper(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,const DiamondStringConstant *method_name);

DIAMOND_INTERNAL const char *diamond_vm_status_name(DiamondVmStatus status);

DIAMOND_INTERNAL DiamondVmStatus run_chunk(const DiamondChunk *chunk, DiamondVm *vm,
                                 const DiamondValue *arguments,
                                 size_t argument_count, size_t depth,
                                 const DiamondClosure *closure,
                                 DiamondValue *result);

#ifdef DIAMOND_ASAN_FIBERS
DIAMOND_INTERNAL void diamond_resume_target_bounds(const DiamondFiber *fiber,
        const void **bottom, size_t *size);
#endif

#endif
