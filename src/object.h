#ifndef DIAMOND_OBJECT_H
#define DIAMOND_OBJECT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
} DiamondObjectKind;

typedef struct DiamondObject {
    struct DiamondObject *next;
    DiamondObjectKind kind;
    bool marked;
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
        uint8_t set_index;
        const DiamondClass *classes;
        size_t class_count;
        const DiamondInterface *interfaces;
        size_t interface_count;
        DiamondTypeBinding *type_variable_bindings;
        uint8_t type_variable_count;
    } constraints[4];
    uint8_t constraint_count;
    DiamondValue *values;
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
        uint8_t key_set;
        uint8_t value_set;
        const DiamondClass *classes;
        size_t class_count;
        const DiamondInterface *interfaces;
        size_t interface_count;
        DiamondTypeBinding *type_variable_bindings;
        uint8_t type_variable_count;
    } constraints[4];
    uint8_t constraint_count;
} DiamondHash;

typedef struct DiamondClosure {
    DiamondObject object;
    uint16_t function_index;
    uint8_t capture_count;
    DiamondValue captures[16];
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

/* Forward-declared, not included: DiamondProgram is defined in compiler.h,
 * which itself includes vm.h (and so, transitively, this file) -- a
 * pointer to the incomplete type is all this struct needs. See
 * docs/roadmap.md's self-hosting Phase 1 entry for the full design: this
 * is the ProgramBuilder native bridge letting Diamond code construct and
 * run a DiamondProgram at runtime. Like DiamondFileHandle/
 * DiamondListenerHandle, no mark_object branch is needed -- the wrapped
 * DiamondProgram's own constants are restricted to scalar DiamondValues
 * (see ProgramBuilder#add_constant in vm.c), so nothing inside one ever
 * references another Diamond value. */
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
