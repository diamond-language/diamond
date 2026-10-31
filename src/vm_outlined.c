#include "vm_internal.h"
#include "bignum.h"
#include "compiler.h"
#include "jit.h"
#include "tls_crypto.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef DIAMOND_NO_SQLITE
#include <sqlite3.h>
#endif
#ifndef DIAMOND_NO_POSTGRES
#include <libpq-fe.h>
#endif
#ifndef DIAMOND_NO_MYSQL
#include <mysql.h>
#endif

extern char **environ;

#ifndef DIAMOND_NO_MYSQL
/* MySQL.open on a supervised attempt connects on a helper thread so the attempt
 * can be interrupted while the connector blocks in its connect or handshake
 * (docs/threads.md). `done` is 0 while the thread runs, 1 once it has finished
 * and the attempt will collect the result, 2 once the attempt gave up: the
 * thread then closes the connection and frees the job itself. The connector's
 * own non-blocking API was tried first and failed on aarch64 without setting
 * an error, so this does not depend on it. */
typedef struct MysqlConnectJob {
    pthread_t thread;
    MYSQL *conn;
    char *host,*user,*password,*database;
    unsigned int port;
    MYSQL *result;
    atomic_int done;
} MysqlConnectJob;

static void mysql_connect_job_free(MysqlConnectJob *job) {
    free(job->host);free(job->user);free(job->password);free(job->database);
    free(job);
}

static void *mysql_connect_thread(void *argument) {
    MysqlConnectJob *job=argument;
    job->result=mysql_real_connect(job->conn,job->host,job->user,job->password,
        job->database,job->port,nullptr,0);
    int running=0;
    if(!atomic_compare_exchange_strong(&job->done,&running,1)) {
        /* Abandoned: nobody will collect this. */
        mysql_close(job->result!=nullptr?job->result:job->conn);
        mysql_connect_job_free(job);
    }
    return nullptr;
}
#endif

/* The macros the outlined case bodies below were written with. run_chunk defines its own, with the same
 * names, further down: there they unwind through its handlers, here they return the status and the
 * caller applies VM_PROPAGATE. HELPER_DONE is the case's normal `break`. */
#define VM_RETURN(status_) return (status_)
#define VM_PROPAGATE(status_)                                      \
    do { const DiamondVmStatus propagated_=(status_);              \
         if(propagated_!=DIAMOND_VM_OK)return propagated_; } while (false)
/* run_chunk's VM_PROPAGATE_SIGNAL only differs from its VM_PROPAGATE inside a nested loop; returning
 * leaves every loop, so here the two are the same. */
#define VM_PROPAGATE_SIGNAL(status_) VM_PROPAGATE(status_)
#define VM_REJECT_TYPE_ARGUMENTS(method_name_)                            \
    do {                                                                  \
        snprintf(vm->error,sizeof vm->error,                              \
            "'%.*s' does not accept generic type arguments",              \
            (int)(method_name_)->length,(method_name_)->chars);           \
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);                                 \
    } while (false)
#define VM_SANDBOX_GUARD(capability_name_, category_)                     \
    do {                                                                  \
        if (getenv("DIAMOND_SANDBOX") != nullptr &&                      \
            !sandbox_category_allowed(category_)) {                       \
            snprintf(vm->error,sizeof vm->error,                          \
                "sandbox denies %s",(capability_name_));                  \
            return DIAMOND_VM_SANDBOX_ERROR;                              \
        }                                                                 \
    } while (false)
#define READ_BYTE(target_)                                  \
    do {                                                    \
        if (ip >= chunk->code_count)return DIAMOND_VM_INVALID_BYTECODE; \
        (target_) = chunk->code[ip++];                      \
    } while (false)
#define READ_SHORT(target_)                                 \
    do {                                                    \
        if (ip + 1 >= chunk->code_count)return DIAMOND_VM_INVALID_BYTECODE; \
        (target_) = (uint16_t)(((unsigned)chunk->code[ip] << 8) | chunk->code[ip + 1]); \
        ip += 2;                                            \
    } while (false)
#define HELPER_DONE do { *ip_ref=ip; return DIAMOND_VM_OK; } while (false)


/* Creates and configures TLSSocket.connect's client SSL_CTX from its options: peer verification
 * against the system or a custom trust store, an optional client certificate, ALPN and client-side
 * session caching. On failure frees the context and returns the status. Part of
 * outlined_tls_connect. */
static DiamondVmStatus tls_client_context_helper(DiamondVm *vm,const DiamondSocketConnectOptions *options,
        SSL_CTX **context_out) {
    SSL_CTX *context=SSL_CTX_new(TLS_client_method());
    if(context==nullptr) {
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,"cannot create TLS context: %s",detail);
        VM_RETURN(DIAMOND_VM_IO_ERROR);
    }
    /* Secure by default, no opt-out exposed: every
     * TLSSocket.connect verifies the peer's certificate
     * (SSL_VERIFY_PEER) and that the certificate is actually
     * for `host` (SSL_set1_host below) -- there is still no
     * `verify: false`-style escape hatch. The trust anchor
     * itself, though, is now a real option: `ca_file`/
     * `ca_path` load a caller-supplied CA bundle/directory
     * (SSL_CTX_load_verify_locations) instead of the system
     * trust store, for talking to an internal CA or a
     * hermetic test server. Neither given still means the
     * original, unchanged default (SSL_CTX_set_default_
     * verify_paths, honoring $SSL_CERT_FILE/$SSL_CERT_DIR). */
    SSL_CTX_set_verify(context,SSL_VERIFY_PEER,nullptr);
    if(options->ca_file!=nullptr||options->ca_path!=nullptr) {
        if(SSL_CTX_load_verify_locations(context,
                options->ca_file!=nullptr?options->ca_file->chars:nullptr,
                options->ca_path!=nullptr?options->ca_path->chars:nullptr)!=1) {
            char detail[256];tls_format_error(detail,sizeof detail);
            snprintf(vm->error,sizeof vm->error,
                "cannot load custom TLS trust store: %s",detail);
            SSL_CTX_free(context);
            VM_RETURN(DIAMOND_VM_IO_ERROR);
        }
    } else if(SSL_CTX_set_default_verify_paths(context)!=1) {
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,
            "cannot load system TLS trust store: %s",detail);
        SSL_CTX_free(context);
        VM_RETURN(DIAMOND_VM_IO_ERROR);
    }
    /* Mutual TLS: a client certificate/key pair presented to
     * a server that requests one (RECOMMENDED: only offered
     * when the server asks, per normal TLS negotiation --
     * this does not force client-cert auth on a server that
     * doesn't want it). Pairing already validated above. */
    if(options->cert!=nullptr) {
        if(SSL_CTX_use_certificate_chain_file(context,options->cert->chars)!=1||
           SSL_CTX_use_PrivateKey_file(context,options->key->chars,
               SSL_FILETYPE_PEM)!=1||
           SSL_CTX_check_private_key(context)!=1) {
            char detail[256];tls_format_error(detail,sizeof detail);
            snprintf(vm->error,sizeof vm->error,
                "cannot load TLS client certificate/key: %s",detail);
            SSL_CTX_free(context);
            VM_RETURN(DIAMOND_VM_IO_ERROR);
        }
    }
    /* ALPN protocol offer, encoded and handed to the context
     * before SSL_new -- see tls_encode_alpn_protocols. */
    if(options->alpn!=nullptr) {
        unsigned char *alpn_encoded=nullptr;unsigned int alpn_encoded_length=0;
        const DiamondVmStatus alpn_status=tls_encode_alpn_protocols(vm,
            "TLSSocket.connect",options->alpn,&alpn_encoded,&alpn_encoded_length);
        if(alpn_status!=DIAMOND_VM_OK) {
            SSL_CTX_free(context);
            VM_RETURN(alpn_status);
        }
        /* SSL_CTX_set_alpn_protos copies the buffer internally
         * -- freed right after the call regardless of outcome,
         * matching every OpenSSL client example's own usage. */
        const int alpn_set_status=
            SSL_CTX_set_alpn_protos(context,alpn_encoded,alpn_encoded_length);
        free(alpn_encoded);
        if(alpn_set_status!=0) {
            char detail[256];tls_format_error(detail,sizeof detail);
            snprintf(vm->error,sizeof vm->error,
                "cannot set ALPN protocols: %s",detail);
            SSL_CTX_free(context);
            VM_RETURN(DIAMOND_VM_IO_ERROR);
        }
    }
    /* Client-side session caching, needed for
     * tls_new_session_callback to ever fire at all -- the
     * default mode is server-side caching only, even on a
     * context created with TLS_client_method(). Registered
     * unconditionally (not just when `session` was passed in)
     * since #session should work for capturing a *new*
     * session to reuse on a future connection too, not only
     * for resuming one given here. */
    SSL_CTX_set_session_cache_mode(context,SSL_SESS_CACHE_CLIENT);
    SSL_CTX_sess_set_new_cb(context,tls_new_session_callback);
    *context_out=context;
    return DIAMOND_VM_OK;
}

/* DIAMOND_OP_TLS_CONNECT, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_tls_connect(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        uint8_t instruction,size_t *ip_ref) {
    size_t ip=*ip_ref;
    const bool start_handshake=instruction==DIAMOND_OP_TLS_START_HANDSHAKE;
    uint16_t dest=0,host_reg=0,port_reg=0,options_reg=0;
    READ_SHORT(dest);READ_SHORT(host_reg);READ_SHORT(port_reg);
    READ_SHORT(options_reg);
    VM_SANDBOX_GUARD("TLSSocket.connect", "network");
    DiamondSocketHandle *tcp=nullptr;
    if(start_handshake) {
        if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
           registers[host_reg].as.object->kind!=DIAMOND_OBJECT_SOCKET) {
            snprintf(vm->error,sizeof vm->error,"start_handshake expects a connected Socket");
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
        tcp=(DiamondSocketHandle *)registers[host_reg].as.object;
        if(tcp->fd<0||tcp->connecting) {
            snprintf(vm->error,sizeof vm->error,"start_handshake requires a completed TCP connection");
            VM_RETURN(DIAMOND_VM_IO_ERROR);
        }
        host_reg=port_reg;
    }
    if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[host_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
       (!start_handshake&&registers[port_reg].kind!=DIAMOND_VALUE_INT)) {
        snprintf(vm->error,sizeof vm->error,
                 "TLSSocket.connect arguments must be a String host and an Int port");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    DiamondSocketConnectOptions options;
    const DiamondVmStatus options_status=parse_socket_connect_options_helper(
        vm,registers[options_reg],true,&options);
    VM_PROPAGATE(options_status);
    if(start_handshake&&options.connect_timeout_ms>=0) {
        snprintf(vm->error,sizeof vm->error,"start_handshake uses caller readiness/deadlines, not connect_timeout_ms");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    /* Checked before ever touching the network: an incomplete
     * cert/key pair is a caller mistake regardless of whether
     * the connection attempt would otherwise succeed. */
    if((options.cert!=nullptr)!=(options.key!=nullptr)) {
        snprintf(vm->error,sizeof vm->error,
            "TLSSocket.connect: 'cert' and 'key' must be given together");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    /* Same "cheap input, check before the network" reasoning
     * as the cert/key pairing check just above -- the actual
     * wire-format encoding still happens later, once a TLS
     * context exists (see tls_encode_alpn_protocols below). */
    if(options.alpn!=nullptr) {
        const DiamondVmStatus alpn_validate_status=
            tls_validate_alpn_protocols(vm,"TLSSocket.connect",options.alpn);
        VM_PROPAGATE(alpn_validate_status);
    }
    const DiamondString *host=(const DiamondString *)registers[host_reg].as.object;
    int connected_fd=-1;
    if(host->length==0||memchr(host->chars,'\0',host->length)!=nullptr) {
        snprintf(vm->error,sizeof vm->error,"TLS hostname must be nonempty and contain no NUL");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(start_handshake) {
        connected_fd=tcp->fd;tcp->fd=-1; /* ownership moves exactly once */
    } else {
        const DiamondVmStatus connect_status=tcp_connect_helper(vm,host,
            registers[port_reg].as.integer,options.connect_timeout_ms,
        options.recv_buffer_bytes,options.send_buffer_bytes,&connected_fd);
        VM_PROPAGATE(connect_status);
    }
    const DiamondVmStatus timeout_status=apply_socket_timeouts_helper(vm,
        connected_fd,options.read_timeout_ms,options.write_timeout_ms);
    if(timeout_status!=DIAMOND_VM_OK) {close(connected_fd);VM_RETURN(timeout_status);}
    SSL_CTX *context=nullptr;
    const DiamondVmStatus context_status=tls_client_context_helper(vm,&options,&context);
    if(context_status!=DIAMOND_VM_OK) {close(connected_fd);VM_RETURN(context_status);}
    SSL *ssl=SSL_new(context);
    SSL_CTX_free(context); /* ssl already holds its own reference */
    if(ssl==nullptr) {
        close(connected_fd);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    DiamondTlsSocketHandle *handle=
        allocate_tls_socket_handle(vm,ssl,connected_fd);
    if(handle==nullptr) {
        SSL_free(ssl);close(connected_fd);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    /* Rooted immediately, and app_data wired up before
     * anything that might trigger tls_new_session_callback --
     * a TLS <=1.2 session is commonly established
     * synchronously inside SSL_connect itself, not only via a
     * later TLS 1.3 post-handshake ticket, so the correlation
     * needs to already work before SSL_connect is even
     * called. Every failure path below this point
     * deliberately does NOT free ssl/close connected_fd
     * directly any more -- `handle` already owns both, and
     * letting the ordinary sweep-time cleanup (or an explicit
     * #close) do it once, later, avoids a double free through
     * this now-reachable object's own fields. */
    registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
        .as.object=(DiamondObject *)handle};
    handle->handshake_pending=start_handshake;
    SSL_set_app_data(ssl,handle);
    if(SSL_set_fd(ssl,connected_fd)!=1) {
        tls_abort_handshake(handle);VM_RETURN(DIAMOND_VM_IO_ERROR);
    }
    /* SNI (which certificate a multi-tenant server presents)
     * and the hostname check SSL_get_verify_result below
     * relies on both need a null-terminated hostname --
     * host->chars always is (see allocate_string). */
    struct in6_addr numeric_host;
    const bool is_ip=inet_pton(AF_INET,host->chars,&numeric_host)==1||
        inet_pton(AF_INET6,host->chars,&numeric_host)==1;
    const int identity_status=is_ip?
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl),host->chars):
        SSL_set1_host(ssl,host->chars);
    if(identity_status!=1||(!is_ip&&SSL_set_tlsext_host_name(ssl,host->chars)!=1)) {
        tls_abort_handshake(handle);VM_RETURN(DIAMOND_VM_IO_ERROR);
    }
    /* Session resumption is always best-effort: a `session`
     * blob that fails to parse (corrupt, or from an
     * incompatible OpenSSL build/rotated ticket key) is
     * silently ignored rather than an error -- TLS itself
     * transparently falls back to a full handshake when
     * resumption doesn't happen, exactly the same outcome as
     * if this option had never been given at all.
     * #session_reused? tells the caller which actually
     * happened. */
    if(options.session!=nullptr) {
        const unsigned char *session_cursor=
            (const unsigned char *)options.session->chars;
        SSL_SESSION *resume_session=d2i_SSL_SESSION(nullptr,&session_cursor,
            (long)options.session->length);
        if(resume_session!=nullptr) {
            SSL_set_session(ssl,resume_session);
            SSL_SESSION_free(resume_session); /* SSL_set_session took its own ref */
        }
    }
    if(start_handshake)HELPER_DONE;
    int connect_result=0;
    const DiamondVmStatus connect_status=
        tls_handshake_interruptible(vm,ssl,false,&connect_result);
    if(connect_status!=DIAMOND_VM_OK)VM_RETURN(connect_status);
    if(connect_result!=1) {
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,
            "cannot connect to '%.*s' over TLS: %s",
            (int)host->length,host->chars,detail);
        VM_RETURN(DIAMOND_VM_IO_ERROR);
    }
    const long verify_result=SSL_get_verify_result(ssl);
    if(verify_result!=X509_V_OK) {
        snprintf(vm->error,sizeof vm->error,
            "TLS certificate verification failed for '%.*s': %s",
            (int)host->length,host->chars,
            X509_verify_cert_error_string(verify_result));
        VM_RETURN(DIAMOND_VM_IO_ERROR);
    }
    HELPER_DONE;
}

