#ifndef DIAMOND_OBJECT_H
#define DIAMOND_OBJECT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <reginold.h>

#include "value.h"

/* Forward-declared, not included: only vm.c ever calls an actual OpenSSL
 * function, so keeping <openssl/ssl.h> out of this header (included by
 * nearly every other .c file in the project) avoids dragging OpenSSL's
 * own transitive includes into everything. `ssl_st`/`ssl_ctx_st` are
 * OpenSSL's own opaque struct tags (see <openssl/types.h>), matched here
 * exactly so this typedef and OpenSSL's are the same type. */
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

/* Forward-declared because object.h is included by vm.h (src/vm.h:5),
 * not the other way around, so DiamondChunk's real definition isn't
 * visible here yet -- only ever used as an opaque pointer in this file
 * (DiamondClosure.foreign_chunk below), never dereferenced. */
typedef struct DiamondChunk DiamondChunk;

/* Forward-declared for the same reason SSL/SSL_CTX are above: only vm.c
 * calls real sqlite3 functions, so <sqlite3.h> stays out of this header.
 * Matches sqlite3.h's own `typedef struct sqlite3 sqlite3;` exactly. */
typedef struct sqlite3 sqlite3;

/* Forward-declared for the same reason sqlite3 is above: a prepared
 * statement handle, opaque here too. Matches sqlite3.h's own
 * `typedef struct sqlite3_stmt sqlite3_stmt;` exactly. */
typedef struct sqlite3_stmt sqlite3_stmt;

/* Forward-declared for the same reason sqlite3 is above: only vm.c calls
 * real libpq functions, so <libpq-fe.h> stays out of this header. Matches
 * libpq-fe.h's own `typedef struct pg_conn PGconn;` exactly. */
typedef struct pg_conn PGconn;

/* Forward-declared for the same reason PGconn is above: only vm.c calls
 * real MariaDB Connector/C (libmysqlclient-API-compatible) functions, so
 * <mysql.h> stays out of this header. Matches mysql.h's own
 * `typedef struct st_mysql MYSQL;` exactly. */
typedef struct st_mysql MYSQL;

typedef enum DiamondObjectKind : uint8_t {
    DIAMOND_OBJECT_STRING,
    DIAMOND_OBJECT_INSTANCE,
    DIAMOND_OBJECT_ARRAY,
    DIAMOND_OBJECT_HASH,
    DIAMOND_OBJECT_CLOSURE,
    DIAMOND_OBJECT_CELL,
    DIAMOND_OBJECT_FIBER,
    DIAMOND_OBJECT_FILE,
    DIAMOND_OBJECT_LISTENER,
    DIAMOND_OBJECT_SOCKET,
    DIAMOND_OBJECT_UDP_SOCKET,
    DIAMOND_OBJECT_TLS_SOCKET,
    DIAMOND_OBJECT_BIGNUM,
    DIAMOND_OBJECT_SYMBOL,
    DIAMOND_OBJECT_REGEXP,
    DIAMOND_OBJECT_PROGRAM_BUILDER,
    DIAMOND_OBJECT_THREAD,
    DIAMOND_OBJECT_SQLITE3,
    DIAMOND_OBJECT_SQLITE3_STATEMENT,
    DIAMOND_OBJECT_POSTGRES,
    DIAMOND_OBJECT_MYSQL,
    DIAMOND_OBJECT_TIME,
    DIAMOND_OBJECT_PROCESS_RESULT,
    DIAMOND_OBJECT_PROCESS_HANDLE,
    DIAMOND_OBJECT_PROCESS_STREAM,
} DiamondObjectKind;

typedef struct DiamondObject {
    struct DiamondObject *next;
    DiamondObjectKind kind;
    bool marked;
    /* Generation: false = nursery (vm->young_objects), true = tenured
     * (vm->old_objects). Every allocate_* helper always allocates young;
     * an object is promoted (list-spliced from young_objects onto
     * old_objects, this bit flipped true) the moment it survives one
     * minor collection -- no age counter, since promotion is free for a
     * non-moving collector (see docs/design.md's "Generational garbage
     * collection" section). */
    bool old;
    /* True once this (necessarily old) object is already present in
     * vm->remembered_set -- an old object gaining a reference to a young
     * one is exactly the case a minor collection's own root walk
     * wouldn't otherwise see, so the write barrier (src/vm.c's
     * gc_write_barrier) adds it to the remembered set and sets this the
     * first time that happens, so later writes to the same object don't
     * add duplicate entries. Sticky for the object's whole lifetime as
     * an old object -- see gc_write_barrier's own comment for why this
     * deliberately never gets cleared except when the object itself
     * dies. Meaningless (always false) for a young object. */
    bool remembered;
} DiamondObject;

typedef struct DiamondString {
    DiamondObject object;
    size_t length;
    char chars[];
} DiamondString;

/* An interned-free, content-compared name: equality and hashing are by
 * byte content (exactly like DiamondString), and lifetime is ordinary GC,
 * not permanent -- see docs/roadmap.md for why Symbol was scoped this way
 * rather than as Ruby-style pointer-equal singletons. Deliberately its own
 * struct (not just DiamondString reused under a different kind) purely for
 * type clarity, even though the layout is identical. */
typedef struct DiamondSymbol {
    DiamondObject object;
    size_t length;
    char chars[];
} DiamondSymbol;

/* An Int that overflowed int64_t. Sign + magnitude, base 10^9 limbs
 * (each 0..999999999), little-endian (limbs[0] is least significant) --
 * chosen over a binary base specifically to make decimal stringification
 * (the operation a bignum actually exercises on every puts/interpolation/
 * error message) close to trivial, at an acceptable cost to raw
 * arithmetic throughput that doesn't matter on this cold path. Never
 * represents a value that fits in int64_t -- every operation that
 * produces a bignum result canonicalizes back to a plain DIAMOND_VALUE_INT
 * when it fits, so downstream code never has two representations of the
 * same value to worry about. */
typedef struct DiamondBignum {
    DiamondObject object;
    bool negative;
    size_t limb_count;
    uint32_t limbs[];
} DiamondBignum;

typedef struct DiamondClass DiamondClass;
typedef struct DiamondShape DiamondShape;
typedef struct DiamondTypeSet DiamondTypeSet;
typedef struct DiamondInterface DiamondInterface;
typedef struct DiamondChunk DiamondChunk;

enum { DIAMOND_BOUND_TYPE_NODES=16,DIAMOND_BOUND_TYPE_MEMBERS=8 };
typedef struct DiamondBoundTypeMember {
    uint8_t id;
    uint8_t argument_node;
    uint8_t second_argument_node;
} DiamondBoundTypeMember;
typedef struct DiamondBoundTypeNode {
    DiamondBoundTypeMember members[DIAMOND_BOUND_TYPE_MEMBERS];
    uint8_t count;
} DiamondBoundTypeNode;
typedef struct DiamondTypeBinding {
    DiamondBoundTypeNode nodes[DIAMOND_BOUND_TYPE_NODES];
    uint8_t node_count;
} DiamondTypeBinding;

typedef struct DiamondInstance {
    DiamondObject object;
    const DiamondClass *class;
    const DiamondShape *shape;
    /* nullptr (the ordinary case) means `class` (and its own superclass
     * chain / method function indices) resolve against whatever chunk is
     * ambient at each dispatch site, same as before this field existed --
     * safe because every DiamondChunk anywhere in a single program's own
     * call tree, however transient the *struct* wrapping them is (many
     * are stack-local to one run_chunk call), always carries the same
     * underlying functions/classes table pointers. Only ever non-null for
     * an instance that crossed a ProgramBuilder#run boundary
     * (copy_value_into_vm, src/vm.c): its class lives in a separately
     * adopted foreign DiamondProgram, so dispatch needs a *stable*
     * pointer to that program's own chunk (DiamondVm.adopted_programs,
     * src/vm.h) instead of the ambient one, which by then belongs to a
     * different program entirely. Every dispatch site that reads this
     * must fall back to its own ambient chunk when it's nullptr -- never
     * assume non-null. */
    const DiamondChunk *owner;
    size_t field_count;
    DiamondValue fields[];
} DiamondInstance;