/* DIAMOND_OP_MYSQL_OPEN, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_mysql_open(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,host_reg=0,user_reg=0,password_reg=0,database_reg=0,
        port_reg=0;
    READ_SHORT(dest);READ_SHORT(host_reg);READ_SHORT(user_reg);
    READ_SHORT(password_reg);READ_SHORT(database_reg);READ_SHORT(port_reg);
    VM_SANDBOX_GUARD("MySQL.open", "database");
#ifdef DIAMOND_NO_MYSQL
    (void)dest;(void)host_reg;(void)user_reg;(void)password_reg;
    (void)database_reg;(void)port_reg;
    snprintf(vm->error,sizeof vm->error,
        "MySQL support is not compiled into this build");
    VM_RETURN(DIAMOND_VM_MYSQL_ERROR);
#else
    if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[host_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "MySQL.open's host argument must be a String");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(registers[user_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[user_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "MySQL.open's user argument must be a String");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(registers[password_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[password_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "MySQL.open's password argument must be a String");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(registers[database_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[database_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "MySQL.open's database argument must be a String");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(registers[port_reg].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,
            "MySQL.open's port argument must be an Int");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondString *host=(const DiamondString *)registers[host_reg].as.object;
    const DiamondString *user=(const DiamondString *)registers[user_reg].as.object;
    const DiamondString *password=
        (const DiamondString *)registers[password_reg].as.object;
    const DiamondString *database=
        (const DiamondString *)registers[database_reg].as.object;
    const int64_t port=registers[port_reg].as.integer;
    if(port<0||port>UINT16_MAX) {
        snprintf(vm->error,sizeof vm->error,
            "MySQL.open's port argument out of range");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    /* mysql_real_connect's const char* arguments assume C
     * strings, unlike MYSQL_BIND's explicit buffer_length --
     * DiamondString->chars isn't guaranteed NUL-terminated (a
     * flexible array member sized by ->length only), so each
     * needs its own NUL-terminated copy here. */
    char *host_copy=malloc(host->length+1);
    char *user_copy=malloc(user->length+1);
    char *password_copy=malloc(password->length+1);
    char *database_copy=malloc(database->length+1);
    if(host_copy==nullptr||user_copy==nullptr||password_copy==nullptr||
       database_copy==nullptr) {
        free(host_copy);free(user_copy);free(password_copy);free(database_copy);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    memcpy(host_copy,host->chars,host->length);host_copy[host->length]='\0';
    memcpy(user_copy,user->chars,user->length);user_copy[user->length]='\0';
    memcpy(password_copy,password->chars,password->length);
    password_copy[password->length]='\0';
    memcpy(database_copy,database->chars,database->length);
    database_copy[database->length]='\0';
    MYSQL *conn=mysql_init(nullptr);
    if(conn==nullptr) {
        free(host_copy);free(user_copy);free(password_copy);free(database_copy);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    /* Same requirement as libpq's PQconnectdb: a failed
     * mysql_real_connect returns nullptr but leaves `conn`
     * itself (allocated by mysql_init above) still owned by
     * the caller -- mysql_close(conn), not mysql_close(connected)
     * (nullptr), is what actually frees it and must run on
     * this path too. */
    MYSQL *connected=nullptr;
    if(vm->interrupt_flag==nullptr) {
        connected=mysql_real_connect(conn,host_copy,user_copy,password_copy,
            database_copy,(unsigned int)port,nullptr,0);
    } else {
        /* A supervised attempt can be stopped: connect on a helper thread
         * and wait for it in slices that look at the interrupt flag. An
         * abandoned connect finishes on its own and then cleans up after
         * itself (see MysqlConnectJob). */
        MysqlConnectJob *job=calloc(1,sizeof *job);
        if(job==nullptr) {
            free(host_copy);free(user_copy);free(password_copy);
            free(database_copy);mysql_close(conn);
            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
        }
        *job=(MysqlConnectJob){.conn=conn,.host=host_copy,.user=user_copy,
            .password=password_copy,.database=database_copy,
            .port=(unsigned int)port};
        host_copy=user_copy=password_copy=database_copy=nullptr;
        if(pthread_create(&job->thread,nullptr,mysql_connect_thread,job)!=0) {
            mysql_connect_job_free(job);
            mysql_close(conn);
            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
        }
        /* Once `done` is 2 the helper thread may free the job at any
         * moment, so nothing below reads it after that exchange. */
        const pthread_t connect_thread=job->thread;
        for(;;) {
            if(atomic_load(&job->done))break;
            if(vm_interrupt_requested(vm)) {
                int running=0;
                if(atomic_compare_exchange_strong(&job->done,&running,2)) {
                    pthread_detach(connect_thread);
                    VM_RETURN(DIAMOND_VM_INTERRUPTED);
                }
                break;
            }
            struct timespec slice={0,5L*1000000L};
            nanosleep(&slice,nullptr);
        }
        pthread_join(job->thread,nullptr);
        connected=job->result;
        mysql_connect_job_free(job);
    }
    free(host_copy);free(user_copy);free(password_copy);free(database_copy);
    if(connected==nullptr) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_error(conn));
        mysql_close(conn);
        VM_RETURN(DIAMOND_VM_MYSQL_ERROR);
    }
    DiamondMysqlHandle *handle=allocate_mysql_handle(vm,connected);
    if(handle==nullptr) {
        mysql_close(connected);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
        .as.object=(DiamondObject *)handle};
#endif
    HELPER_DONE;
}

/* Dir.mkdir_p: mkdir each prefix of path in turn, as `mkdir -p` does. A
 * prefix that already exists is fine as long as the whole path ends up a
 * directory. */
static DiamondVmStatus dir_mkdir_p_helper(DiamondVm *vm,const DiamondString *path,mode_t mode) {
    if(path->length==0) {
        snprintf(vm->error,sizeof vm->error,"Dir.mkdir_p path must not be empty");
        return DIAMOND_VM_IO_ERROR;
    }
    char *prefix=malloc(path->length+1);
    if(prefix==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    memcpy(prefix,path->chars,path->length);prefix[path->length]='\0';
    int failure=0;
    for(size_t end=1;end<=path->length&&failure==0;end++) {
        if(end<path->length&&prefix[end]!='/')continue;
        if(prefix[end-1]=='/')continue;
        const char saved=prefix[end];prefix[end]='\0';
        if(mkdir(prefix,mode)!=0&&errno!=EEXIST)failure=errno;
        prefix[end]=saved;
    }
    struct stat made;
    if(failure==0&&(stat(prefix,&made)!=0||!S_ISDIR(made.st_mode)))
        failure=errno!=0&&stat(prefix,&made)!=0?errno:ENOTDIR;
    free(prefix);
    if(failure!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot create directory '%.*s': %s",
            (int)path->length,path->chars,strerror(failure));
        return DIAMOND_VM_IO_ERROR;
    }
    return DIAMOND_VM_OK;
}