typedef struct DiamondArray {
    DiamondObject object;
    size_t count;
    size_t capacity;
    struct {
        const DiamondTypeSet *type_sets;
        size_t type_set_count;
        uint16_t set_index;
        const DiamondClass *classes;
        size_t class_count;
        const DiamondInterface *interfaces;
        size_t interface_count;
        DiamondTypeBinding *type_variable_bindings;
        uint8_t type_variable_count;
    } constraints[4];
    uint8_t constraint_count;
    DiamondValue *values;
    /* Generational GC card table: one byte per DIAMOND_GC_CARD_SIZE-
     * element card of `values`, set by the write barrier whenever an
     * index inside that card is written while this array is old, and
     * cleared again once a minor collection has scanned it. nullptr
     * until this array's first index write after promotion (see
     * mark_card_dirty, src/vm.c) -- most arrays are never promoted, and
     * of those that are, most are never mutated again, so this stays
     * unallocated in the common case. dirty_card_capacity is the number
     * of bytes currently backing dirty_cards (grown in lockstep with
     * `capacity` at allocate_array/array_push, but only once already
     * allocated -- see grow_dirty_cards), not necessarily the number
     * actually needed for the current `capacity` at every instant. */
    uint8_t *dirty_cards;
    size_t dirty_card_capacity;
} DiamondArray;

typedef struct DiamondHashEntry {
    DiamondValue key;
    DiamondValue value;
    uint64_t hash;
} DiamondHashEntry;

typedef struct DiamondHash {
    DiamondObject object;
    size_t count;
    size_t capacity;
    DiamondHashEntry *entries;
    size_t *buckets;
    size_t bucket_capacity;
    struct {
        const DiamondTypeSet *type_sets;
        size_t type_set_count;
        uint16_t key_set;
        uint16_t value_set;
        const DiamondClass *classes;
        size_t class_count;
        const DiamondInterface *interfaces;
        size_t interface_count;
        DiamondTypeBinding *type_variable_bindings;
        uint8_t type_variable_count;
    } constraints[4];
    uint8_t constraint_count;
    /* Same scheme as DiamondArray's own dirty_cards -- one card per
     * DIAMOND_GC_CARD_SIZE entries of `entries`, keyed by entry index
     * (not bucket index -- hash_rehash only ever rebuilds `buckets`, it
     * never moves `entries` around, so entry-index-to-card mapping stays
     * valid across a rehash). See DiamondArray's own comment. */
    uint8_t *dirty_cards;
    size_t dirty_card_capacity;
} DiamondHash;

typedef struct DiamondClosure {
    DiamondObject object;
    uint16_t function_index;
    uint8_t capture_count;
    /* True for a source `do ... end` block, false for named closures and
     * function references. Preserved across forwarding and Thread cloning so
     * variadic prologues can distinguish an optional block from a final rest
     * argument without overloading positional value kinds. */
    bool is_block;
    DiamondValue captures[16];
    /* Non-null only for a value returned by ClassName.compile_method
     * (src/vm.c's DIAMOND_OP_COMPILE_METHOD) -- function_index above is
     * relative to *this* chunk, not whichever chunk is ambient when the
     * closure is later consumed (e.g. by ClassName.define_method). A
     * non-owning reference: lifetime is held by vm->adopted_programs
     * (the same mechanism ProgramBuilder's own adopt mode already uses),
     * not by this closure, so no GC-tracing change is needed here.
     * nullptr for every ordinary closure Diamond source code creates. */
    const DiamondChunk *foreign_chunk;
    /* Non-null exactly when foreign_chunk is: the DiamondClass entry (in
     * the *caller's* own chunk, i.e. an ordinary stable pointer, not a
     * foreign one) compile_method captured its field snapshot from.
     * define_method compares this by pointer identity against its own
     * class_operand to reject installing a method compiled for one
     * class onto a different one -- compile_method's own field-count
     * validation only proves the body is safe for *this* class's field
     * layout, not any other. */
    const DiamondClass *intended_class;
    /* compile_method's optional bound_values: Hash argument -- already-
     * evaluated runtime values (e.g. Book.repository(), evaluated in the
     * *caller's* own chunk, where "Book" actually resolves) spliced in as
     * extra trailing parameters of the synthesized method, so body_source
     * can reference them as ordinary local names without needing to name
     * another class itself (which the synthesized source's own isolated
     * compile can never resolve -- see docs/design.md's "Runtime method
     * synthesis" section). Points into the owning DiamondAdoptedProgram's
     * own bound_values array (src/vm.c) -- permanent, vm-lifetime storage
     * this closure doesn't own, same non-owning-reference shape as
     * foreign_chunk above, but unlike foreign_chunk this one *does* need
     * GC tracing (diamond_vm_collect walks vm->adopted_programs directly
     * for this, not through this closure -- see that function's own
     * comment). bound_value_count is 0/nullptr for every ordinary
     * closure and for a compile_method result with no bound_values. */
    const DiamondValue *bound_values;
    uint8_t bound_value_count;
} DiamondClosure;

typedef struct DiamondCell {
    DiamondObject object;
    DiamondValue value;
} DiamondCell;

typedef struct DiamondFiber DiamondFiber;

typedef struct DiamondFiberHandle {
    DiamondObject object;
    DiamondFiber *fiber;
} DiamondFiberHandle;

/* Split the same way DiamondFiberHandle/DiamondFiber are: a thin GC handle
 * wrapping a heavier native struct (real OS thread handle, its own fully
 * isolated DiamondVm + cloned DiamondProgram, pthread synchronization) --
 * see docs/threads.md. DiamondThread itself is defined in src/vm.c, where
 * pthread.h is already reachable and the rest of its native-resource
 * cousins (DiamondFiber, DiamondRegexp's handle) already live. */
typedef struct DiamondThread DiamondThread;

typedef struct DiamondThreadHandle {
    DiamondObject object;
    DiamondThread *thread;
} DiamondThreadHandle;

typedef struct DiamondFileHandle {
    DiamondObject object;
    FILE *stream;
} DiamondFileHandle;

typedef struct DiamondListenerHandle {
    DiamondObject object;
    int fd;
    /* Set only by TCPServer.listen_nonblocking -- an ordinary
     * TCPServer.listen listener is unaffected (false), and .accept()
     * branches on this to decide whether "no pending connection" is an
     * IOError (blocking listener: can't happen, accept() itself blocks
     * until one exists) or a plain nil return (nonblocking listener: the
     * normal "nothing to accept right now" case a poll-driven caller
     * expects to see routinely). */
    bool nonblocking;
    /* Non-null only for a TLSServer.listen listener (nullptr for an
     * ordinary TCPServer.listen/listen_nonblocking one) -- holds the
     * server certificate/key TLSServer.listen loaded, reused for every
     * .accept()'s own SSL_new so the potentially-expensive cert/key
     * parsing happens once per listener, not once per connection. Freed
     * on .close() and at GC/VM-teardown sweep; safe to free while
     * already-accepted TLS sockets are still alive, since SSL_new gives
     * each of them their own reference-counted hold on it. */
    SSL_CTX *tls_context;
} DiamondListenerHandle;

/* A non-blocking TCP connection, returned only by .accept() on a
 * TCPServer.listen_nonblocking listener -- deliberately not
 * DiamondFileHandle's buffered FILE*, since libc stdio buffering and
 * EAGAIN don't mix cleanly (a short buffered read can silently swallow
 * the "nothing available yet" signal a poll-driven caller needs to see
 * on every call, not just the first). .read(n)/.write(value) are raw
 * read(2)/write(2) against `fd` directly; see docs/io.md. */
typedef struct DiamondSocketHandle {
    DiamondObject object;
    int fd;
} DiamondSocketHandle;

/* UDP is connectionless -- one socket both sends and receives, to/from
 * whatever address each individual call names, so unlike DiamondSocketHandle
 * this isn't paired with a listener/is never produced by anything but its
 * own two constructors (UDPSocket.bind/UDPSocket.open, see docs/io.md).
 * Also a raw fd, not a FILE*: sendto(2)/recvfrom(2) need the peer address
 * on every call, which buffered stdio read/write has no way to carry. */
typedef struct DiamondUdpSocketHandle {
    DiamondObject object;
    int fd;
} DiamondUdpSocketHandle;

/* A TLS connection -- both the client side (TLSSocket.connect) and each
 * server-side accepted connection (TLSServer.listen's .accept()) use
 * this same handle. Like DiamondSocketHandle, a raw fd rather than a
 * FILE*: SSL_read/SSL_write need to own the fd's I/O directly, so mixing
 * in libc stdio buffering underneath them would be actively wrong, not
 * just redundant. `ssl` is the per-connection OpenSSL session object;
 * .close() and GC sweep all call SSL_shutdown(ssl) (best-effort, one
 * call, result ignored -- this only sends this side's own close_notify
 * alert; waiting for the peer's own close_notify back would mean
 * blocking on a peer that may never send one) then SSL_free(ssl) then
 * close(fd) -- SSL_free alone sends nothing on its own, and SSL_set_fd
 * wraps `fd` in a BIO_NOCLOSE socket BIO, so SSL_free never closes it
 * either; both steps are genuinely required, not defensive redundancy. */