/* DIAMOND_OP_FILE_PATH, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_file_path(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,arg1=0,arg2=0;uint8_t selector=0;
    READ_SHORT(dest);READ_SHORT(arg1);READ_SHORT(arg2);READ_BYTE(selector);
    if(selector==DIAMOND_FILE_PATH_TTY) {
        /* A Symbol and a String share their layout (length, then chars). */
        const DiamondValue stream=registers[arg1];
        if(stream.kind!=DIAMOND_VALUE_OBJECT||(stream.as.object->kind!=DIAMOND_OBJECT_SYMBOL&&
           stream.as.object->kind!=DIAMOND_OBJECT_STRING)) {
            snprintf(vm->error,sizeof vm->error,"IO.tty? takes :stdin, :stdout or :stderr");
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
        const DiamondString *name=(const DiamondString *)stream.as.object;
        int descriptor=-1;
        if(name->length==5&&memcmp(name->chars,"stdin",5)==0)descriptor=STDIN_FILENO;
        else if(name->length==6&&memcmp(name->chars,"stdout",6)==0)descriptor=STDOUT_FILENO;
        else if(name->length==6&&memcmp(name->chars,"stderr",6)==0)descriptor=STDERR_FILENO;
        if(descriptor<0) {
            snprintf(vm->error,sizeof vm->error,
                "IO.tty? takes :stdin, :stdout or :stderr, not :%.*s",
                (int)(name->length>40?40:name->length),name->chars);
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
        registers[dest]=DIAMOND_BOOL(isatty(descriptor)!=0);
        HELPER_DONE;
    }
    if(registers[arg1].kind!=DIAMOND_VALUE_OBJECT||
       registers[arg1].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"File path argument must be a String");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondString *path=(const DiamondString *)registers[arg1].as.object;
    const bool has_second=registers[arg2].kind!=DIAMOND_VALUE_NIL;
    /* Dir.mkdir's second operand is the mode, an Int; every other selector
     * takes a String there. */
    const bool second_is_mode=selector==DIAMOND_FILE_PATH_MKDIR||
        selector==DIAMOND_FILE_PATH_MKDIR_P;
    if(second_is_mode&&has_second&&registers[arg2].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,"Dir.mkdir mode must be an Int");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(has_second&&!second_is_mode&&(registers[arg2].kind!=DIAMOND_VALUE_OBJECT||
                    registers[arg2].as.object->kind!=DIAMOND_OBJECT_STRING)) {
        snprintf(vm->error,sizeof vm->error,"File path argument must be a String");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondString *second=has_second&&!second_is_mode?
        (const DiamondString *)registers[arg2].as.object:nullptr;
    DiamondValue path_result=DIAMOND_NIL;
    DiamondVmStatus path_status=DIAMOND_VM_OK;
    /* The string-only selectors tolerate a NUL; every other one
     * hands path->chars to the OS, which would stop at it. */
    switch((DiamondFilePathFunction)selector) {
        case DIAMOND_FILE_PATH_DIRNAME:
        case DIAMOND_FILE_PATH_BASENAME:
        case DIAMOND_FILE_PATH_EXTNAME:
        case DIAMOND_FILE_PATH_ABSOLUTE:
            break;
        default:
            if(memchr(path->chars,'\0',path->length)!=nullptr) {
                snprintf(vm->error,sizeof vm->error,
                    "File path must not contain a NUL byte");
                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
            }
    }
    switch((DiamondFilePathFunction)selector) {
        case DIAMOND_FILE_PATH_SYNC:
            VM_SANDBOX_GUARD("File.sync", "filesystem");
            path_status=file_sync_helper(vm,path);
            path_result=registers[arg1];
            break;
        case DIAMOND_FILE_PATH_PUBLISH:
            VM_SANDBOX_GUARD("File.publish", "filesystem");
            path_status=file_publish_helper(vm,path,second);
            path_result=registers[arg1];
            break;
        case DIAMOND_FILE_PATH_RENAME:
            VM_SANDBOX_GUARD("File.rename", "filesystem");
            path_status=file_rename_helper(vm,path,second);
            path_result=registers[arg2];
            break;
        case DIAMOND_FILE_PATH_DIRNAME:
            path_status=file_path_dirname_helper(vm,path,&path_result);break;
        case DIAMOND_FILE_PATH_BASENAME:
            path_status=file_path_basename_helper(vm,path,second,&path_result);break;
        case DIAMOND_FILE_PATH_EXTNAME:
            path_status=file_path_extname_helper(vm,path,&path_result);break;
        case DIAMOND_FILE_PATH_ABSOLUTE:
            path_result=DIAMOND_BOOL(path->length>0&&path->chars[0]=='/');break;
        case DIAMOND_FILE_PATH_EXPAND:
            VM_SANDBOX_GUARD("File.expand_path", "filesystem");
            path_status=file_path_expand_helper(vm,path,second,&path_result);break;
        case DIAMOND_FILE_PATH_EXIST: {
            VM_SANDBOX_GUARD("File.exist?", "filesystem");
            struct stat path_stat;
            path_result=DIAMOND_BOOL(stat(path->chars,&path_stat)==0);
            break;
        }
        case DIAMOND_FILE_PATH_READ:
            VM_SANDBOX_GUARD("File.read", "filesystem");
            path_status=file_read_helper(vm,path,&path_result);break;
        case DIAMOND_FILE_PATH_WRITE:
            VM_SANDBOX_GUARD("File.write", "filesystem");
            path_status=file_write_helper(vm,path,second,&path_result);break;
        case DIAMOND_FILE_PATH_COPY:
            VM_SANDBOX_GUARD("File.copy", "filesystem");
            path_status=file_copy_helper(vm,path,second);
            path_result=registers[arg2];
            break;
        case DIAMOND_FILE_PATH_SIZE: {
            VM_SANDBOX_GUARD("File.size", "filesystem");
            struct stat path_stat;
            if(stat(path->chars,&path_stat)!=0) {
                snprintf(vm->error,sizeof vm->error,"cannot read the size of '%.*s': %s",
                    (int)path->length,path->chars,strerror(errno));
                VM_RETURN(DIAMOND_VM_IO_ERROR);
            }
            path_result=DIAMOND_INT((int64_t)path_stat.st_size);
            break;
        }
        case DIAMOND_FILE_PATH_SHA256:
            VM_SANDBOX_GUARD("File.sha256", "filesystem");
            path_status=file_sha256_helper(vm,path,&path_result);break;
        case DIAMOND_FILE_PATH_MODE: {
            VM_SANDBOX_GUARD("File.mode", "filesystem");
            struct stat path_stat;
            if(stat(path->chars,&path_stat)!=0) {
                snprintf(vm->error,sizeof vm->error,"cannot read the mode of '%.*s': %s",
                    (int)path->length,path->chars,strerror(errno));
                VM_RETURN(DIAMOND_VM_IO_ERROR);
            }
            path_result=DIAMOND_INT((int64_t)(path_stat.st_mode&07777));
            break;
        }
        case DIAMOND_FILE_PATH_FILE: {
            VM_SANDBOX_GUARD("File.file?", "filesystem");
            struct stat path_stat;
            path_result=DIAMOND_BOOL(stat(path->chars,&path_stat)==0&&S_ISREG(path_stat.st_mode));
            break;
        }
        case DIAMOND_FILE_PATH_SYMLINK: {
            VM_SANDBOX_GUARD("File.symlink?", "filesystem");
            struct stat path_stat;
            path_result=DIAMOND_BOOL(lstat(path->chars,&path_stat)==0&&S_ISLNK(path_stat.st_mode));
            break;
        }
        case DIAMOND_FILE_PATH_MKDIR: {
            VM_SANDBOX_GUARD("Dir.mkdir", "filesystem");
            const mode_t mode=has_second?(mode_t)(registers[arg2].as.integer&07777):0777;
            if(mkdir(path->chars,mode)!=0) {
                snprintf(vm->error,sizeof vm->error,"cannot create directory '%.*s': %s",
                    (int)path->length,path->chars,strerror(errno));
                VM_RETURN(DIAMOND_VM_IO_ERROR);
            }
            path_result=registers[arg1];
            break;
        }
        case DIAMOND_FILE_PATH_MKDIR_P: {
            VM_SANDBOX_GUARD("Dir.mkdir_p", "filesystem");
            const mode_t mode=has_second?(mode_t)(registers[arg2].as.integer&07777):0777;
            path_status=dir_mkdir_p_helper(vm,path,mode);
            path_result=registers[arg1];
            break;
        }
        case DIAMOND_FILE_PATH_RMDIR:
            VM_SANDBOX_GUARD("Dir.delete", "filesystem");
            if(rmdir(path->chars)!=0) {
                snprintf(vm->error,sizeof vm->error,"cannot remove directory '%.*s': %s",
                    (int)path->length,path->chars,strerror(errno));
                VM_RETURN(DIAMOND_VM_IO_ERROR);
            }
            path_result=registers[arg1];
            break;
        case DIAMOND_FILE_PATH_DIRSYNC: {
            VM_SANDBOX_GUARD("Dir.sync", "filesystem");
            const int directory=open(path->chars,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
            const int saved=directory<0?errno:(fsync(directory)!=0?errno:0);
            if(directory>=0)close(directory);
            if(saved!=0) {
                snprintf(vm->error,sizeof vm->error,"cannot sync directory '%.*s': %s",
                    (int)path->length,path->chars,strerror(saved));
                VM_RETURN(DIAMOND_VM_IO_ERROR);
            }
            path_result=registers[arg1];
            break;
        }
        case DIAMOND_FILE_PATH_DIRECTORY: {
            VM_SANDBOX_GUARD("File.directory?", "filesystem");
            struct stat path_stat;
            const bool is_directory=
                stat(path->chars,&path_stat)==0&&S_ISDIR(path_stat.st_mode);
            path_result=DIAMOND_BOOL(is_directory);
            break;
        }
        default: VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    }
    VM_PROPAGATE(path_status);
    registers[dest]=path_result;
    HELPER_DONE;
}

/* DIAMOND_OP_TCP_LISTEN_NONBLOCK, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_tcp_listen_nonblock(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        uint8_t instruction,size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,port_reg=0,reuse_port_reg=0;
    READ_SHORT(dest);READ_SHORT(port_reg);READ_SHORT(reuse_port_reg);
    VM_SANDBOX_GUARD("TCPServer.listen", "network");
    if(registers[port_reg].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,
                 "TCPServer.listen argument must be an Int port");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    /* The third operand is a Bool (reuse_port, the original form), nil,
     * or a Hash of the keyword options {"reuse_port", "host", "backlog"}
     * the compiler builds when more than reuse_port is given. */
    bool reuse_port=false;
    const char *listen_host=nullptr;
    int64_t listen_backlog=0;
    if(registers[reuse_port_reg].kind==DIAMOND_VALUE_BOOL) {
        reuse_port=registers[reuse_port_reg].as.boolean;
    } else if(registers[reuse_port_reg].kind==DIAMOND_VALUE_OBJECT&&
              registers[reuse_port_reg].as.object->kind==DIAMOND_OBJECT_HASH) {
        const DiamondHash *listen_options=
            (const DiamondHash *)registers[reuse_port_reg].as.object;
        for(size_t option_index=0;option_index<listen_options->count;option_index++) {
            const DiamondValue option_key=listen_options->entries[option_index].key;
            const DiamondValue option_value=listen_options->entries[option_index].value;
            const DiamondString *key_string=
                option_key.kind==DIAMOND_VALUE_OBJECT&&
                option_key.as.object->kind==DIAMOND_OBJECT_STRING
                ?(const DiamondString *)option_key.as.object:nullptr;
            if(key_string!=nullptr&&key_string->length==10&&
               memcmp(key_string->chars,"reuse_port",10)==0) {
                if(option_value.kind!=DIAMOND_VALUE_BOOL) {
                    snprintf(vm->error,sizeof vm->error,
                        "TCPServer.listen's reuse_port option must be a Bool");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                reuse_port=option_value.as.boolean;
            } else if(key_string!=nullptr&&key_string->length==4&&
                      memcmp(key_string->chars,"host",4)==0) {
                if(option_value.kind!=DIAMOND_VALUE_OBJECT||
                   option_value.as.object->kind!=DIAMOND_OBJECT_STRING||
                   ((const DiamondString *)option_value.as.object)->length==0) {
                    snprintf(vm->error,sizeof vm->error,
                        "TCPServer.listen's host option must be a non-empty String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                listen_host=((const DiamondString *)option_value.as.object)->chars;
            } else if(key_string!=nullptr&&key_string->length==7&&
                      memcmp(key_string->chars,"backlog",7)==0) {
                if(option_value.kind!=DIAMOND_VALUE_INT||
                   option_value.as.integer<1) {
                    snprintf(vm->error,sizeof vm->error,
                        "TCPServer.listen's backlog option must be a positive Int");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                listen_backlog=option_value.as.integer;
            } else {
                snprintf(vm->error,sizeof vm->error,
                    "unrecognized TCPServer.listen option");
                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
            }
        }
    } else if(registers[reuse_port_reg].kind!=DIAMOND_VALUE_NIL) {
        snprintf(vm->error,sizeof vm->error,
                 "TCPServer.listen's reuse_port option must be a Bool");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    DiamondListenerHandle *listener_handle=nullptr;
    const DiamondVmStatus listen_status=tcp_listen_helper(vm,listen_host,
        registers[port_reg].as.integer,
        instruction==DIAMOND_OP_TCP_LISTEN_NONBLOCK,
        reuse_port,listen_backlog,&listener_handle);
    VM_PROPAGATE(listen_status);
    registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
        .as.object=(DiamondObject *)listener_handle};
    HELPER_DONE;
}

/* DIAMOND_OP_CHANNEL_NEW, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_channel_new(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,capacity_reg=0;
    READ_SHORT(dest);READ_SHORT(capacity_reg);
    if(registers[capacity_reg].kind!=DIAMOND_VALUE_INT||
       registers[capacity_reg].as.integer<0||
       registers[capacity_reg].as.integer>1000000) {
        snprintf(vm->error,sizeof vm->error,
            "Channel.new's argument must be an Int capacity between "
            "0 (a rendezvous channel) and 1,000,000");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const bool rendezvous=registers[capacity_reg].as.integer==0;
    const size_t capacity=rendezvous?1:(size_t)registers[capacity_reg].as.integer;
    /* Same clone_program_from_chunk call DIAMOND_OP_THREAD_NEW
     * above already uses -- see DiamondChannel's own comment for
     * why the channel needs its own permanent copy of these
     * tables rather than borrowing the ambient chunk's. */
    DiamondProgram *private_program=clone_program_from_chunk(chunk);
    if(private_program==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    DiamondVm *private_vm=malloc(sizeof *private_vm);
    if(private_vm==nullptr) {
        diamond_program_free(private_program);free(private_program);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    diamond_vm_init(private_vm);
    DiamondValue *queue=calloc(capacity,sizeof *queue);
    if(queue==nullptr) {
        diamond_vm_free(private_vm);free(private_vm);
        diamond_program_free(private_program);free(private_program);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    DiamondChannel *new_channel=malloc(sizeof *new_channel);
    if(new_channel==nullptr) {
        free(queue);
        diamond_vm_free(private_vm);free(private_vm);
        diamond_program_free(private_program);free(private_program);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    *new_channel=(DiamondChannel){.private_vm=private_vm,
        .private_program=private_program,.queue=queue,
        .capacity=capacity,.rendezvous=rendezvous};
    atomic_init(&new_channel->refcount,1);
    pthread_mutex_init(&new_channel->lock,nullptr);
    pthread_cond_init(&new_channel->not_empty,nullptr);
    pthread_cond_init(&new_channel->not_full,nullptr);
    /* See DiamondVm.extra_roots' own comment: private_vm never
     * runs bytecode of its own, so its only root set is
     * whatever's actually queued -- extra_root_count starts at
     * 0 (nothing queued yet) and is kept current by send/
     * receive, both of which already hold `lock` before
     * touching private_vm at all. */
    private_vm->extra_roots=queue;
    DiamondChannelHandle *new_handle=
        allocate_channel_handle(vm,new_channel);
    if(new_handle==nullptr) {
        free_channel_reference(new_channel);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
        .as.object=(DiamondObject *)new_handle};
    HELPER_DONE;
}

/* DIAMOND_OP_DIR_ENTRIES, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_dir_entries(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,path_reg=0;
    READ_SHORT(dest);READ_SHORT(path_reg);
    VM_SANDBOX_GUARD("Dir.entries", "filesystem");
    if(registers[path_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[path_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"Dir.entries argument must be a String value");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondString *path=(const DiamondString *)registers[path_reg].as.object;
    errno=0;
    DIR *directory=opendir(path->chars);
    if(directory==nullptr) {
        snprintf(vm->error,sizeof vm->error,"cannot open directory '%.*s': %s",
                 (int)path->length,path->chars,strerror(errno));
        VM_RETURN(DIAMOND_VM_IO_ERROR);
    }
    /* Rooted immediately -- entries is pushed onto as it
     * grows, same "root the outer Array first, every element
     * is reachable through it before the next one's own
     * allocation" discipline tensor_to_a_helper's own comment
     * (earlier in this file) explains in full. */
    DiamondArray *entries=allocate_array(vm,nullptr,0);
    if(entries==nullptr) {
        closedir(directory);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    registers[dest]=DIAMOND_OBJECT(entries);
    /* readdir's own error contract: a nullptr return means
     * either "no more entries" (errno left unchanged) or a
     * genuine read error (errno set) -- indistinguishable
     * without resetting errno immediately before every call
     * and checking it only once the loop actually stops. */
    errno=0;
    struct dirent *entry=readdir(directory);
    while(entry!=nullptr) {
        if(strcmp(entry->d_name,".")!=0&&strcmp(entry->d_name,"..")!=0) {
            DiamondString *name=
                allocate_string(vm,entry->d_name,strlen(entry->d_name));
            if(name==nullptr) {
                closedir(directory);
                VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
            }
            if(!array_push(vm,entries,DIAMOND_OBJECT(name))) {
                closedir(directory);
                VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
            }
        }
        errno=0;
        entry=readdir(directory);
    }
    if(errno!=0) {
        snprintf(vm->error,sizeof vm->error,"error reading directory '%.*s': %s",
                 (int)path->length,path->chars,strerror(errno));
        closedir(directory);
        VM_RETURN(DIAMOND_VM_IO_ERROR);
    }
    closedir(directory);
    HELPER_DONE;
}

/* DIAMOND_OP_POSTGRES_OPEN, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_postgres_open(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,conninfo_reg=0;
    READ_SHORT(dest);READ_SHORT(conninfo_reg);
    VM_SANDBOX_GUARD("PostgreSQL.open", "database");
#ifdef DIAMOND_NO_POSTGRES
    (void)dest;(void)conninfo_reg;
    snprintf(vm->error,sizeof vm->error,
        "PostgreSQL support is not compiled into this build");
    VM_RETURN(DIAMOND_VM_POSTGRES_ERROR);
#else
    if(registers[conninfo_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[conninfo_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "PostgreSQL.open argument must be a String conninfo");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondString *conninfo=
        (const DiamondString *)registers[conninfo_reg].as.object;
    PGconn *conn=PQconnectdb(conninfo->chars);
    /* Unlike sqlite3_open, a failed PQconnectdb still returns a
     * non-null conn whose PQerrorMessage must be read before
     * PQfinish-ing it -- conn is only ever null on the client's
     * own OOM, a separate case PQerrorMessage(nullptr) can't
     * describe. */
    if(conn==nullptr) {
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    if(PQstatus(conn)!=CONNECTION_OK) {
        snprintf(vm->error,sizeof vm->error,"%s",PQerrorMessage(conn));
        PQfinish(conn);
        VM_RETURN(DIAMOND_VM_POSTGRES_ERROR);
    }
    DiamondPostgresHandle *handle=allocate_postgres_handle(vm,conn);
    if(handle==nullptr) {
        PQfinish(conn);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
        .as.object=(DiamondObject *)handle};
#endif
    HELPER_DONE;
}

/* DIAMOND_OP_COMPILE_METHOD, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_compile_method(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,name_reg=0,params_reg=0,body_reg=0,bound_values_reg=0;
    uint16_t class_operand=0;
    READ_SHORT(dest);READ_SHORT(class_operand);READ_SHORT(name_reg);
    READ_SHORT(params_reg);READ_SHORT(body_reg);READ_SHORT(bound_values_reg);
    if(!resolve_class_operand(vm,chunk,registers,"compile_method",&class_operand))
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    if((size_t)class_operand>=chunk->class_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    if(registers[name_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[name_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"compile_method name must be a String");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(registers[params_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[params_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,
            "compile_method params must be an Array of Strings");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(registers[body_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[body_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"compile_method body_source must be a String");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(registers[bound_values_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[bound_values_reg].as.object->kind!=DIAMOND_OBJECT_HASH) {
        snprintf(vm->error,sizeof vm->error,"compile_method bound_values must be a Hash");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondString *name_string=(const DiamondString *)registers[name_reg].as.object;
    const DiamondArray *params_array=(const DiamondArray *)registers[params_reg].as.object;
    const DiamondString *body_string=(const DiamondString *)registers[body_reg].as.object;
    const DiamondHash *bound_values_hash=
        (const DiamondHash *)registers[bound_values_reg].as.object;
    const DiamondClass *target_class=chunk->classes[class_operand];
    const DiamondVmStatus compile_status=compile_method_helper(vm,target_class,
        name_string,params_array,body_string,bound_values_hash,&registers[dest]);
    VM_PROPAGATE(compile_status);
    HELPER_DONE;
}

/* DIAMOND_OP_THREAD_NEW, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_thread_new(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,callable_reg=0,base=0;uint8_t argc=0;
    READ_SHORT(dest);READ_SHORT(callable_reg);READ_SHORT(base);READ_BYTE(argc);
    if(registers[callable_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[callable_reg].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
        snprintf(vm->error,sizeof vm->error,
            "Thread.new's first argument must be a Callable value");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondClosure *callable=
        (const DiamondClosure *)registers[callable_reg].as.object;
    if(callable->capture_count!=0) {
        snprintf(vm->error,sizeof vm->error,
            "Thread.new's callable must not capture any local state");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    /* See DIAMOND_OP_CALL_CLOSURE's own comment -- doubly true
     * here, since clone_program_from_chunk below clones only
     * the ambient chunk, never a compile_method result's own
     * foreign one. */
    if(callable->foreign_chunk!=nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "a compile_method callable can only be passed to define_method");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if((size_t)callable->function_index>=chunk->function_count)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    const DiamondFunction *target_fn=chunk->functions[callable->function_index];
    if(argc<target_fn->required_arity||
       (argc>target_fn->arity && !target_fn->has_variadic))
        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    if(atomic_load(&diamond_active_thread_count)>=diamond_live_thread_limit()) {
        snprintf(vm->error,sizeof vm->error,
            "too many concurrently active threads");
        VM_RETURN(DIAMOND_VM_THREAD_ERROR);
    }
    DiamondProgram *child_program=clone_program_from_chunk(chunk);
    if(child_program==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    DiamondVm *child_vm=malloc(sizeof *child_vm);
    if(child_vm==nullptr) {
        diamond_program_free(child_program);free(child_program);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    diamond_vm_init(child_vm);
    diamond_vm_configure_jit_from_env(child_vm);
    DiamondThread *new_thread=malloc(sizeof *new_thread);
    if(new_thread==nullptr) {
        diamond_vm_free(child_vm);free(child_vm);
        diamond_program_free(child_program);free(child_program);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    *new_thread=(DiamondThread){.child_vm=child_vm,
        .child_program=child_program,
        .function_index=callable->function_index,.arg_count=argc};
    pthread_mutex_init(&new_thread->join_lock,nullptr);
    /* A supervised attempt that gets interrupted must not wait
     * forever for the threads it started: see thread_interrupt. */
    if(vm->interrupt_flag!=nullptr)
        child_vm->interrupt_flag=&new_thread->interrupt;
    /* From here on, `new_thread` is a fully valid DiamondThread
     * (free_thread works correctly on it regardless of whether
     * the arg copy below actually finishes), so every
     * remaining failure path in this case reuses free_thread
     * as its single cleanup rather than hand-rolling another
     * teardown sequence -- also where diamond_active_thread_
     * count's matching increment belongs: exactly the window
     * where a future free_thread call is guaranteed to
     * decrement it back out again. */
    if(!diamond_reserve_thread_slot()) {
        /* Another thread took the last slot since the check above. free_thread decrements the count, so count this one first. */
        atomic_fetch_add(&diamond_active_thread_count,1);
        free_thread(new_thread);
        snprintf(vm->error,sizeof vm->error,
            "too many concurrently active threads");
        VM_RETURN(DIAMOND_VM_THREAD_ERROR);
    }
    bool copy_failed=false;
    /* new_thread->args[] is a plain struct field, not scanned
     * by child_vm's GC until thread_entry_trampoline's own
     * run_chunk call copies it into a real frame -- so an
     * already-copied earlier argument is just as unrooted here
     * as the Array/Hash/Instance cases inside
     * copy_value_into_vm itself were before this fix. Same
     * remedy: protect each arg on child_vm's own gc_protected
     * stack as it's produced, and only unwind once every
     * argument is safely copied -- nothing else allocates on
     * child_vm between this loop finishing and the child
     * thread's first run_chunk frame taking over as the real
     * root. */
    /* Constants first: each is rooted the moment it is stored,
     * whereas the argument copies below are only protected while
     * the loop runs, so nothing may allocate on child_vm after it. */
    child_vm->spawned_thread=true;
    copy_namespace_constants_into_vm(vm,child_vm,chunk->classes,
        child_program->classes);
    const size_t args_mark=child_vm->gc_protected_count;
    for(size_t index=0;index<argc;index++) {
        if(!copy_value_into_vm(child_vm,registers[(size_t)base+index],
                nullptr,chunk->classes,child_program->classes,
                nullptr,&new_thread->args[index])||
           !gc_protect(child_vm,new_thread->args[index])) {
            copy_failed=true;break;
        }
    }
    new_thread->args[argc]=DIAMOND_NIL;
    if(!copy_failed&&callable->type_binding_count>0&&
       !copy_value_into_vm(child_vm,registers[callable_reg],nullptr,
            chunk->classes,child_program->classes,nullptr,&new_thread->args[argc]))
        copy_failed=true;
    gc_unprotect(child_vm,args_mark);
    if(copy_failed) {
        free_thread(new_thread);
        snprintf(vm->error,sizeof vm->error,
            "%s %s","Thread.new argument",copy_failure_reason());
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    new_thread->spawned=create_vm_thread(&new_thread->handle,
        thread_entry_trampoline,new_thread)==0;
    if(!new_thread->spawned) {
        free_thread(new_thread);
        snprintf(vm->error,sizeof vm->error,"failed to create thread");
        VM_RETURN(DIAMOND_VM_THREAD_ERROR);
    }
    DiamondThreadHandle *thread_handle=
        allocate_thread_handle(vm,new_thread);
    if(thread_handle==nullptr) {
        free_thread(new_thread);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
        .as.object=(DiamondObject *)thread_handle};
    HELPER_DONE;
}

/* DIAMOND_OP_SUPERVISOR_NEW, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_supervisor_new(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    /* One optional operand: the restart strategy symbol (nil
     * register when Supervisor.new() is called with none).
     * The child cap is a fixed constant, see
     * DIAMOND_MAX_SUPERVISOR_CHILDREN's own comment; the restart
     * delay and intensity limit come from Supervisor#restart_policy. */
    uint16_t dest=0,strategy_reg=0;
    READ_SHORT(dest);READ_SHORT(strategy_reg);
    /* calloc, never a `(DiamondSupervisor){}` compound literal --
     * see DiamondSupervisor's own comment: with children[]'s
     * DIAMOND_MAX_SUPERVISOR_CHILDREN*DIAMOND_MAX_ARGUMENTS-sized
     * footprint, a compound literal here would be a large
     * automatic-storage temporary that an unoptimized build
     * allocates unconditionally on run_chunk's OWN stack frame
     * (regardless of which opcode case actually runs), inflating
     * every recursive run_chunk call enough to blow the C stack
     * long before DIAMOND_MAX_CALL_DEPTH's own counter check
     * could catch it -- exactly the DiamondProgram calloc
     * convention this codebase already uses for its own large
     * fixed structs, for the identical reason. */
    DiamondSupervisorStrategy strategy=DIAMOND_SUPERVISOR_ONE_FOR_ONE;
    if(registers[strategy_reg].kind!=DIAMOND_VALUE_NIL) {
        const DiamondValue given=registers[strategy_reg];
        const DiamondSymbol *name=given.kind==DIAMOND_VALUE_OBJECT&&
            given.as.object->kind==DIAMOND_OBJECT_SYMBOL?
            (const DiamondSymbol *)given.as.object:nullptr;
        if(name!=nullptr&&name->length==11&&
           memcmp(name->chars,"one_for_one",11)==0)
            strategy=DIAMOND_SUPERVISOR_ONE_FOR_ONE;
        else if(name!=nullptr&&name->length==11&&
           memcmp(name->chars,"one_for_all",11)==0)
            strategy=DIAMOND_SUPERVISOR_ONE_FOR_ALL;
        else if(name!=nullptr&&name->length==12&&
           memcmp(name->chars,"rest_for_one",12)==0)
            strategy=DIAMOND_SUPERVISOR_REST_FOR_ONE;
        else {
            snprintf(vm->error,sizeof vm->error,
                "Supervisor.new's strategy must be :one_for_one, "
                ":one_for_all, or :rest_for_one");
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
    }
    DiamondSupervisor *new_supervisor=calloc(1,sizeof *new_supervisor);
    if(new_supervisor==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    new_supervisor->strategy=strategy;
    new_supervisor->max_restarts=-1;
    new_supervisor->within_ms=5000;
    new_supervisor->backoff_ms=20;
    new_supervisor->max_backoff_ms=20;
    atomic_init(&new_supervisor->refcount,1);
    atomic_init(&new_supervisor->stop_requested,false);
    pthread_mutex_init(&new_supervisor->lock,nullptr);
    DiamondSupervisorHandle *new_handle=
        allocate_supervisor_handle(vm,new_supervisor);
    if(new_handle==nullptr) {
        free_supervisor_reference(new_supervisor);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
        .as.object=(DiamondObject *)new_handle};
    HELPER_DONE;
}

/* DIAMOND_OP_DEFINE_METHOD, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_define_method(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,name_reg=0,callable_reg=0;uint16_t class_operand=0;
    READ_SHORT(dest);READ_SHORT(class_operand);READ_SHORT(name_reg);READ_SHORT(callable_reg);
    if(!resolve_class_operand(vm,chunk,registers,"define_method",&class_operand))
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    if((size_t)class_operand>=chunk->class_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    if(registers[name_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[name_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"define_method name must be a String");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(registers[callable_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[callable_reg].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
        snprintf(vm->error,sizeof vm->error,"define_method callable must be a Callable value");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondString *name_string=(const DiamondString *)registers[name_reg].as.object;
    if(name_string->length==0||name_string->length>=DIAMOND_MAX_FUNCTION_NAME) {
        snprintf(vm->error,sizeof vm->error,
            "define_method name must be 1 to %d characters",
            DIAMOND_MAX_FUNCTION_NAME-1);
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    DiamondClosure *replacement=(DiamondClosure *)registers[callable_reg].as.object;
    DiamondClass *class=(DiamondClass *)(void *)chunk->classes[class_operand];
    for(size_t index=0;index<class->method_count;index++)
        if(strlen(class->methods[index].name)==name_string->length&&
           memcmp(class->methods[index].name,name_string->chars,
                  name_string->length)==0) {
            snprintf(vm->error,sizeof vm->error,
                "class '%s' already has a method '%.*s' -- use redefine_method instead",
                class->name,(int)name_string->length,name_string->chars);
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
    if(replacement->capture_count!=0) {
        snprintf(vm->error,sizeof vm->error,
                 "define_method callable must not capture any variables");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    /* A ClassName.compile_method result carries its own chunk
     * (function_index is meaningless against the ambient
     * `chunk` for one of those) and was already validated
     * against a specific class's field layout at compile
     * time -- intended_class, compared by pointer identity,
     * catches installing it onto a *different* class than
     * the one it was actually compiled for. An ordinary
     * closure (foreign_chunk==nullptr) keeps the original
     * same-chunk, same-owner_class checks unchanged. */
    const DiamondChunk *function_chunk=
        replacement->foreign_chunk!=nullptr?replacement->foreign_chunk:chunk;
    if((size_t)replacement->function_index>=function_chunk->function_count)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    const DiamondFunction *new_function=
        function_chunk->functions[replacement->function_index];
    if(replacement->foreign_chunk!=nullptr) {
        if(replacement->intended_class!=class) {
            snprintf(vm->error,sizeof vm->error,
                "define_method callable was compiled for a different class than '%s'",
                class->name);
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
    } else if(new_function->owner_class!=class_operand) {
        snprintf(vm->error,sizeof vm->error,
                 "define_method callable must be a method of '%s'",class->name);
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(!diamond_class_grow_methods_while_running(class,class->method_count+1)) {
        if(class->method_count>=DIAMOND_MAX_METHODS) {
            snprintf(vm->error,sizeof vm->error,
                "class '%s' already has the maximum number of methods (%d)",
                class->name,DIAMOND_MAX_METHODS);
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    }
    DiamondMethod *added=&class->methods[class->method_count];
    *added=(DiamondMethod){0};
    memcpy(added->name,name_string->chars,name_string->length);
    added->name[name_string->length]='\0';
    added->function_index=replacement->function_index;
    added->source_chunk=replacement->foreign_chunk;
    /* bound_values are trailing parameters compile_method's own
     * synthesized source already appended (see
     * build_compiled_method_source) -- a caller of the
     * installed method never supplies them, so they're
     * excluded from the arity a caller is checked against;
     * dispatch appends them itself, see the
     * DIAMOND_OP_INVOKE_TYPED-family sites that read
     * method->bound_values. */
    added->bound_values=replacement->bound_values;
    added->bound_value_count=replacement->bound_value_count;
    added->arity=(uint8_t)(new_function->arity-1-replacement->bound_value_count);
    added->required_arity=
        (uint8_t)(new_function->required_arity-1-replacement->bound_value_count);
    added->has_variadic=new_function->has_variadic;
    class->method_count++;
    diamond_vm_invalidate_method_caches(vm);
    registers[dest]=DIAMOND_NIL;HELPER_DONE;
}

/* DIAMOND_OP_REDEFINE_METHOD, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_redefine_method(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,name_reg=0,callable_reg=0;uint16_t class_operand=0;
    READ_SHORT(dest);READ_SHORT(class_operand);READ_SHORT(name_reg);READ_SHORT(callable_reg);
    if(!resolve_class_operand(vm,chunk,registers,"redefine_method",&class_operand))
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    if((size_t)class_operand>=chunk->class_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    if(registers[name_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[name_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"redefine_method name must be a String");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(registers[callable_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[callable_reg].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
        snprintf(vm->error,sizeof vm->error,"redefine_method callable must be a Callable value");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondString *name_string=(const DiamondString *)registers[name_reg].as.object;
    DiamondClosure *replacement=(DiamondClosure *)registers[callable_reg].as.object;
    DiamondClass *class=(DiamondClass *)(void *)chunk->classes[class_operand];
    DiamondMethod *target=nullptr;
    for(size_t index=0;index<class->method_count;index++)
        if(strlen(class->methods[index].name)==name_string->length&&
           memcmp(class->methods[index].name,name_string->chars,name_string->length)==0) {
            target=&class->methods[index];break;
        }
    if(target==nullptr) {
        snprintf(vm->error,sizeof vm->error,"class '%s' has no method '%.*s' to redefine",
                 class->name,(int)name_string->length,name_string->chars);
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(replacement->capture_count!=0) {
        snprintf(vm->error,sizeof vm->error,
                 "redefine_method callable must not capture any variables");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    /* A compile_method result carries its own chunk and bound
     * values and was compiled for one specific class; it's
     * installed the same way define_method installs one (see
     * that handler's comment), just into the existing slot. */
    const DiamondChunk *function_chunk=
        replacement->foreign_chunk!=nullptr?replacement->foreign_chunk:chunk;
    if((size_t)replacement->function_index>=function_chunk->function_count)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    const DiamondFunction *new_function=
        function_chunk->functions[replacement->function_index];
    if(replacement->foreign_chunk!=nullptr) {
        if(replacement->intended_class!=class) {
            snprintf(vm->error,sizeof vm->error,
                "redefine_method callable was compiled for a different class than '%s'",
                class->name);
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
    } else if(new_function->owner_class!=class_operand) {
        snprintf(vm->error,sizeof vm->error,
                 "redefine_method callable must be a method of '%s'",class->name);
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const uint8_t new_arity=
        (uint8_t)(new_function->arity-1-replacement->bound_value_count);
    const uint8_t new_required_arity=
        (uint8_t)(new_function->required_arity-1-replacement->bound_value_count);
    if(new_arity!=target->arity||new_required_arity!=target->required_arity||
       new_function->has_variadic!=target->has_variadic)
        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    target->function_index=replacement->function_index;
    target->source_chunk=replacement->foreign_chunk;
    target->bound_values=replacement->bound_values;
    target->bound_value_count=replacement->bound_value_count;
    diamond_vm_invalidate_method_caches(vm);
    registers[dest]=DIAMOND_NIL;HELPER_DONE;
}

/* DIAMOND_OP_CHANNEL_SELECT, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_channel_select(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    /* Channel.select(channels, deadline) -- see docs/threads.md.
     * Receives from the first channel (in array order) that holds
     * a value, returning [index, value]; blocks until one does.
     * nil means nothing will arrive: the deadline passed, or
     * every channel is closed and drained. Built from the same
     * take-one-value step as Channel#receive plus the readiness
     * wait wait_readable already uses, so a value is only ever
     * consumed under its own channel's lock. */
    uint16_t dest=0,channels_reg=0,deadline_reg=0;
    READ_SHORT(dest);READ_SHORT(channels_reg);READ_SHORT(deadline_reg);
    const DiamondValue channels_value=registers[channels_reg];
    const DiamondValue deadline_value=registers[deadline_reg];
    bool valid_channels=channels_value.kind==DIAMOND_VALUE_OBJECT&&
        channels_value.as.object->kind==DIAMOND_OBJECT_ARRAY;
    const DiamondArray *candidates=valid_channels?
        (const DiamondArray *)channels_value.as.object:nullptr;
    for(size_t i=0;valid_channels&&i<candidates->count;i++)
        valid_channels=candidates->values[i].kind==DIAMOND_VALUE_OBJECT&&
            candidates->values[i].as.object->kind==DIAMOND_OBJECT_CHANNEL;
    if(!valid_channels||candidates->count==0) {
        snprintf(vm->error,sizeof vm->error,
            "Channel.select's first argument must be a non-empty Array of Channels");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const bool select_timed=deadline_value.kind!=DIAMOND_VALUE_NIL;
    if(select_timed&&(deadline_value.kind!=DIAMOND_VALUE_FLOAT||
                      !isfinite(deadline_value.as.real))) {
        snprintf(vm->error,sizeof vm->error,
            "Channel.select's deadline must be a finite monotonic Float or nil");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    bool *drained=calloc(candidates->count,sizeof *drained);
    if(drained==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    DiamondValue selected=DIAMOND_NIL;
    bool found=false;
    while(!found) {
        size_t open_count=0;
        for(size_t i=0;i<candidates->count&&!found;i++) {
            DiamondChannel *candidate=
                ((DiamondChannelHandle *)candidates->values[i].as.object)->channel;
            pthread_mutex_lock(&candidate->lock);
            if(candidate->count==0) {
                if(candidate->closed)drained[i]=true;
                else open_count++;
                pthread_mutex_unlock(&candidate->lock);
                continue;
            }
            DiamondValue copied=DIAMOND_NIL;
            if(!copy_value_into_vm(vm,candidate->queue[0],nullptr,
                   candidate->private_program->classes,chunk->classes,
                   nullptr,&copied)) {
                pthread_mutex_unlock(&candidate->lock);
                free(drained);
                snprintf(vm->error,sizeof vm->error,
                    "%s %s","Channel.select result",copy_failure_reason());
                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
            }
            memmove(candidate->queue,candidate->queue+1,
                (candidate->count-1)*sizeof *candidate->queue);
            candidate->count--;
            candidate->taken++;
            candidate->private_vm->extra_root_count=candidate->count;
            notify_channel_waiters(candidate);
            if(candidate->rendezvous)pthread_cond_broadcast(&candidate->not_full);
            else pthread_cond_signal(&candidate->not_full);
            pthread_mutex_unlock(&candidate->lock);
            /* Root the value in dest before allocating the result
             * pair, which may trigger a collection. */
            registers[dest]=copied;
            const DiamondValue pair_values[2]={DIAMOND_INT((int64_t)i),copied};
            DiamondArray *pair=allocate_array(vm,pair_values,2);
            if(pair==nullptr) {
                free(drained);
                VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
            }
            selected=DIAMOND_OBJECT(pair);
            found=true;
        }
        if(found)break;
        /* Closed and drained everywhere: nothing can ever arrive. */
        if(open_count==0)break;
        if(select_timed) {
            struct timespec now;
            if(clock_gettime(CLOCK_MONOTONIC,&now)!=0 ||
               deadline_value.as.real-(double)now.tv_sec-(double)now.tv_nsec/1e9<=0)
                break;
        }
        struct pollfd wait_fd[1];
        const DiamondVmStatus select_status=cancellable_wait_helper(vm,nullptr,
            false,channels_value,deadline_value,wait_fd,0,drained);
        if(select_status!=DIAMOND_VM_OK) {
            free(drained);
            VM_RETURN(select_status);
        }
    }
    free(drained);
    registers[dest]=selected;
    HELPER_DONE;
}

/* DIAMOND_OP_CHANNEL_SELECT_SEND, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_channel_select_send(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t *ip_ref) {
    size_t ip=*ip_ref;
    /* Channel.select_send(channels, value, deadline) -- the
     * send-side twin of CHANNEL_SELECT above (docs/threads.md).
     * Sends `value` to the first channel (in array order) with
     * room, returning its index; blocks until one has room. A
     * closed channel is skipped, like a drained one in select.
     * nil means nothing can accept it: the deadline passed, or
     * every channel is closed. The value is copied only into the
     * channel actually chosen, under that channel's own lock. */
    uint16_t dest=0,channels_reg=0,value_reg=0,deadline_reg=0;
    READ_SHORT(dest);READ_SHORT(channels_reg);
    READ_SHORT(value_reg);READ_SHORT(deadline_reg);
    const DiamondValue channels_value=registers[channels_reg];
    const DiamondValue deadline_value=registers[deadline_reg];
    bool valid_channels=channels_value.kind==DIAMOND_VALUE_OBJECT&&
        channels_value.as.object->kind==DIAMOND_OBJECT_ARRAY;
    const DiamondArray *candidates=valid_channels?
        (const DiamondArray *)channels_value.as.object:nullptr;
    for(size_t i=0;valid_channels&&i<candidates->count;i++)
        valid_channels=candidates->values[i].kind==DIAMOND_VALUE_OBJECT&&
            candidates->values[i].as.object->kind==DIAMOND_OBJECT_CHANNEL;
    if(!valid_channels||candidates->count==0) {
        snprintf(vm->error,sizeof vm->error,
            "Channel.select_send's first argument must be a non-empty Array of Channels");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const bool select_timed=deadline_value.kind!=DIAMOND_VALUE_NIL;
    if(select_timed&&(deadline_value.kind!=DIAMOND_VALUE_FLOAT||
                      !isfinite(deadline_value.as.real))) {
        snprintf(vm->error,sizeof vm->error,
            "Channel.select_send's deadline must be a finite monotonic Float or nil");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    for(size_t i=0;i<candidates->count;i++) {
        if(((DiamondChannelHandle *)candidates->values[i].as.object)->channel->rendezvous) {
            snprintf(vm->error,sizeof vm->error,
                "Channel.select_send does not accept a rendezvous channel (Channel.new(0)); "
                "use send or try_send");
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
    }
    bool *skipped=calloc(candidates->count,sizeof *skipped);
    if(skipped==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    DiamondValue selected=DIAMOND_NIL;
    bool found=false;
    while(!found) {
        size_t open_count=0;
        for(size_t i=0;i<candidates->count&&!found;i++) {
            DiamondChannel *candidate=
                ((DiamondChannelHandle *)candidates->values[i].as.object)->channel;
            pthread_mutex_lock(&candidate->lock);
            if(candidate->closed) {
                skipped[i]=true;
                pthread_mutex_unlock(&candidate->lock);
                continue;
            }
            open_count++;
            if(candidate->count==candidate->capacity) {
                pthread_mutex_unlock(&candidate->lock);
                continue;
            }
            DiamondValue copied=DIAMOND_NIL;
            candidate->private_vm->extra_root_count=candidate->count;
            if(!copy_value_into_vm(candidate->private_vm,registers[value_reg],
                   nullptr,chunk->classes,candidate->private_program->classes,
                   nullptr,&copied)) {
                pthread_mutex_unlock(&candidate->lock);
                free(skipped);
                snprintf(vm->error,sizeof vm->error,
                    "%s %s","Channel.select_send value",copy_failure_reason());
                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
            }
            candidate->queue[candidate->count++]=copied;
            candidate->private_vm->extra_root_count=candidate->count;
            notify_channel_waiters(candidate);
            pthread_cond_signal(&candidate->not_empty);
            pthread_mutex_unlock(&candidate->lock);
            selected=DIAMOND_INT((int64_t)i);
            found=true;
        }
        if(found||open_count==0)break;
        if(select_timed) {
            struct timespec now;
            if(clock_gettime(CLOCK_MONOTONIC,&now)!=0 ||
               deadline_value.as.real-(double)now.tv_sec-(double)now.tv_nsec/1e9<=0)
                break;
        }
        struct pollfd wait_fd[1];
        const DiamondVmStatus select_status=cancellable_wait_helper(vm,nullptr,
            true,channels_value,deadline_value,wait_fd,0,skipped);
        if(select_status!=DIAMOND_VM_OK) {
            free(skipped);
            VM_RETURN(select_status);
        }
    }
    free(skipped);
    registers[dest]=selected;
    HELPER_DONE;
}

/* DIAMOND_OP_IO_POLL, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_io_poll(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t depth,size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,readable_reg=0,writable_reg=0,timeout_reg=0;
    READ_SHORT(dest);READ_SHORT(readable_reg);READ_SHORT(writable_reg);
    READ_SHORT(timeout_reg);
    if(registers[readable_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[readable_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY||
       registers[writable_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[writable_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,"IO.poll readables and writables must be Arrays");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondArray *readable_array=
        (const DiamondArray *)registers[readable_reg].as.object;
    const DiamondArray *writable_array=
        (const DiamondArray *)registers[writable_reg].as.object;
    enum { DIAMOND_MAX_POLL_FDS = 256 };
    if(readable_array->count>DIAMOND_MAX_POLL_FDS||
       writable_array->count>DIAMOND_MAX_POLL_FDS) {
        snprintf(vm->error,sizeof vm->error,
            "IO.poll supports at most %d fds per list",DIAMOND_MAX_POLL_FDS);
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    /* Reserve a separate slot for the cancellation notification pipe. */
    struct pollfd fds[DIAMOND_MAX_POLL_FDS+1];
    nfds_t fd_count=0;
    size_t read_slot[DIAMOND_MAX_POLL_FDS];
    size_t write_slot[DIAMOND_MAX_POLL_FDS];
    for(size_t index=0;index<readable_array->count;index++) {
        int fd=-1;
        const DiamondVmStatus fd_status=
            pollable_fd(vm,readable_array->values[index],&fd);
        VM_PROPAGATE(fd_status);
        if(!poll_register_fd(fds,&fd_count,DIAMOND_MAX_POLL_FDS,fd,
                POLLIN,&read_slot[index])) {
            snprintf(vm->error,sizeof vm->error,
                "IO.poll supports at most %d distinct fds",DIAMOND_MAX_POLL_FDS);
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
    }
    for(size_t index=0;index<writable_array->count;index++) {
        int fd=-1;
        const DiamondVmStatus fd_status=
            pollable_fd(vm,writable_array->values[index],&fd);
        VM_PROPAGATE(fd_status);
        if(!poll_register_fd(fds,&fd_count,DIAMOND_MAX_POLL_FDS,fd,
                POLLOUT,&write_slot[index])) {
            snprintf(vm->error,sizeof vm->error,
                "IO.poll supports at most %d distinct fds",DIAMOND_MAX_POLL_FDS);
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
    }
    const DiamondValue timeout=registers[timeout_reg];
    if(timeout.kind==DIAMOND_VALUE_OBJECT&&
       timeout.as.object->kind==DIAMOND_OBJECT_HASH) {
        const DiamondHash *options=(const DiamondHash *)timeout.as.object;
        DiamondValue cancellations=DIAMOND_NIL,deadline=DIAMOND_NIL;
        for(size_t i=0;i<options->count;i++) {
            const DiamondValue key=options->entries[i].key;
            if(key.kind!=DIAMOND_VALUE_OBJECT||key.as.object->kind!=DIAMOND_OBJECT_STRING) {
                snprintf(vm->error,sizeof vm->error,"IO.poll option keys must be Strings");
                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
            }
            const DiamondString *name=(const DiamondString *)key.as.object;
            if(name->length==13&&memcmp(name->chars,"cancellations",13)==0)
                cancellations=options->entries[i].value;
            else if(name->length==8&&memcmp(name->chars,"deadline",8)==0)
                deadline=options->entries[i].value;
            else {
                snprintf(vm->error,sizeof vm->error,"unknown IO.poll option");
                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
            }
        }
        /* Unlink before returning to the VM's signal dispatcher:
         * handlers can close sockets or cancellation sources. */
        const DiamondVmStatus wait_status=cancellable_wait_helper(vm,
            nullptr,false,cancellations,deadline,fds,fd_count,nullptr);
        VM_PROPAGATE(wait_status);
    } else if(timeout.kind==DIAMOND_VALUE_INT) {
        const int64_t timeout_value=registers[timeout_reg].as.integer;
        bool budget_bounded=false;
        const int timeout_ms=bound_wait_by_wall_budget(vm,
            timeout_value<0?-1:
            (timeout_value>INT_MAX?INT_MAX:(int)timeout_value),
            &budget_bounded);
        int poll_result=0;
        DiamondVmStatus interrupt_status=DIAMOND_VM_OK;
        errno=0;
        poll_result=poll_interruptible(vm,fds,fd_count,timeout_ms,
            &interrupt_status);
        VM_PROPAGATE(interrupt_status);
        /* Unlike the plain EINTR-retries-unconditionally loop
         * this replaced, a signal actually gets handled here
         * before retrying -- gremlin_serve's own event loop calls
         * IO.poll with timeout_ms=-1 (block until something's
         * ready), so blindly retrying on every EINTR would mean a
         * trapped signal arriving while a gremlin server sits
         * idle would never actually run its handler until some
         * connection activity happened to wake the poll() up
         * first -- exactly backwards for "let me shut this server
         * down gracefully on Ctrl+C." */
        while(poll_result<0&&errno==EINTR) {
            bool signal_invoked=false;
            const DiamondVmStatus signal_status=
                dispatch_pending_signals(vm,chunk,depth,&signal_invoked);
            VM_PROPAGATE_SIGNAL(signal_status);
            errno=0;
            poll_result=poll_interruptible(vm,fds,fd_count,
                timeout_ms,&interrupt_status);
            VM_PROPAGATE(interrupt_status);
        }
        if(poll_result<0) {
            snprintf(vm->error,sizeof vm->error,"poll failed: %s",strerror(errno));
            VM_RETURN(DIAMOND_VM_IO_ERROR);
        }
        if(poll_result==0&&budget_bounded) {
            const DiamondVmStatus budget_status=
                wall_budget_after_wait(vm);
            VM_PROPAGATE(budget_status);
        }
    } else {
        snprintf(vm->error,sizeof vm->error,"IO.poll timeout must be an Int or cancellation options Hash");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    /* POLLHUP/POLLERR/POLLNVAL count toward *both* readiness
     * directions: a peer that closed its end, or a socket that
     * hit a genuine error, is exactly the condition a caller's
     * next .read()/.write() needs to be woken up to observe
     * (EOF as nil, or the error surfacing as IOError) rather
     * than sitting forever waiting for a POLLIN/POLLOUT that a
     * dead connection will never produce. */
    DiamondValue readable_results[DIAMOND_MAX_POLL_FDS];
    for(size_t index=0;index<readable_array->count;index++) {
        const short revents=fds[read_slot[index]].revents;
        readable_results[index]=
            DIAMOND_BOOL((revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL))!=0);
    }
    DiamondValue writable_results[DIAMOND_MAX_POLL_FDS];
    for(size_t index=0;index<writable_array->count;index++) {
        const short revents=fds[write_slot[index]].revents;
        writable_results[index]=
            DIAMOND_BOOL((revents&(POLLOUT|POLLHUP|POLLERR|POLLNVAL))!=0);
    }
    /* Every allocate_* call below can trigger a collection, and
     * this VM's GC only marks from rooted locations (registers,
     * the exception slot, frame chains) -- a value sitting in a
     * plain C local between two allocate_* calls is invisible to
     * it and would be swept out from under this function
     * (confirmed the hard way: a heap-use-after-free in
     * hash_find, caught by `make test-sanitize` under
     * DIAMOND_STRESS_GC=1, from an earlier version of this code
     * that allocated all four pieces before touching
     * registers[dest] at all). So: root the hash in
     * registers[dest] immediately, then for each entry, root its
     * key first with a DIAMOND_NIL placeholder value -- nil
     * needs no GC protection, so this is always safe -- before
     * allocating the real value and overwriting the placeholder
     * (hash_set already updates an existing key in place).
     * Nothing is ever more than one allocation away from being
     * reachable through registers[dest]. */
    DiamondHash *poll_result_hash=allocate_hash(vm);
    if(poll_result_hash==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    registers[dest]=DIAMOND_OBJECT(poll_result_hash);
    DiamondString *readable_key=allocate_string(vm,"readable",8);
    if(readable_key==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    if(!hash_set(vm,poll_result_hash,DIAMOND_OBJECT(readable_key),DIAMOND_NIL))
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    DiamondArray *readable_result_array=
        allocate_array(vm,readable_results,readable_array->count);
    if(readable_result_array==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    if(!hash_set(vm,poll_result_hash,DIAMOND_OBJECT(readable_key),
            DIAMOND_OBJECT(readable_result_array)))
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    DiamondString *writable_key=allocate_string(vm,"writable",8);
    if(writable_key==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    if(!hash_set(vm,poll_result_hash,DIAMOND_OBJECT(writable_key),DIAMOND_NIL))
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    DiamondArray *writable_result_array=
        allocate_array(vm,writable_results,writable_array->count);
    if(writable_result_array==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    if(!hash_set(vm,poll_result_hash,DIAMOND_OBJECT(writable_key),
            DIAMOND_OBJECT(writable_result_array)))
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    HELPER_DONE;
}

/* DIAMOND_OP_CALL_SPREAD, DIAMOND_OP_CALL_TYPED_SPREAD, DIAMOND_OP_CALL_KEYWORD_SPREAD,
 * DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD, moved out of run_chunk's switch (see HELPER_DONE below):
 * the case body is unchanged apart from its macros, so it keeps the operand decoding, sandbox
 * check and error returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_call_spread(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t depth,uint8_t instruction,size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t destination=0,function_index=0,array_register=0;
    READ_SHORT(destination);READ_SHORT(function_index);
    READ_SHORT(array_register);
    const bool has_keywords=(DiamondOpCode)instruction==
        DIAMOND_OP_CALL_KEYWORD_SPREAD||
        (DiamondOpCode)instruction==
        DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD;
    uint8_t keyword_count=0,keyword_slots[DIAMOND_MAX_DECLARED_PARAMETERS];
    uint16_t keyword_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
    if(has_keywords) {
        READ_BYTE(keyword_count);
        if(keyword_count==0||keyword_count>DIAMOND_MAX_DECLARED_PARAMETERS)
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        for(size_t index=0;index<keyword_count;index++) {
            READ_BYTE(keyword_slots[index]);
            READ_SHORT(keyword_registers[index]);
        }
    }
    uint8_t type_argument_count=0;uint16_t type_arguments[8];
    if((DiamondOpCode)instruction==DIAMOND_OP_CALL_TYPED_SPREAD||
       (DiamondOpCode)instruction==
        DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD) {
        READ_BYTE(type_argument_count);
        if(type_argument_count>8)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        for(size_t index=0;index<type_argument_count;index++)
            READ_SHORT(type_arguments[index]);
    }
    if((size_t)function_index>=chunk->function_count)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    if(registers[array_register].kind!=DIAMOND_VALUE_OBJECT||
       registers[array_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,
            "spread argument (*expr) must be an Array");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondArray *spread=
        (const DiamondArray *)registers[array_register].as.object;
    const DiamondFunction *function=chunk->functions[function_index];
    for(size_t index=0;index<keyword_count;index++)
        if(keyword_slots[index]>=function->arity||
           keyword_registers[index]>=DIAMOND_REGISTER_COUNT)
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    if(type_argument_count>0&&
       type_argument_count!=function->type_variable_count)
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    /* Only the first type_argument_count entries are ever read (below,
     * and through .type_variable_bindings); zeroing all eight cost about
     * 3 KB of memset on every call, typed or not. */
    DiamondTypeBinding explicit_bindings[type_argument_count==0?1:type_argument_count];
    for(size_t index=0;index<type_argument_count;index++) {
        explicit_bindings[index]=(DiamondTypeBinding){};
        if((size_t)type_arguments[index]>=chunk->type_set_count)
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        (void)binding_node(&explicit_bindings[index]);
        bind_context_set(&explicit_bindings[index],0,chunk,
            chunk->type_sets,type_arguments[index]);
    }
    const DiamondValue *call_arguments=spread->values;
    size_t call_argument_count=spread->count;
    DiamondValue *merged_arguments=nullptr;
    if(has_keywords) {
        bool filled[DIAMOND_MAX_DECLARED_PARAMETERS]={0};
        if(spread->count>DIAMOND_MAX_DECLARED_PARAMETERS)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
        for(size_t index=0;index<spread->count;index++)filled[index]=true;
        for(size_t index=0;index<keyword_count;index++) {
            const size_t slot=keyword_slots[index];
            if(filled[slot]) {
                snprintf(vm->error,sizeof vm->error,
                    "multiple values for the same argument");
                VM_RETURN(DIAMOND_VM_ARITY_ERROR);
            }
            filled[slot]=true;
            if(slot+1>call_argument_count)call_argument_count=slot+1;
        }
        /* A gap may only fall on a defaulted parameter; it stays
         * UNDEFINED, which ARGUMENT_PROVIDED reads as "not supplied". */
        for(size_t index=0;index<call_argument_count;index++)
            if(!filled[index]&&index<function->required_arity) {
                snprintf(vm->error,sizeof vm->error,"missing argument");
                VM_RETURN(DIAMOND_VM_ARITY_ERROR);
            }
        merged_arguments=malloc(call_argument_count*
            sizeof *merged_arguments);
        if(merged_arguments==nullptr)
            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
        for(size_t index=0;index<call_argument_count;index++)
            merged_arguments[index]=DIAMOND_UNDEFINED;
        for(size_t index=0;index<spread->count;index++)
            merged_arguments[index]=spread->values[index];
        for(size_t index=0;index<keyword_count;index++)
            merged_arguments[keyword_slots[index]]=
                registers[keyword_registers[index]];
        call_arguments=merged_arguments;
    }
    if(call_argument_count<function->required_arity||
       (call_argument_count>function->arity&&!function->has_variadic)) {
        free(merged_arguments);
        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    }
    const DiamondChunk called_chunk={
        .name=function->name,.code=function->code,
        .lines=function->lines,.columns=function->columns,
        .code_count=function->code_count,
        .constants=function->constants,
        .constant_count=function->constant_count,
        .strings=function->strings,.string_count=function->string_count,
        .type_sets=function->type_sets,
        .type_set_count=function->type_set_count,
        .functions=chunk->functions,.function_count=chunk->function_count,
        .classes=chunk->classes,.class_count=chunk->class_count,
        .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
        .parameter_type_sets=function->parameter_type_sets,
        .type_variable_count=function->type_variable_count,
        .class_type_variable=function->class_type_variable,
        .binding_owner_plus_one=(uint16_t)(function->owner_class+1u),
        .parameter_offset=function->owner_class==DIAMOND_OWNER_NONE?0:1,
        .type_variable_bindings=type_argument_count==0?nullptr:
            explicit_bindings,
        .register_count=function->register_count,
        .has_variadic=function->has_variadic,
    };
    DiamondValue spread_result=DIAMOND_NIL;
    /* spread->values is already a plain contiguous
     * DiamondValue* -- an Array's normal in-memory shape --
     * so it's passed straight to run_chunk as `arguments`,
     * no copy needed, the same way an ordinary call already
     * passes a live pointer into its own caller's registers
     * (&registers[argument_base] above) into a nested
     * run_chunk frame. `spread` itself stays reachable
     * throughout via registers[array_register], the same
     * live root any other in-use register already is. */
    const DiamondVmStatus spread_status=run_chunk(&called_chunk,vm,
        call_arguments,call_argument_count,depth+1,nullptr,
        &spread_result);
    free(merged_arguments);
    VM_PROPAGATE(spread_status);
    registers[destination]=spread_result;
    HELPER_DONE;
}

/* DIAMOND_OP_CALL_SINGLETON_KEYWORDS, DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS, moved out of
 * run_chunk's switch (see HELPER_DONE below): the case body is unchanged apart from its macros, so
 * it keeps the operand decoding, sandbox check and error returns it had. The caller applies
 * VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_call_singleton_keywords(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t depth,uint8_t instruction,size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t destination=0,function_index=0,positional_register=0;
    uint16_t class_index=0;uint8_t needs_receiver=0,keyword_count=0;
    uint16_t keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS],type_arguments[8];uint8_t type_count=0;
    uint16_t keyword_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
    READ_SHORT(destination);READ_SHORT(function_index);
    READ_SHORT(positional_register);READ_SHORT(class_index);
    READ_BYTE(needs_receiver);READ_BYTE(keyword_count);
    const bool has_block=(keyword_count&0x80u)!=0;
    keyword_count&=0x7fu;
    if((keyword_count==0&&!has_block)||keyword_count>DIAMOND_MAX_DECLARED_PARAMETERS)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    for(size_t index=0;index<keyword_count;index++) {
        READ_SHORT(keyword_names[index]);READ_SHORT(keyword_registers[index]);
    }
    uint16_t block_register=0;
    if(has_block)READ_SHORT(block_register);
    if(has_block&&block_register>=DIAMOND_REGISTER_COUNT)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    if((DiamondOpCode)instruction==
            DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS) {
        READ_BYTE(type_count);if(type_count>8)
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        for(size_t index=0;index<type_count;index++)READ_SHORT(type_arguments[index]);
    }
    if((size_t)function_index>=chunk->function_count||needs_receiver>1||
       (class_index!=DIAMOND_NO_CLASS&&(size_t)class_index>=chunk->class_count)||
       registers[positional_register].kind!=DIAMOND_VALUE_OBJECT||
       registers[positional_register].as.object->kind!=DIAMOND_OBJECT_ARRAY)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    const DiamondFunction *function=chunk->functions[function_index];
    if(function->arity<(uint8_t)needs_receiver||
       (type_count>0&&type_count!=function->type_variable_count))
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    DiamondValue *merged=nullptr;size_t merged_count=0;
    DiamondVmStatus status=merge_keyword_arguments(vm,chunk,function,
        (const DiamondArray *)registers[positional_register].as.object,
        keyword_names,keyword_registers,keyword_count,registers,
        function->arity-needs_receiver,
        has_block?&registers[block_register]:nullptr,
        &merged,&merged_count);
    VM_PROPAGATE(status);
    const size_t total=merged_count+needs_receiver;
    if(total<function->required_arity||
       (total>function->arity&&!function->has_variadic)) {
        free(merged);VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    }
    DiamondValue *singleton_arguments=malloc(total*
        sizeof *singleton_arguments);
    if(singleton_arguments==nullptr&&total>0) {free(merged);
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);}
    if(needs_receiver)singleton_arguments[0]=class_index==DIAMOND_NO_CLASS?
        DIAMOND_NIL:DIAMOND_CLASS(class_index);
    for(size_t index=0;index<merged_count;index++)
        singleton_arguments[index+needs_receiver]=merged[index];
    free(merged);
    DiamondTypeBinding singleton_bindings[type_count==0?1:type_count];
    for(size_t index=0;index<type_count;index++) {
        singleton_bindings[index]=(DiamondTypeBinding){};
        if((size_t)type_arguments[index]>=chunk->type_set_count) {
            free(singleton_arguments);VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);}
        (void)binding_node(&singleton_bindings[index]);
        bind_context_set(&singleton_bindings[index],0,chunk,chunk->type_sets,
            type_arguments[index]);
    }
    DiamondChunk child={.name=function->name,.code=function->code,
        .lines=function->lines,.columns=function->columns,
        .code_count=function->code_count,.constants=function->constants,
        .constant_count=function->constant_count,.strings=function->strings,
        .string_count=function->string_count,.type_sets=function->type_sets,
        .type_set_count=function->type_set_count,.functions=chunk->functions,
        .function_count=chunk->function_count,.classes=chunk->classes,
        .class_count=chunk->class_count,.interfaces=chunk->interfaces,
        .interface_count=chunk->interface_count,
        .parameter_type_sets=function->parameter_type_sets,
        .type_variable_count=function->type_variable_count,
        .class_type_variable=function->class_type_variable,
        .binding_owner_plus_one=(uint16_t)(function->owner_class+1u),
        .parameter_offset=function->owner_class==DIAMOND_OWNER_NONE?0:1,
        .type_variable_bindings=type_count==0?nullptr:singleton_bindings,
        .register_count=function->register_count,
        .has_variadic=function->has_variadic};
    DiamondValue singleton_result=DIAMOND_NIL;
    status=run_chunk(&child,vm,singleton_arguments,total,depth+1,
        nullptr,&singleton_result);
    free(singleton_arguments);VM_PROPAGATE(status);
    registers[destination]=singleton_result;HELPER_DONE;
}

/* DIAMOND_OP_CALL_SINGLETON_SPREAD, DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD, moved out of
 * run_chunk's switch (see HELPER_DONE below): the case body is unchanged apart from its macros, so
 * it keeps the operand decoding, sandbox check and error returns it had. The caller applies
 * VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_call_singleton_spread(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t depth,uint8_t instruction,size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t destination=0,function_index=0,spread_register=0;
    uint16_t class_index=0;uint8_t needs_receiver=0;
    READ_SHORT(destination);READ_SHORT(function_index);
    READ_SHORT(spread_register);READ_SHORT(class_index);
    READ_BYTE(needs_receiver);
    uint8_t type_argument_count=0;uint16_t type_arguments[8];
    if((DiamondOpCode)instruction==
            DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD) {
        READ_BYTE(type_argument_count);
        if(type_argument_count>8)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        for(size_t index=0;index<type_argument_count;index++)
            READ_SHORT(type_arguments[index]);
    }
    if((size_t)function_index>=chunk->function_count||
       needs_receiver>1||
       (class_index!=DIAMOND_NO_CLASS&&(size_t)class_index>=chunk->class_count))
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    if(registers[spread_register].kind!=DIAMOND_VALUE_OBJECT||
       registers[spread_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,
            "spread argument (*expr) must be an Array");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondArray *spread=(const DiamondArray *)
        registers[spread_register].as.object;
    const DiamondFunction *function=chunk->functions[function_index];
    if(type_argument_count>0&&
       type_argument_count!=function->type_variable_count)
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    /* Only the first type_argument_count entries are ever read (below,
     * and through .type_variable_bindings); zeroing all eight cost about
     * 3 KB of memset on every call, typed or not. */
    DiamondTypeBinding explicit_bindings[type_argument_count==0?1:type_argument_count];
    for(size_t index=0;index<type_argument_count;index++) {
        explicit_bindings[index]=(DiamondTypeBinding){};
        if((size_t)type_arguments[index]>=chunk->type_set_count)
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        (void)binding_node(&explicit_bindings[index]);
        bind_context_set(&explicit_bindings[index],0,chunk,
            chunk->type_sets,type_arguments[index]);
    }
    const size_t spread_argument_count=
        spread->count+(needs_receiver?1:0);
    if(spread_argument_count<function->required_arity||
       (spread_argument_count>function->arity&&!function->has_variadic))
        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    const DiamondValue *spread_arguments=spread->values;
    DiamondValue *padded=nullptr;
    if(needs_receiver) {
        if(spread->count>DIAMOND_REGISTER_COUNT-1)
            VM_RETURN(DIAMOND_VM_ARITY_ERROR);
        padded=malloc(spread_argument_count*sizeof *padded);
        if(padded==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
        padded[0]=class_index==DIAMOND_NO_CLASS?DIAMOND_NIL:
            DIAMOND_CLASS(class_index);
        for(size_t index=0;index<spread->count;index++)
            padded[index+1]=spread->values[index];
        spread_arguments=padded;
    }
    const DiamondChunk called_chunk={
        .name=function->name,.code=function->code,
        .lines=function->lines,.columns=function->columns,
        .code_count=function->code_count,
        .constants=function->constants,
        .constant_count=function->constant_count,
        .strings=function->strings,.string_count=function->string_count,
        .type_sets=function->type_sets,
        .type_set_count=function->type_set_count,
        .functions=chunk->functions,.function_count=chunk->function_count,
        .classes=chunk->classes,.class_count=chunk->class_count,
        .interfaces=chunk->interfaces,
        .interface_count=chunk->interface_count,
        .parameter_type_sets=function->parameter_type_sets,
        .type_variable_count=function->type_variable_count,
        .class_type_variable=function->class_type_variable,
        .binding_owner_plus_one=(uint16_t)(function->owner_class+1u),
        .parameter_offset=function->owner_class==DIAMOND_OWNER_NONE?0:1,
        .type_variable_bindings=type_argument_count==0?nullptr:
            explicit_bindings,
        .register_count=function->register_count,
        .has_variadic=function->has_variadic};
    DiamondValue call_result=DIAMOND_NIL;
    const DiamondVmStatus status=run_chunk(&called_chunk,vm,
        spread_arguments,spread_argument_count,depth+1,nullptr,
        &call_result);
    free(padded);VM_PROPAGATE(status);
    registers[destination]=call_result;HELPER_DONE;
}

/* DIAMOND_OP_NEW_SPREAD, moved out of run_chunk's switch (see HELPER_DONE below): the case body is
 * unchanged apart from its macros, so it keeps the operand decoding, sandbox check and error
 * returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_new_spread(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t depth,size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,spread_register=0;uint16_t class_index=0;
    READ_SHORT(dest);READ_SHORT(class_index);READ_SHORT(spread_register);
    if((size_t)class_index>=chunk->class_count)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    if(registers[spread_register].kind!=DIAMOND_VALUE_OBJECT||
       registers[spread_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,
            "spread argument (*expr) must be an Array");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondArray *spread=(const DiamondArray *)
        registers[spread_register].as.object;
    const DiamondVmStatus status=new_instance_from_array(vm,chunk,registers,dest,
        class_index,spread,DIAMOND_NO_TYPE_SET,depth);
    VM_PROPAGATE(status);HELPER_DONE;
}

DiamondVmStatus new_instance_from_array(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondValue *registers,uint16_t dest,uint16_t class_index,
        const DiamondArray *spread,uint16_t explicit_type,size_t depth) {
    if(class_index>=chunk->class_count)return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondClass *class=chunk->classes[class_index];
    /* The constructor arguments decide instead, as for `Box.new(...)`. */
    if(explicit_type!=DIAMOND_NO_TYPE_SET&&
       explicit_type_has_unbound_variable(chunk,explicit_type,0))
        explicit_type=DIAMOND_NO_TYPE_SET;
    DiamondInstance *instance=allocate_instance(vm,class,nullptr);
    if(instance==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    registers[dest]=DIAMOND_OBJECT(instance);
    const DiamondMethod *initialize=lookup_method(chunk,class,
        "initialize",sizeof("initialize")-1);
    if(instance->type_binding!=nullptr&&initialize==nullptr&&explicit_type==DIAMOND_NO_TYPE_SET&&!class->fixed_type_arguments)
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    if(explicit_type!=DIAMOND_NO_TYPE_SET) {
        if(instance->type_binding==nullptr||
           !bind_explicit_instance_types(chunk,explicit_type,instance)) {
            snprintf(vm->error,sizeof vm->error,"unsupported explicit class type argument");
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
    }
    if(initialize!=nullptr) {
        if(spread->count<initialize->required_arity||
           (spread->count>initialize->arity&&
            !initialize->has_variadic))
            VM_RETURN(DIAMOND_VM_ARITY_ERROR);
        if(spread->count>(size_t)DIAMOND_REGISTER_COUNT-1-
                initialize->bound_value_count)
            VM_RETURN(DIAMOND_VM_ARITY_ERROR);
        const size_t total=spread->count+1+
            initialize->bound_value_count;
        DiamondValue *args=malloc(total*sizeof *args);
        if(args==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
        args[0]=registers[dest];
        for(size_t index=0;index<spread->count;index++)
            args[index+1]=spread->values[index];
        for(size_t index=0;index<initialize->bound_value_count;index++)
            args[spread->count+1+index]=initialize->bound_values[index];
        const DiamondChunk *function_chunk=initialize->source_chunk!=nullptr?
            initialize->source_chunk:chunk;
        const DiamondFunction *fn=
            function_chunk->functions[initialize->function_index];
        if(instance->type_binding!=nullptr&&explicit_type==DIAMOND_NO_TYPE_SET) {
            const DiamondVmStatus bound=bind_instance_constructor_type(
                vm,function_chunk,fn,spread->values,spread->count,instance);
            if(bound!=DIAMOND_VM_OK) {free(args);VM_RETURN(bound);}
        }
        DiamondChunk child={.name=fn->name,.code=fn->code,
          .lines=fn->lines,.columns=fn->columns,
          .code_count=fn->code_count,.constants=fn->constants,
          .constant_count=fn->constant_count,.strings=fn->strings,
          .string_count=fn->string_count,.type_sets=fn->type_sets,
          .type_set_count=fn->type_set_count,
          .functions=function_chunk->functions,
          .function_count=function_chunk->function_count,
          .classes=function_chunk->classes,
          .class_count=function_chunk->class_count,
          .interfaces=function_chunk->interfaces,
          .interface_count=function_chunk->interface_count,
          .parameter_type_sets=fn->parameter_type_sets,
          .type_variable_count=fn->type_variable_count,
          .class_type_variable=fn->class_type_variable,
        .binding_owner_plus_one=(uint16_t)(fn->owner_class+1u),
          .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
          .register_count=fn->register_count,
          .has_variadic=fn->has_variadic};
        DiamondValue ignored=DIAMOND_NIL;
        const DiamondVmStatus status=run_chunk(&child,vm,args,total,
            depth+1,nullptr,&ignored);
        free(args);VM_PROPAGATE(status);
    } else {
        bool exception_class=false;const DiamondClass *ancestor=class;
        while(ancestor!=nullptr) {
            if(ancestor==chunk->classes[DIAMOND_CLASS_EXCEPTION]) {
                exception_class=true;break;
            }
            ancestor=ancestor->superclass==DIAMOND_NO_CLASS?nullptr:
                chunk->classes[ancestor->superclass];
        }
        if(exception_class) {
            if(spread->count>2)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
            if(spread->count>0)instance->fields[0]=spread->values[0];
            if(spread->count>1)instance->fields[1]=spread->values[1];
        } else if(spread->count!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    }
    return DIAMOND_VM_OK;
}

/* DIAMOND_OP_INVOKE_KEYWORDS, DIAMOND_OP_INVOKE_TYPED_KEYWORDS, moved out of run_chunk's switch
 * (see HELPER_DONE below): the case body is unchanged apart from its macros, so it keeps the
 * operand decoding, sandbox check and error returns it had. The caller applies VM_PROPAGATE to the
 * status. */
__attribute__((noinline))
DiamondVmStatus outlined_invoke_keywords(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t depth,uint8_t instruction,size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,recv=0,positional_register=0;
    uint16_t method_name_index=0,keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS];uint8_t keyword_count=0;
    uint16_t keyword_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
    READ_SHORT(dest);READ_SHORT(recv);READ_SHORT(method_name_index);
    READ_SHORT(positional_register);READ_BYTE(keyword_count);
    const bool has_block=(keyword_count&0x80u)!=0;
    keyword_count&=0x7fu;
    if((keyword_count==0&&!has_block)||keyword_count>DIAMOND_MAX_DECLARED_PARAMETERS||
       (size_t)method_name_index>=chunk->string_count)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    for(size_t index=0;index<keyword_count;index++) {
        READ_SHORT(keyword_names[index]);READ_SHORT(keyword_registers[index]);
    }
    uint16_t block_register=0;
    if(has_block)READ_SHORT(block_register);
    if(has_block&&block_register>=DIAMOND_REGISTER_COUNT)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    uint8_t type_count=0;uint16_t type_arguments[8];
    if((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_TYPED_KEYWORDS) {
        READ_BYTE(type_count);if(type_count>8)
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        for(size_t index=0;index<type_count;index++)READ_SHORT(type_arguments[index]);
    }
    if(registers[positional_register].kind!=DIAMOND_VALUE_OBJECT||
       registers[positional_register].as.object->kind!=DIAMOND_OBJECT_ARRAY)
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
       registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE) {
        const DiamondStringConstant *native_name=
            &chunk->strings[method_name_index];
        if(type_count!=0)VM_REJECT_TYPE_ARGUMENTS(native_name);
        if(has_block) {
            snprintf(vm->error,sizeof vm->error,
                "native keyword method cannot take a block");
            VM_RETURN(DIAMOND_VM_ARITY_ERROR);
        }
        const NativeKeywordSignature *signature=
            native_keyword_signature(native_name);
        if(signature==nullptr) {snprintf(vm->error,sizeof vm->error,
            "no parameter with this name");
            VM_RETURN(DIAMOND_VM_ARITY_ERROR);}
        DiamondValue *native_merged=nullptr;size_t native_count=0;
        const DiamondVmStatus native_merge=merge_native_keyword_arguments(vm,
            chunk,signature,(const DiamondArray *)
            registers[positional_register].as.object,keyword_names,
            keyword_registers,keyword_count,registers,&native_merged,
            &native_count);
        VM_PROPAGATE(native_merge);
        DiamondArray *native_array=allocate_array(vm,native_merged,native_count);
        free(native_merged);if(native_array==nullptr)
            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
        const size_t native_protected=vm->gc_protected_count;
        if(!gc_protect(vm,DIAMOND_OBJECT(native_array)))
            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
        uint8_t native_code[12]={DIAMOND_OP_INVOKE_SPREAD,
            0,2,0,0,(uint8_t)(method_name_index>>8),
            (uint8_t)method_name_index,0,1,
            DIAMOND_OP_RETURN,0,2};
        uint32_t native_locations[12]={0};
        DiamondChunk native_chunk=*chunk;native_chunk.class_type_variable=false;
        native_chunk.name="<native keyword invoke>";
        native_chunk.code=native_code;
        native_chunk.lines=native_locations;
        native_chunk.columns=native_locations;
        native_chunk.code_count=12;native_chunk.register_count=3;
        DiamondValue native_arguments[2]={registers[recv],
            DIAMOND_OBJECT(native_array)};
        DiamondValue native_result=DIAMOND_NIL;
        const DiamondVmStatus native_status=run_chunk(&native_chunk,vm,
            native_arguments,2,depth+1,nullptr,&native_result);
        gc_unprotect(vm,native_protected);VM_PROPAGATE(native_status);
        registers[dest]=native_result;HELPER_DONE;
    }
    DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
    const DiamondChunk *owner=instance->owner!=nullptr?
        instance->owner:vm->root_chunk;
    const DiamondStringConstant *method_name=
        &chunk->strings[method_name_index];
    const DiamondMethod *method=lookup_method(owner,instance->class,
        method_name->chars,method_name->length);
    if(method==nullptr) {
        /* Unlike the plain INVOKE opcodes, a keyword call has no
         * method_missing fallback: method_missing's own
         * `(name, args)` contract has no established shape for
         * keyword arguments, so this stays a hard error. */
        snprintf(vm->error,sizeof vm->error,
            "undefined method '%.*s' for an instance of %s",
            (int)method_name->length,method_name->chars,
            instance->class->name);
        diamond_append_method_suggestion(vm,owner,DIAMOND_TYPE_NONE,nullptr,0,
            instance->class,method_name->chars,method_name->length);
        VM_RETURN(DIAMOND_VM_NO_METHOD_ERROR);
    }
    const DiamondChunk *function_chunk=method->source_chunk!=nullptr?
        method->source_chunk:owner;
    const DiamondFunction *fn=
        function_chunk->functions[method->function_index];
    DiamondValue *merged=nullptr;size_t merged_count=0;
    const DiamondVmStatus merge_status=merge_keyword_arguments(vm,chunk,
        fn,(const DiamondArray *)registers[positional_register].as.object,
        keyword_names,keyword_registers,keyword_count,registers,
        method->arity,has_block?&registers[block_register]:nullptr,
        &merged,&merged_count);
    VM_PROPAGATE(merge_status);
    DiamondArray *merged_array=allocate_array(vm,merged,merged_count);
    free(merged);if(merged_array==nullptr)
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    const size_t protected_count=vm->gc_protected_count;
    if(!gc_protect(vm,DIAMOND_OBJECT(merged_array)))
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    uint8_t code[32]={0};size_t code_count=0;
#define KEYWORD_SHORT(value) do {code[code_count++]=(uint8_t)((value)>>8); \
    code[code_count++]=(uint8_t)(value);} while(false)
    code[code_count++]=(uint8_t)(type_count==0?
        DIAMOND_OP_INVOKE_SPREAD:DIAMOND_OP_INVOKE_TYPED_SPREAD);
    KEYWORD_SHORT(2);KEYWORD_SHORT(0);KEYWORD_SHORT(method_name_index);
    KEYWORD_SHORT(1);
    if(type_count>0) {code[code_count++]=type_count;
        for(size_t index=0;index<type_count;index++)
            KEYWORD_SHORT(type_arguments[index]);}
    code[code_count++]=DIAMOND_OP_RETURN;KEYWORD_SHORT(2);
#undef KEYWORD_SHORT
    uint32_t locations[32]={0};
    DiamondChunk synthetic=*chunk;synthetic.class_type_variable=false;synthetic.name="<keyword invoke>";
    synthetic.code=code;synthetic.lines=locations;
    synthetic.columns=locations;synthetic.code_count=code_count;
    synthetic.register_count=3;
    DiamondValue synthetic_arguments[2]={registers[recv],
        DIAMOND_OBJECT(merged_array)};DiamondValue call_result=DIAMOND_NIL;
    const DiamondVmStatus status=run_chunk(&synthetic,vm,
        synthetic_arguments,2,depth+1,nullptr,&call_result);
    gc_unprotect(vm,protected_count);VM_PROPAGATE(status);
    registers[dest]=call_result;HELPER_DONE;
}

/* A spread call on a native receiver: re-enters the INVOKE dispatch through a synthetic one-call
 * frame, so the native receiver kinds keep a single method matrix. Part of outlined_invoke_spread. */
static DiamondVmStatus native_spread_invoke_helper(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,
        DiamondValue receiver,const DiamondArray *spread,uint16_t name,uint8_t type_argument_count,
        const uint16_t *type_arguments,DiamondValue *result_out) {
    if(spread->count>16)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    DiamondValue *native_arguments=malloc((spread->count+1)*
        sizeof *native_arguments);
    if(native_arguments==nullptr)
        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    native_arguments[0]=receiver;
    for(size_t index=0;index<spread->count;index++)
        native_arguments[index+1]=spread->values[index];
    uint8_t synthetic_code[32]={0};size_t code_index=0;
    const uint16_t destination=(uint16_t)(spread->count+1);
#define SYNTHETIC_SHORT(value) do { \
    synthetic_code[code_index++]=(uint8_t)((value)>>8); \
    synthetic_code[code_index++]=(uint8_t)(value); \
} while(false)
    synthetic_code[code_index++]=(uint8_t)(
        type_argument_count==0?DIAMOND_OP_INVOKE:
        DIAMOND_OP_INVOKE_TYPED);
    SYNTHETIC_SHORT(destination);
    SYNTHETIC_SHORT(0);
    SYNTHETIC_SHORT(name);
    SYNTHETIC_SHORT(1);
    synthetic_code[code_index++]=(uint8_t)spread->count;
    if(type_argument_count>0) {
        synthetic_code[code_index++]=type_argument_count;
        for(size_t index=0;index<type_argument_count;index++)
            SYNTHETIC_SHORT(type_arguments[index]);
    }
    synthetic_code[code_index++]=DIAMOND_OP_RETURN;
    SYNTHETIC_SHORT(destination);
#undef SYNTHETIC_SHORT
    uint32_t synthetic_lines[32]={0};
    uint32_t synthetic_columns[32]={0};
    DiamondChunk synthetic={.name="<native spread>",
        .code=synthetic_code,.lines=synthetic_lines,
        .columns=synthetic_columns,.code_count=code_index,
        .constants=chunk->constants,
        .constant_count=chunk->constant_count,
        .strings=chunk->strings,.string_count=chunk->string_count,
        .type_sets=chunk->type_sets,
        .type_set_count=chunk->type_set_count,
        .functions=chunk->functions,
        .function_count=chunk->function_count,
        .classes=chunk->classes,.class_count=chunk->class_count,
        .interfaces=chunk->interfaces,
        .interface_count=chunk->interface_count,
        .modules=chunk->modules,.module_count=chunk->module_count,
        .type_variable_bindings=chunk->type_variable_bindings,
        .register_count=(uint16_t)(spread->count+2),
        .range_class_index=chunk->range_class_index};
    DiamondValue native_result=DIAMOND_NIL;
    const DiamondVmStatus native_status=run_chunk(&synthetic,vm,
        native_arguments,spread->count+1,depth+1,nullptr,
        &native_result);
    free(native_arguments);VM_PROPAGATE(native_status);
    *result_out=native_result;
    return DIAMOND_VM_OK;
}

/* DIAMOND_OP_INVOKE_SPREAD, DIAMOND_OP_INVOKE_TYPED_SPREAD, moved out of run_chunk's switch (see
 * HELPER_DONE below): the case body is unchanged apart from its macros, so it keeps the operand
 * decoding, sandbox check and error returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_invoke_spread(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t depth,uint8_t instruction,size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t dest=0,recv=0,spread_register=0,name=0;
    READ_SHORT(dest);READ_SHORT(recv);READ_SHORT(name);
    READ_SHORT(spread_register);
    uint8_t type_argument_count=0;uint16_t type_arguments[8];
    if((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_TYPED_SPREAD) {
        READ_BYTE(type_argument_count);
        if(type_argument_count>8)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        for(size_t index=0;index<type_argument_count;index++)
            READ_SHORT(type_arguments[index]);
    }
    if((size_t)name>=chunk->string_count)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    if(registers[spread_register].kind!=DIAMOND_VALUE_OBJECT||
       registers[spread_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,
            "spread argument (*expr) must be an Array");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    const DiamondArray *spread=(const DiamondArray *)
        registers[spread_register].as.object;
    const DiamondStringConstant *method_name=&chunk->strings[name];
    const bool instance_receiver=registers[recv].kind==DIAMOND_VALUE_OBJECT&&
        registers[recv].as.object->kind==DIAMOND_OBJECT_INSTANCE;
    bool user_public_send=false;
    if(instance_receiver) {
        const DiamondInstance *candidate=
            (const DiamondInstance *)registers[recv].as.object;
        const DiamondChunk *candidate_owner=candidate->owner!=nullptr?
            candidate->owner:vm->root_chunk;
        user_public_send=lookup_method(candidate_owner,candidate->class,
            "public_send",11)!=nullptr;
    }
    if(method_name->length==11&&
       memcmp(method_name->chars,"public_send",11)==0&&
       !user_public_send) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        DiamondValue sent=DIAMOND_NIL;
        const DiamondVmStatus send_status=public_send_helper(vm,chunk,
            registers[recv],spread->values,spread->count,depth,&sent);
        VM_PROPAGATE(send_status);
        registers[dest]=sent;HELPER_DONE;
    }
    if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
       registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE) {
        /* Native receivers already share one deliberately ordered
         * INVOKE matrix below (universal methods, built-ins, then
         * collection extensions). Re-enter that matrix through a
         * verifier-safe synthetic frame instead of cloning its
         * many receiver branches here. The ordinary INVOKE format
         * has an 8-bit count and currently caps calls at 16 args. */
        DiamondValue native_result=DIAMOND_NIL;
        const DiamondVmStatus native_status=native_spread_invoke_helper(vm,chunk,depth,registers[recv],
            spread,name,type_argument_count,type_arguments,&native_result);
        VM_PROPAGATE(native_status);
        registers[dest]=native_result;HELPER_DONE;
    }
    DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
    const DiamondChunk *owner=
        instance->owner!=nullptr?instance->owner:vm->root_chunk;
    const DiamondMethod *method=lookup_method(owner,instance->class,
        method_name->chars,method_name->length);
    if(method==nullptr) {
        DiamondValue missing_result=DIAMOND_NIL;bool missing_found=false;
        const DiamondVmStatus missing_status=method_missing_helper(vm,owner,
            instance,method_name->chars,method_name->length,
            spread->values,spread->count,depth,&missing_result,&missing_found);
        if(missing_found) {
            VM_PROPAGATE(missing_status);
            registers[dest]=missing_result;HELPER_DONE;
        }
        snprintf(vm->error,sizeof vm->error,
            "undefined method '%.*s' for an instance of %s",
            (int)method_name->length,method_name->chars,
            instance->class->name);
        diamond_append_method_suggestion(vm,owner,DIAMOND_TYPE_NONE,nullptr,0,
            instance->class,method_name->chars,method_name->length);
        VM_RETURN(DIAMOND_VM_NO_METHOD_ERROR);
    }
    if(method->is_private&&!(chunk->parameter_offset==1&&recv==0)) {
        snprintf(vm->error,sizeof vm->error,
            "private method '%.*s' called with an explicit receiver",
            (int)method_name->length,method_name->chars);
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(method->is_protected) {
        const DiamondClass *declaring=method_declaring_class(
            owner,instance->class,method);
        const bool has_method_self=chunk->parameter_offset==1&&
            registers[0].kind==DIAMOND_VALUE_OBJECT&&
            registers[0].as.object->kind==DIAMOND_OBJECT_INSTANCE;
        const DiamondClass *caller_class=has_method_self?
            ((DiamondInstance *)registers[0].as.object)->class:nullptr;
        if(declaring==nullptr||caller_class==nullptr||
           !class_is_a(owner,caller_class,declaring)) {
            snprintf(vm->error,sizeof vm->error,
                "protected method '%.*s' called outside its class hierarchy",
                (int)method_name->length,method_name->chars);
            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
        }
    }
    if(spread->count<method->required_arity||
       (spread->count>method->arity&&!method->has_variadic))
        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    if(spread->count>(size_t)DIAMOND_REGISTER_COUNT-1-
            method->bound_value_count)
        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    const size_t total_args=spread->count+1+method->bound_value_count;
    DiamondValue *args=malloc(total_args*sizeof *args);
    if(args==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    args[0]=registers[recv];
    for(size_t index=0;index<spread->count;index++)
        args[index+1]=spread->values[index];
    for(size_t index=0;index<method->bound_value_count;index++)
        args[spread->count+1+index]=method->bound_values[index];
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner;
    const DiamondFunction *fn=
        function_chunk->functions[method->function_index];
    const size_t class_offset=fn->class_type_variable;
    if(type_argument_count>0&&(fn->type_variable_count>8||
       fn->type_variable_count<class_offset||
       type_argument_count!=fn->type_variable_count-class_offset)) {
        if(fn->class_type_variable&&fn->type_variable_count==fn->class_type_variable)
            snprintf(vm->error,sizeof vm->error,
                "generic class type arguments are fixed by construction");
        free(args);VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    /* Only the first type_argument_count entries are ever read (below,
     * and through .type_variable_bindings); zeroing all eight cost about
     * 3 KB of memset on every call, typed or not. */
    DiamondTypeBinding explicit_bindings[type_argument_count>0?fn->type_variable_count:1];
    if(type_argument_count>0&&class_offset!=0)memset(explicit_bindings,0,class_offset*sizeof explicit_bindings[0]);
    for(size_t index=0;index<type_argument_count;index++) {
        DiamondTypeBinding *binding=&explicit_bindings[index+class_offset];
        *binding=(DiamondTypeBinding){};
        if((size_t)type_arguments[index]>=chunk->type_set_count) {
            free(args);VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        }
        (void)binding_node(binding);
        bind_context_set(binding,0,chunk,
            chunk->type_sets,type_arguments[index]);
    }
    DiamondChunk child={.name=fn->name,.code=fn->code,
        .lines=fn->lines,.columns=fn->columns,
        .code_count=fn->code_count,.constants=fn->constants,
        .constant_count=fn->constant_count,.strings=fn->strings,
        .string_count=fn->string_count,.type_sets=fn->type_sets,
        .type_set_count=fn->type_set_count,
        .functions=function_chunk->functions,
        .function_count=function_chunk->function_count,
        .classes=function_chunk->classes,
        .class_count=function_chunk->class_count,
        .interfaces=function_chunk->interfaces,
        .interface_count=function_chunk->interface_count,
        .parameter_type_sets=fn->parameter_type_sets,
        .type_variable_count=fn->type_variable_count,
        .class_type_variable=fn->class_type_variable,
        .binding_owner_plus_one=(uint16_t)(fn->owner_class+1u),
        .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
        .type_variable_bindings=type_argument_count==0?nullptr:
            explicit_bindings,
        .register_count=fn->register_count,
        .has_variadic=fn->has_variadic};
    DiamondValue call_result=DIAMOND_NIL;
    const DiamondVmStatus spread_status=run_chunk(&child,vm,args,
        total_args,depth+1,nullptr,&call_result);
    free(args);
    VM_PROPAGATE(spread_status);
    registers[dest]=call_result;HELPER_DONE;
}

/* tap() on a native receiver, moved out of run_chunk's INVOKE case (the name check stays inline).
 * The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_invoke_tap(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,size_t depth) {
    if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_CLOSURE)
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    DiamondClosure *called=(DiamondClosure *)registers[base].as.object;
    /* See DIAMOND_OP_CALL_CLOSURE's own comment. */
    if(called->foreign_chunk!=nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "a compile_method callable can only be passed to define_method");
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
    }
    if(called->function_index>=chunk->function_count)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    const DiamondFunction *fn=chunk->functions[called->function_index];
    DiamondValue tap_argument[1]={registers[recv]};
    DiamondValue tap_result=DIAMOND_NIL;
    const DiamondVmStatus tap_status=call_closure_helper(vm,chunk,fn,
        called,tap_argument,0,1,depth,&tap_result);
    VM_PROPAGATE(tap_status);
    registers[dest]=registers[recv];return DIAMOND_VM_OK;
}

/* Array#shuffle and #sample, moved out of run_chunk's INVOKE case (the name check stays inline).
 * The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_array_shuffle_sample(DiamondVm *vm,DiamondValue *registers,uint16_t recv,uint16_t dest,
        const DiamondStringConstant *method_name) {
    const DiamondArray *source=(const DiamondArray *)registers[recv].as.object;
    const bool sample=method_name->length==6;
    if(sample&&source->count==0) {registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;}
    DiamondArray *shuffled=sample?nullptr:
        allocate_array(vm,source->values,source->count);
    if(!sample&&shuffled==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    const size_t rounds=sample?1:(shuffled->count>0?shuffled->count-1:0);
    for(size_t round=0;round<rounds;round++) {
        const uint64_t bound=sample?(uint64_t)source->count:
            (uint64_t)(shuffled->count-round);
        const uint64_t limit=UINT64_MAX-UINT64_MAX%bound;
        uint64_t draw=0;
        do {
            if(RAND_bytes((unsigned char *)&draw,sizeof draw)!=1) {
                snprintf(vm->error,sizeof vm->error,
                    "Array#%s: RAND_bytes failed",sample?"sample":"shuffle");
                VM_RETURN(DIAMOND_VM_IO_ERROR);
            }
        } while(draw>=limit);
        const size_t pick=(size_t)(draw%bound);
        if(sample) {registers[dest]=source->values[pick];break;}
        const size_t last=shuffled->count-1-round;
        const DiamondValue held=shuffled->values[last];
        shuffled->values[last]=shuffled->values[pick];
        shuffled->values[pick]=held;
    }
    if(!sample)registers[dest]=DIAMOND_OBJECT(shuffled);
    return DIAMOND_VM_OK;
}

/* Callable#arity, moved out of run_chunk's INVOKE case (the name check stays inline). The caller
 * applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_callable_arity(const DiamondChunk *chunk,DiamondValue *registers,
        uint16_t recv,uint16_t dest) {
    const DiamondClosure *callable=
        (const DiamondClosure *)registers[recv].as.object;
    const DiamondChunk *owner=callable->foreign_chunk!=nullptr?
        callable->foreign_chunk:chunk;
    if(callable->function_index>=owner->function_count)
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
    const DiamondFunction *fn=owner->functions[callable->function_index];
    const uint8_t declared=fn->has_variadic?fn->required_arity:fn->arity;
    registers[dest]=DIAMOND_INT((int64_t)declared-
        (int64_t)diamond_function_self_offset(fn));
    return DIAMOND_VM_OK;
}

/* Int#to_s(base) on a bignum, moved out of run_chunk's INVOKE case (the name check stays inline).
 * The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_bignum_to_s_base(DiamondVm *vm,DiamondValue *registers,uint16_t recv,uint16_t base,
        uint16_t dest) {
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[base].as.integer<2||registers[base].as.integer>36) {
        snprintf(vm->error,sizeof vm->error,
            "Int#to_s base must be an Int from 2 to 36");
        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
    }
    DiamondIntView view;
    diamond_int_view(registers[recv],&view);
    size_t digit_count=0;
    char *digits=diamond_bignum_to_radix_string(view,
        (int)registers[base].as.integer,&digit_count);
    if(digits==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    DiamondString *formatted=allocate_string(vm,digits,digit_count);
    free(digits);
    if(formatted==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
    registers[dest]=DIAMOND_OBJECT(formatted);
    return DIAMOND_VM_OK;
}

/* to_s/to_i/to_f/abs on a bignum, moved out of run_chunk's INVOKE case (the name check stays
 * inline). The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_bignum_convert(DiamondVm *vm,DiamondValue *registers,uint16_t recv,uint16_t dest,
        const DiamondStringConstant *method_name) {
    DiamondIntView view;
    diamond_int_view(registers[recv],&view);
    if(method_name->chars[0]=='a') {
        if(view.negative) {
            const DiamondValue positive=diamond_bignum_negate(vm,view);
            if(positive.kind==DIAMOND_VALUE_NIL)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
            registers[dest]=positive;
        } else registers[dest]=registers[recv];
    } else if(method_name->chars[3]=='i') {
        registers[dest]=registers[recv];
    } else if(method_name->chars[3]=='f') {
        registers[dest]=DIAMOND_FLOAT(diamond_bignum_to_double(
            (const DiamondBignum *)registers[recv].as.object));
    } else {
        StringBuilder text={};
        if(!builder_format_value(&text,registers[recv])) {
            free(text.chars);VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
        }
        DiamondString *formatted=allocate_string(vm,text.chars,text.length);
        free(text.chars);
        if(formatted==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
        registers[dest]=DIAMOND_OBJECT(formatted);
    }
    return DIAMOND_VM_OK;
}

/* run_chunk's INVOKE dispatch for the native handle kinds from Channel through ProgramBuilder, moved
 * out of the case. Sets *handled to false, returning DIAMOND_VM_OK, when receiver_kind is none of
 * them; the caller skips the call for an Instance. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_native_object_invoke(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,
        DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,
        const DiamondStringConstant *method_name,uint8_t type_argument_count,DiamondObjectKind receiver_kind,
        bool *handled) {
    *handled=true;
    if(receiver_kind==DIAMOND_OBJECT_CHANNEL) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=channel_invoke_helper(
            vm,chunk,registers,recv,base,argc,dest,method_name);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_SUPERVISOR) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=supervisor_invoke_helper(
            vm,chunk,registers,recv,base,argc,dest,method_name);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_FILE) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=file_invoke_helper(
            vm,chunk,depth,registers,recv,base,argc,dest,method_name);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_LISTENER) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=listener_invoke_helper(
            vm,chunk,depth,registers,recv,argc,dest,method_name);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_SOCKET) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=socket_invoke_helper(
            vm,chunk,depth,registers,recv,base,argc,dest,method_name);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_UDP_SOCKET) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=udp_socket_invoke_helper(
            vm,chunk,depth,registers,recv,base,argc,dest,method_name);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_TLS_SOCKET) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=tls_socket_invoke_helper(
            vm,chunk,depth,registers,recv,base,argc,dest,method_name);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_REGEXP) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=regexp_invoke_helper(
            vm,registers,recv,base,argc,dest,method_name);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_SQLITE3) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=sqlite3_dispatch_helper(vm,
            (DiamondSqlite3Handle *)registers[recv].as.object,
            method_name,registers,base,argc,dest);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_TENSOR) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=tensor_dispatch_helper(vm,
            (DiamondTensor *)registers[recv].as.object,
            method_name,registers,base,argc,dest);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_SQLITE3_STATEMENT) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=sqlite3_statement_dispatch_helper(vm,
            (DiamondSqlite3StatementHandle *)registers[recv].as.object,
            method_name,registers,base,argc,dest);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_POSTGRES) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=postgres_dispatch_helper(vm,
            (DiamondPostgresHandle *)registers[recv].as.object,
            method_name,registers,base,argc,dest);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_MYSQL) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=mysql_dispatch_helper(vm,
            (DiamondMysqlHandle *)registers[recv].as.object,
            method_name,registers,base,argc,dest);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_TIME) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=time_dispatch_helper(vm,
            (DiamondTime *)registers[recv].as.object,
            method_name,registers,base,argc,dest);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_PROCESS_RESULT) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=process_result_dispatch_helper(vm,
            (DiamondProcessResult *)registers[recv].as.object,
            method_name,registers,argc,dest);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_PROCESS_HANDLE) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=process_handle_dispatch_helper(vm,
            (DiamondProcessHandle *)registers[recv].as.object,
            method_name,registers,argc,dest);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_PROCESS_STREAM) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        const DiamondVmStatus dispatch_status=process_stream_dispatch_helper(vm,
            (DiamondProcessStream *)registers[recv].as.object,
            method_name,registers,argc,base,dest);
        VM_PROPAGATE(dispatch_status);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_PROGRAM_BUILDER) {
        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
        DiamondValue invoke_result=DIAMOND_NIL;
        const DiamondVmStatus invoke_status=program_builder_invoke_helper(vm,
            (DiamondProgramBuilder *)registers[recv].as.object,method_name,
            registers,base,argc,depth,&invoke_result);
        VM_PROPAGATE(invoke_status);
        registers[dest]=invoke_result;return DIAMOND_VM_OK;
    }
    *handled=false;
    return DIAMOND_VM_OK;
}

/* DIAMOND_OP_BREAKPOINT_CHECK, moved out of run_chunk's switch (see HELPER_DONE below): the case
 * body is unchanged apart from its macros, so it keeps the operand decoding, sandbox check and
 * error returns it had. The caller applies VM_PROPAGATE to the status. */
__attribute__((noinline))
DiamondVmStatus outlined_breakpoint_check(DiamondVm *vm,const DiamondChunk *chunk,DiamondValue *registers,
        size_t depth,size_t instruction_offset,size_t *ip_ref) {
    size_t ip=*ip_ref;
    uint16_t destination=0;uint8_t local_count=0;
    READ_SHORT(destination);READ_BYTE(local_count);
    uint64_t source_line_offset=0;
    for(size_t index=0;index<8;index++) {
        uint8_t next=0;READ_BYTE(next);
        source_line_offset=(source_line_offset<<8)|next;
    }
    uint16_t name_indices[DIAMOND_MAX_LOCALS];
    uint16_t local_registers[DIAMOND_MAX_LOCALS];
    for(size_t index=0;index<local_count;index++) {
        READ_SHORT(name_indices[index]);
        READ_SHORT(local_registers[index]);
    }
    /* Non-blocking: a live `setBreakpoints` sent while this
     * program is running (not currently paused at any
     * breakpoint) has nobody else reading vm->debug_fd to
     * receive it -- see DiamondVm.debug_active_lines's own
     * comment (src/vm.h) and docs/debugging.md's "live
     * breakpoints" section for why this has to happen right
     * here, at every statement, rather than via a background
     * thread. Applies every currently-buffered command
     * (there could be more than one queued up) before
     * deciding whether *this* statement's own line is armed.
     * Throttled to every DIAMOND_DEBUG_POLL_STATEMENT_MASK+1'th
     * statement: an unthrottled poll() per statement tripled
     * run time under a debug session. A new `setBreakpoints`
     * lands at most that many statements late. */
    if(vm->debug_fd>=0&&
       (vm->debug_poll_counter++&DIAMOND_DEBUG_POLL_STATEMENT_MASK)==0) {
        struct pollfd poll_fd={.fd=vm->debug_fd,.events=POLLIN};
        while(poll(&poll_fd,1,0)>0&&(poll_fd.revents&POLLIN)!=0) {
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
            }
            poll_fd.revents=0;
        }
    }
    const bool in_bounds=instruction_offset<chunk->code_count;
    const uint32_t statement_line=in_bounds&&chunk->lines!=nullptr?
        chunk->lines[instruction_offset]:0;
    bool armed=false;
    for(size_t index=0;index<vm->debug_active_line_count;index++)
        if(vm->debug_active_lines[index]==(vm->debug_breakpoints_are_offsets?
                (size_t)source_line_offset:(size_t)statement_line)) { armed=true; break; }
    /* Real stepping (docs/debugging.md's own "Stepping"
     * section): an armed breakpoint line always wins/pauses
     * regardless (checked above, unconditionally) -- this is
     * only a fallback for when that check didn't already
     * decide to pause. See DiamondVm.debug_step_mode's own
     * comment (src/vm.h) for the full semantics; `depth` is
     * this exact run_chunk invocation's own already-tracked
     * recursion depth, needing no new state to read. */
    bool stepped=false;
    if(!armed&&vm->debug_step_mode!=DIAMOND_STEP_NONE) {
        switch(vm->debug_step_mode) {
            case DIAMOND_STEP_IN:
                /* A named target (stepInTargets) pauses only on
                 * entering that callee one level down; if it
                 * turns out to be native, or never runs, the
                 * next statement at this depth or shallower
                 * stops instead, as `next` would. */
                if(vm->debug_step_in_target[0]=='\0')stepped=true;
                else stepped=depth<=vm->debug_step_target_depth||
                    (depth==vm->debug_step_target_depth+1&&chunk->name!=nullptr&&
                     strcmp(chunk->name,vm->debug_step_in_target)==0);
                break;
            case DIAMOND_STEP_OVER:
                stepped=depth<=vm->debug_step_target_depth; break;
            case DIAMOND_STEP_OUT:
                stepped=depth<vm->debug_step_target_depth; break;
            default: break;
        }
    }
    if(armed||stepped) {
        vm->debug_step_mode=DIAMOND_STEP_NONE;
        vm->debug_step_in_target[0]='\0';
        const DiamondVmStatus debugger_status=debugger_helper(vm,chunk,depth,
            instruction_offset,(size_t)source_line_offset,registers,name_indices,local_registers,local_count,
            armed?"breakpoint":"step");
        VM_PROPAGATE(debugger_status);
    }
    registers[destination]=DIAMOND_NIL;
    HELPER_DONE;
}

#undef HELPER_DONE
#undef READ_SHORT
#undef READ_BYTE
#undef VM_SANDBOX_GUARD
#undef VM_REJECT_TYPE_ARGUMENTS
#undef VM_PROPAGATE_SIGNAL
#undef VM_PROPAGATE
#undef VM_RETURN