typedef struct DiamondTlsSocketHandle {
    DiamondObject object;
    SSL *ssl;
    int fd;
} DiamondTlsSocketHandle;

/* A compiled reginold pattern. Unlike DiamondFileHandle/DiamondListenerHandle,
 * this owns no OS resource (fd/socket) -- just heap memory reginold itself
 * allocated -- so there's no explicit .close() method; GC-time
 * reginold_regex_free (see diamond_vm_collect's sweep loop) is sufficient. */
typedef struct DiamondRegexp {
    DiamondObject object;
    reginold_regex *handle;
} DiamondRegexp;

/* Unlike DiamondRegexp, this *does* own a real OS resource (an open
 * database file, via sqlite3_open) -- closer in shape to
 * DiamondFileHandle than to DiamondRegexp. `db` is nulled by an explicit
 * #close() the same way DiamondFileHandle's `stream` is; both GC sweep
 * and VM teardown check for that sentinel before calling sqlite3_close,
 * making repeated/GC-time close idempotent. */
typedef struct DiamondSqlite3Handle {
    DiamondObject object;
    sqlite3 *db;
} DiamondSqlite3Handle;

/* A reusable prepared statement, from `SQLite3#prepare`. Same idempotent-
 * close sentinel discipline as DiamondSqlite3Handle: `stmt` is nulled by
 * an explicit #close() and checked before any other operation; both GC
 * sweep and VM teardown finalize a still-open statement as a safety net.
 * Deliberately holds no back-reference to the owning DiamondSqlite3Handle
 * -- sqlite3_db_handle(stmt) already recovers the owning `sqlite3*` for
 * error messages, and a prepared statement's lifetime isn't tied to its
 * connection's Diamond-level object staying reachable (matching sqlite3's
 * own C-level contract: a statement is only actually invalidated when the
 * connection is closed, at which point every operation on it below
 * already checks the closed-connection sentinel via handle->db first). */
typedef struct DiamondSqlite3StatementHandle {
    DiamondObject object;
    sqlite3_stmt *stmt;
} DiamondSqlite3StatementHandle;

/* Same shape and same idempotent-close reasoning as DiamondSqlite3Handle
 * immediately above, just wrapping a libpq PGconn* (via PQconnectdb)
 * instead of a sqlite3*. `conn` is nulled by an explicit #close() and
 * checked before any other operation; both GC sweep and VM teardown check
 * that sentinel before calling PQfinish. */
typedef struct DiamondPostgresHandle {
    DiamondObject object;
    PGconn *conn;
} DiamondPostgresHandle;

/* Same shape and same idempotent-close reasoning as DiamondPostgresHandle
 * immediately above, just wrapping a MYSQL* (via mysql_init +
 * mysql_real_connect) instead of a PGconn*. `conn` is nulled by an
 * explicit #close() and checked before any other operation; both GC
 * sweep and VM teardown check that sentinel before calling mysql_close. */
typedef struct DiamondMysqlHandle {
    DiamondObject object;
    MYSQL *conn;
} DiamondMysqlHandle;

/* Simpler still than DiamondRegexp: owns no OS resource and no second
 * allocation either -- just two scalars. Freeing one is `free(pointer)`,
 * nothing else, and there's no mark case since neither field is a
 * DiamondValue. `epoch` (seconds since the Unix epoch, fractional) is
 * the single source of truth; `utc` only chooses gmtime_r vs localtime_r
 * for component accessors/strftime -- it never affects equality,
 * ordering, or arithmetic (see values_equal/hash_value and the ADD/
 * SUBTRACT/comparison opcode handlers in vm.c). */
typedef struct DiamondTime {
    DiamondObject object;
    double epoch;
    bool utc;
} DiamondTime;

/* Process.run(argv)'s result: captured stdout/stderr (as real DiamondValue
 * Strings, so they're ordinary GC-managed objects mark_object already
 * knows how to trace once DIAMOND_OBJECT_PROCESS_RESULT has its own
 * branch there -- see vm.c) plus the child's exit code. No owned OS
 * resource by the time this exists: the pipes are fully drained and
 * closed and the child already reaped (waitpid) before
 * process_run_helper ever returns one, so freeing this is just
 * free(pointer), same as DiamondTime. */
typedef struct DiamondProcessResult {
    DiamondObject object;
    DiamondValue stdout_value;
    DiamondValue stderr_value;
    int64_t exit_code;
} DiamondProcessResult;

/* Process.spawn(argv)'s stdout/stderr streams: the *read end* of a pipe
 * to a still-possibly-running child, set O_NONBLOCK right after
 * posix_spawn (same reasoning DiamondSocketHandle's own comment gives --
 * a poll-driven caller needs EAGAIN to actually mean "nothing yet", not
 * silently be swallowed by libc stdio buffering), so this is a raw fd
 * with its own read(2)/close(2) dispatch, not DiamondFileHandle's
 * buffered FILE*. Also why this can be an IO.poll target the same way a
 * DiamondSocketHandle already is (see pollable_fd, vm.c) -- polling
 * genuinely needs the same fd-level readiness poll(2) itself checks. */
typedef struct DiamondProcessStream {
    DiamondObject object;
    int fd;
} DiamondProcessStream;

/* Process.spawn(argv)'s live handle -- unlike DiamondProcessResult
 * (already-finished, output already captured, child already reaped),
 * this represents a child that may still be running: `pid` stays valid
 * until `reaped` (set by #wait or a #running? that observes exit),
 * `stdout_stream`/`stderr_stream` are each a DiamondProcessStream the
 * caller drains (optionally via IO.poll) independently of waiting for
 * exit. `reaped`/`exit_code` cache the one waitpid() call this handle
 * is allowed to make -- calling waitpid a second time on an already-
 * reaped pid would either block forever (no such child) or, worse, race
 * against the OS having already recycled that pid for an unrelated
 * process, so every dispatch path funnels through the same "have we
 * already reaped this?" check. */
typedef struct DiamondProcessHandle {
    DiamondObject object;
    pid_t pid;
    DiamondValue stdout_stream;
    DiamondValue stderr_stream;
    bool reaped;
    int64_t exit_code;
} DiamondProcessHandle;

/* Forward-declared, not included: DiamondProgram is defined in compiler.h,
 * which itself includes vm.h (and so, transitively, this file) -- a
 * pointer to the incomplete type is all this struct needs. See
 * docs/roadmap.md's self-hosting Phase 1 entry for the full design: this
 * is the ProgramBuilder native bridge letting Diamond code construct and
 * run a DiamondProgram at runtime. Like DiamondFileHandle/
 * DiamondListenerHandle, no mark_object branch is needed -- the wrapped
 * DiamondProgram's own constants are restricted to scalar DiamondValues
 * (see ProgramBuilder#add_constant in vm.c), so nothing inside one ever
 * references another Diamond value.
 *
 * Trust model (settled after the pre-release audit that added
 * diamond_verify_bytecode's register-bounds/jump-alignment checks,
 * program_builder_run_helper in vm.c): ProgramBuilder is an internal
 * mechanism the self-hosted compiler bootstrap needs, not a supported
 * embedding API -- it's exactly what docs/roadmap.md's "Explicitly
 * deferred" section means by deferring stable bytecode/embedding APIs.
 * It's still callable from any Diamond script (there's no way to hide a
 * builtin class from `require`d code), so the bounds/alignment
 * validation stays as real defense-in-depth against memory-unsafety --
 * but there's deliberately no resource-limit enforcement (max code
 * size, register count, construction time) and no API stability
 * promise. A future decision to expose this as a real embedding surface
 * should treat that as a new feature with its own hardening pass, not
 * an incremental extension of what's here today. */
typedef struct DiamondProgram DiamondProgram;
typedef struct DiamondSourceBundle DiamondSourceBundle;
typedef struct DiamondProgramBuilder {
    DiamondObject object;
    DiamondProgram *program;
    DiamondSourceBundle *source_bundle;
    uint32_t source_line;
    uint32_t source_column;
} DiamondProgramBuilder;

#endif
