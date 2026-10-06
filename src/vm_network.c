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

/* Formats the current head of OpenSSL's thread-local error queue (and
 * drains it -- ERR_error_string_n reads then implicitly leaves the queue
 * alone, so a real ERR_get_error() pop is needed first, or a stale error
 * from an earlier, unrelated failed call could be reported here instead
 * of the one that actually just happened). Used for every TLS failure
 * that isn't itself a plain errno/getaddrinfo-style failure. */
void tls_format_error(char *buffer,size_t buffer_size) {
    const unsigned long code=ERR_get_error();
    if(code==0) {
        (void)snprintf(buffer,buffer_size,"unknown TLS error");
        return;
    }
    ERR_error_string_n(code,buffer,buffer_size);
}

/* Per RFC 7301, each ALPN protocol name must be 1-255 bytes; an empty
 * Array, an empty name, or a name over 255 bytes is a caller mistake
 * (TypeError), not something to silently truncate or skip. Split out
 * from tls_encode_alpn_protocols below so TLSSocket.connect can run this
 * cheap, no-network-needed check before ever touching the network (the
 * same "validate cheap input first" reasoning its own cert/key-pairing
 * check already follows), while the actual wire-format encoding still
 * happens later, once a TLS context exists to hand it to. */
DiamondVmStatus tls_validate_alpn_protocols(DiamondVm *vm,const char *owner,
        const DiamondArray *protocols) {
    if(protocols->count==0) {
        snprintf(vm->error,sizeof vm->error,"%s: alpn must not be empty",owner);
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t index=0;index<protocols->count;index++) {
        const DiamondValue element=protocols->values[index];
        if(element.kind!=DIAMOND_VALUE_OBJECT||
           element.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,"%s: alpn must be an Array of Strings",owner);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *name=(const DiamondString *)element.as.object;
        if(name->length==0||name->length>255) {
            snprintf(vm->error,sizeof vm->error,
                "%s: alpn protocol names must be 1-255 bytes",owner);
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    return DIAMOND_VM_OK;
}

/* Encodes `protocols` (an Array of Strings, ALPN protocol names in
 * preference order) into OpenSSL's ALPN wire format -- a flat buffer of
 * (1-byte length, that many bytes) entries, exactly what
 * SSL_CTX_set_alpn_protos (client) and SSL_select_next_proto (server,
 * inside tls_alpn_select_callback below) both expect. Shared by
 * TLSSocket.connect's `alpn` option and TLSServer.listen's own -- the
 * two ends of the same negotiation, needing the identical encoding. */
DiamondVmStatus tls_encode_alpn_protocols(DiamondVm *vm,const char *owner,
        const DiamondArray *protocols,unsigned char **out_buffer,unsigned int *out_length) {
    const DiamondVmStatus validate_status=tls_validate_alpn_protocols(vm,owner,protocols);
    if(validate_status!=DIAMOND_VM_OK)return validate_status;
    size_t total=0;
    for(size_t index=0;index<protocols->count;index++) {
        const DiamondString *name=
            (const DiamondString *)protocols->values[index].as.object;
        total+=1+name->length;
    }
    unsigned char *buffer=malloc(total);
    if(buffer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    size_t offset=0;
    for(size_t index=0;index<protocols->count;index++) {
        const DiamondString *name=
            (const DiamondString *)protocols->values[index].as.object;
        buffer[offset]=(unsigned char)name->length;
        memcpy(buffer+offset+1,name->chars,name->length);
        offset+=1+name->length;
    }
    *out_buffer=buffer;
    *out_length=(unsigned int)total;
    return DIAMOND_VM_OK;
}

/* SSL_CTX_sess_set_new_cb's own callback, registered on every
 * TLSSocket.connect's private per-connection SSL_CTX (see
 * DIAMOND_OP_TLS_CONNECT) so a resumable session -- including a TLS 1.3
 * session ticket, which typically arrives as a post-handshake message
 * processed asynchronously during a later SSL_read, not synchronously
 * inside SSL_connect itself -- is captured whenever it actually becomes
 * available, for TLSSocket#session to hand back later. Correlates back
 * to the right DiamondTlsSocketHandle via SSL_get_app_data (ex_data slot
 * 0, no custom index registration needed) rather than assuming "this is
 * the only connection that will ever use this callback" -- true today
 * (each connection's SSL_CTX is private and freed right after SSL_new),
 * but this stays correct even if that ever changes. Returning 1 means
 * this callback has taken ownership of `session`'s reference (freed on
 * #close/GC sweep, or immediately below if superseded by a later
 * ticket); returning 0 tells OpenSSL to free it itself instead, for the
 * (should-never-happen) case app_data isn't set yet. */
int tls_new_session_callback(SSL *ssl,SSL_SESSION *session) {
    DiamondTlsSocketHandle *handle=(DiamondTlsSocketHandle *)SSL_get_app_data(ssl);
    if(handle==nullptr)return 0;
    if(handle->received_session!=nullptr)SSL_SESSION_free(handle->received_session);
    handle->received_session=session;
    return 1;
}

/* SSL_CTX_set_alpn_select_cb's own callback, registered only when
 * TLSServer.listen's own `alpn` option is given (see tls_listen_helper).
 * `arg` is exactly the DiamondListenerHandle this listener's own
 * alpn_protocols/alpn_protocols_length live on -- passed straight
 * through by OpenSSL from SSL_CTX_set_alpn_select_cb's own registration,
 * so (unlike the client-side new-session callback above) no ex_data
 * correlation is needed here; the API already threads it through.
 * SSL_select_next_proto picks the first of the server's own preference-
 * ordered list (`arg`) that the client also offered (`in`) -- the
 * standard, RFC-recommended selection algorithm, not a custom one. A
 * client that offered no protocol the server supports is a hard TLS
 * alert (SSL_TLSEXT_ERR_ALERT_FATAL): with no shared protocol at all,
 * the connection has nothing meaningful to negotiate down to. */
static int tls_alpn_select_callback(SSL *ssl,const unsigned char **out,
        unsigned char *out_length,const unsigned char *in,unsigned int in_length,void *arg) {
    (void)ssl;
    const DiamondListenerHandle *listener=(const DiamondListenerHandle *)arg;
    const int select_status=SSL_select_next_proto((unsigned char **)(const void *)out,out_length,
        listener->alpn_protocols,listener->alpn_protocols_length,in,in_length);
    if(select_status!=OPENSSL_NPN_NEGOTIATED)return SSL_TLSEXT_ERR_ALERT_FATAL;
    return SSL_TLSEXT_ERR_OK;
}

/* Shared behind UDPSocket.bind(port)/UDPSocket.open(). bind_socket==true
 * (UDPSocket.bind) resolves and binds to a specific local port, same
 * getaddrinfo/AI_PASSIVE/try-each-candidate dance as tcp_listen_helper
 * below, minus the listen(2) call UDP has no equivalent of -- one socket
 * both sends and receives, so once bound there's nothing more to set up.
 * bind_socket==false (UDPSocket.open) just opens a plain AF_INET socket
 * with no local address, letting the OS assign an ephemeral port on
 * first use -- the common "client that doesn't care what port it sends
 * from" case. Deliberately AF_INET only (not AF_UNSPEC/getaddrinfo, which
 * needs a destination to resolve against and open() has none yet): an
 * unbound socket created this way can only reach IPv4 destinations from
 * a later .send(data, host, port) -- a real, documented scope cut, not
 * an oversight (see docs/io.md). */
DiamondVmStatus udp_socket_helper(DiamondVm *vm,bool bind_socket,
        int64_t port,DiamondUdpSocketHandle **out_handle) {
    int fd=-1;
    if(bind_socket) {
        char port_text[32];
        (void)snprintf(port_text,sizeof port_text,"%" PRId64,port);
        struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_DGRAM,
            .ai_flags=AI_PASSIVE};
        struct addrinfo *results=nullptr;
        const int resolve_status=getaddrinfo(nullptr,port_text,&hints,&results);
        if(resolve_status!=0) {
            snprintf(vm->error,sizeof vm->error,"cannot bind UDP port %s: %s",
                     port_text,gai_strerror(resolve_status));
            return DIAMOND_VM_IO_ERROR;
        }
        int last_errno=0;
        for(struct addrinfo *candidate=results;candidate!=nullptr;
            candidate=candidate->ai_next) {
            const int candidate_fd=socket(candidate->ai_family,candidate->ai_socktype,
                                 candidate->ai_protocol);
            if(candidate_fd<0) {last_errno=errno;continue;}
            /* Same IPV6_V6ONLY fix as tcp_listen_helper above, same reason:
             * an IPv6-wildcard UDP bind needs this to also reach an IPv4
             * .send() on FreeBSD/OpenBSD (net.inet6.ip6.v6only=1 by
             * default there, unlike Linux). */
            if(candidate->ai_family==AF_INET6) {
                const int v6only_off=0;
                (void)setsockopt(candidate_fd,IPPROTO_IPV6,IPV6_V6ONLY,
                                  &v6only_off,sizeof v6only_off);
            }
            if(bind(candidate_fd,candidate->ai_addr,candidate->ai_addrlen)==0) {
                fd=candidate_fd;break;
            }
            last_errno=errno;close(candidate_fd);
        }
        freeaddrinfo(results);
        if(fd<0) {
            snprintf(vm->error,sizeof vm->error,"cannot bind UDP port %s: %s",
                     port_text,strerror(last_errno));
            return DIAMOND_VM_IO_ERROR;
        }
    } else {
        fd=socket(AF_INET,SOCK_DGRAM,0);
        if(fd<0) {
            snprintf(vm->error,sizeof vm->error,"cannot open UDP socket: %s",strerror(errno));
            return DIAMOND_VM_IO_ERROR;
        }
    }
    DiamondUdpSocketHandle *handle=allocate_udp_socket_handle(vm,fd);
    if(handle==nullptr) {
        close(fd);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *out_handle=handle;
    return DIAMOND_VM_OK;
}

/* Shared getaddrinfo/socket/connect dance behind TCPSocket.connect and
 * TLSSocket.connect -- both need a connected fd before doing anything
 * TLS-specific, so this is exactly the code TCP_CONNECT's own handler
 * used to have inline, unchanged, just callable from a second opcode
 * handler now too. No SA_RESTART-style signal-retry here, matching the
 * existing TCPSocket.connect scope cut documented in docs/io.md: a
 * signal arriving mid-connect makes the attempt fail rather than
 * transparently resuming, same as it always has.
 *
 * `connect_timeout_ms` of -1 means "block indefinitely", the original,
 * unchanged behavior -- the fd is never made non-blocking in that case,
 * so this costs nothing for every existing caller that doesn't pass one.
 * A real timeout applies per candidate address, not as one aggregate
 * deadline across every candidate `getaddrinfo` returns -- matching how
 * the existing try-each-candidate loop already treats an ordinary
 * connection refusal (move on to the next candidate, not a hard stop). */
DiamondVmStatus tcp_connect_helper(DiamondVm *vm,const DiamondString *host,
        int64_t port,int64_t connect_timeout_ms,int *out_fd) {
    char port_text[32];
    (void)snprintf(port_text,sizeof port_text,"%" PRId64,port);
    struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM};
    struct addrinfo *results=nullptr;
    const int resolve_status=getaddrinfo(host->chars,port_text,&hints,&results);
    if(resolve_status!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot connect to '%.*s:%s': %s",
                 (int)host->length,host->chars,port_text,gai_strerror(resolve_status));
        return DIAMOND_VM_IO_ERROR;
    }
    int connected_fd=-1;
    int last_errno=0;
    for(struct addrinfo *candidate=results;candidate!=nullptr;
        candidate=candidate->ai_next) {
        const int fd=socket(candidate->ai_family,candidate->ai_socktype,
                             candidate->ai_protocol);
        if(fd<0) {last_errno=errno;continue;}
        if(connect_timeout_ms>=0) {
            const int flags=fcntl(fd,F_GETFL,0);
            if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) {
                last_errno=errno;close(fd);continue;
            }
            if(connect_with_timeout(fd,candidate,connect_timeout_ms)!=0) {
                last_errno=errno;close(fd);continue;
            }
            if(fcntl(fd,F_SETFL,flags)<0) {last_errno=errno;close(fd);continue;}
            connected_fd=fd;break;
        }
        if(connect(fd,candidate->ai_addr,candidate->ai_addrlen)==0) {
            connected_fd=fd;break;
        }
        last_errno=errno;close(fd);
    }
    freeaddrinfo(results);
    if(connected_fd<0) {
        snprintf(vm->error,sizeof vm->error,"cannot connect to '%.*s:%s': %s",
                 (int)host->length,host->chars,port_text,strerror(last_errno));
        return DIAMOND_VM_IO_ERROR;
    }
    *out_fd=connected_fd;
    return DIAMOND_VM_OK;
}

#define DIAMOND_RESOLVER_LIMIT 8
static atomic_uint diamond_active_resolvers=0;
typedef struct DiamondResolverJob {
    atomic_uint references;
    atomic_bool done;
    char *host;
    int pipe_fds[2];
    int error;
    struct addrinfo *addresses;
} DiamondResolverJob;

static void resolver_release(DiamondResolverJob *job) {
    if(atomic_fetch_sub(&job->references,1)!=1)return;
    if(job->addresses!=nullptr)freeaddrinfo(job->addresses);
    close(job->pipe_fds[0]);close(job->pipe_fds[1]);
    free(job->host);free(job);
}

static void *resolver_worker(void *argument) {
    DiamondResolverJob *job=argument;
    const struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM};
    job->error=getaddrinfo(job->host,nullptr,&hints,&job->addresses);
    atomic_store_explicit(&job->done,true,memory_order_release);
    const char byte=1;
    ssize_t written;
    do {written=write(job->pipe_fds[1],&byte,1);} while(written<0&&errno==EINTR);
    /* Both pipe ends stay alive until the last reference, even if the caller
     * has already cancelled. No SIGPIPE or write to a recycled descriptor. */
    resolver_release(job);
    atomic_fetch_sub(&diamond_active_resolvers,1);
    return nullptr;
}

static DiamondVmStatus resolver_cancelled(DiamondVm *vm,DiamondValue channels,
        DiamondValue deadline,bool *cancelled) {
    *cancelled=false;
    if(channels.kind!=DIAMOND_VALUE_OBJECT||channels.as.object->kind!=DIAMOND_OBJECT_ARRAY||
       (deadline.kind!=DIAMOND_VALUE_NIL&&
        (deadline.kind!=DIAMOND_VALUE_FLOAT||!isfinite(deadline.as.real)))) {
        snprintf(vm->error,sizeof vm->error,"DNS.resolve expects an Array of Channels and a finite Float deadline or nil");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondArray *array=(const DiamondArray *)channels.as.object;
    for(size_t i=0;i<array->count;i++) {
        if(array->values[i].kind!=DIAMOND_VALUE_OBJECT||
           array->values[i].as.object->kind!=DIAMOND_OBJECT_CHANNEL) {
            snprintf(vm->error,sizeof vm->error,"DNS.resolve cancellations must be Channels");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondChannel *channel=((DiamondChannelHandle *)array->values[i].as.object)->channel;
        pthread_mutex_lock(&channel->lock);
        *cancelled=*cancelled||channel->closed;
        pthread_mutex_unlock(&channel->lock);
    }
    if(deadline.kind!=DIAMOND_VALUE_NIL) {
        struct timespec now;
        if(clock_gettime(CLOCK_MONOTONIC,&now)!=0)return DIAMOND_VM_IO_ERROR;
        *cancelled=*cancelled||((double)now.tv_sec+(double)now.tv_nsec/1e9>=deadline.as.real);
    }
    return DIAMOND_VM_OK;
}

/* out points to a VM register so the result remains rooted as strings grow. */
DiamondVmStatus dns_resolve_helper(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,DiamondValue host_value,DiamondValue channels,DiamondValue deadline,
        DiamondValue *out) {
    if(host_value.kind!=DIAMOND_VALUE_OBJECT||host_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"DNS.resolve host must be a String");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *host=(const DiamondString *)host_value.as.object;
    if(host->length==0||memchr(host->chars,'\0',host->length)!=nullptr||
       memchr(host->chars,'%',host->length)!=nullptr||memchr(host->chars,'[',host->length)!=nullptr) {
        snprintf(vm->error,sizeof vm->error,"DNS.resolve requires a hostname or an unscoped IP literal");
        return DIAMOND_VM_TYPE_ERROR;
    }
    *out=DIAMOND_NIL;
    bool cancelled=false;
    DiamondVmStatus status=resolver_cancelled(vm,channels,deadline,&cancelled);
    if(status!=DIAMOND_VM_OK||cancelled)return status;
    struct in6_addr numeric;
    if(inet_pton(AF_INET,host->chars,&numeric)==1||inet_pton(AF_INET6,host->chars,&numeric)==1) {
        DiamondArray *array=allocate_array(vm,&host_value,1);
        if(array==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out=DIAMOND_OBJECT(array);
        return DIAMOND_VM_OK;
    }
    unsigned active=atomic_load(&diamond_active_resolvers);
    do {
        if(active>=DIAMOND_RESOLVER_LIMIT) {
            snprintf(vm->error,sizeof vm->error,"DNS resolver capacity exhausted (8 outstanding lookups)");
            return DIAMOND_VM_IO_ERROR;
        }
    } while(!atomic_compare_exchange_weak(&diamond_active_resolvers,&active,active+1));
    DiamondResolverJob *job=calloc(1,sizeof *job);
    if(job==nullptr) {atomic_fetch_sub(&diamond_active_resolvers,1);return DIAMOND_VM_OUT_OF_MEMORY;}
    job->host=malloc(host->length+1);
    if(job->host==nullptr) {free(job);atomic_fetch_sub(&diamond_active_resolvers,1);return DIAMOND_VM_OUT_OF_MEMORY;}
    memcpy(job->host,host->chars,host->length+1);
    if(pipe(job->pipe_fds)!=0) {
        free(job->host);free(job);atomic_fetch_sub(&diamond_active_resolvers,1);
        snprintf(vm->error,sizeof vm->error,"DNS resolver pipe failed: %s",strerror(errno));
        return DIAMOND_VM_IO_ERROR;
    }
    atomic_init(&job->references,1);
    atomic_init(&job->done,false);
    int error=0;
    for(size_t i=0;i<2;i++) {
        if(fcntl(job->pipe_fds[i],F_SETFD,FD_CLOEXEC)<0||
           fcntl(job->pipe_fds[i],F_SETFL,O_NONBLOCK)<0) {error=errno;break;}
    }
    pthread_attr_t attributes;
    bool initialized=false;
    if(error==0) {error=pthread_attr_init(&attributes);initialized=error==0;}
    if(error==0)error=pthread_attr_setdetachstate(&attributes,PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if(error==0) {
        atomic_fetch_add(&job->references,1);
        error=pthread_create(&thread,&attributes,resolver_worker,job);
        if(error!=0)resolver_release(job);
    }
    if(initialized)pthread_attr_destroy(&attributes);
    if(error!=0) {
        resolver_release(job);atomic_fetch_sub(&diamond_active_resolvers,1);
        snprintf(vm->error,sizeof vm->error,"DNS resolver startup failed: %s",strerror(error));
        return DIAMOND_VM_IO_ERROR;
    }
    for(;;) {
        bool invoked=false;
        status=dispatch_pending_signals(vm,chunk,depth,&invoked);
        if(status!=DIAMOND_VM_OK)break;
        status=resolver_cancelled(vm,channels,deadline,&cancelled);
        if(status!=DIAMOND_VM_OK||cancelled)break;
        if(atomic_load_explicit(&job->done,memory_order_acquire)) {
            if(job->error!=0) {
                snprintf(vm->error,sizeof vm->error,"cannot resolve hostname: %s",gai_strerror(job->error));
                status=DIAMOND_VM_IO_ERROR;break;
            }
            DiamondArray *array=allocate_array(vm,nullptr,0);
            if(array==nullptr) {status=DIAMOND_VM_OUT_OF_MEMORY;break;}
            *out=DIAMOND_OBJECT(array);
            for(const struct addrinfo *entry=job->addresses;entry!=nullptr;entry=entry->ai_next) {
                char address[INET6_ADDRSTRLEN];
                const void *bytes=nullptr;
                if(entry->ai_family==AF_INET)
                    bytes=&((const struct sockaddr_in *)entry->ai_addr)->sin_addr;
                else if(entry->ai_family==AF_INET6&&((const struct sockaddr_in6 *)entry->ai_addr)->sin6_scope_id==0)
                    bytes=&((const struct sockaddr_in6 *)entry->ai_addr)->sin6_addr;
                if(bytes==nullptr||inet_ntop(entry->ai_family,bytes,address,sizeof address)==nullptr)continue;
                bool duplicate=false;
                for(size_t i=0;i<array->count;i++) {
                    const DiamondString *old=(const DiamondString *)array->values[i].as.object;
                    if(strcmp(old->chars,address)==0) {duplicate=true;break;}
                }
                if(duplicate)continue;
                DiamondString *value=allocate_string(vm,address,strlen(address));
                if(value==nullptr||!array_push(vm,array,DIAMOND_OBJECT(value))) {
                    status=DIAMOND_VM_OUT_OF_MEMORY;break;
                }
            }
            if(status==DIAMOND_VM_OK&&array->count==0) {
                snprintf(vm->error,sizeof vm->error,"hostname has no supported TCP addresses");
                status=DIAMOND_VM_IO_ERROR;
            }
            break;
        }
        struct pollfd fds[2]={{.fd=job->pipe_fds[0],.events=POLLIN}};
        status=cancellable_wait_helper(vm,nullptr,false,channels,deadline,fds,1,nullptr);
        if(status!=DIAMOND_VM_OK)break;
    }
    resolver_release(job);
    return status;
}

/* Numeric addresses keep name resolution entirely outside this nonblocking
 * contract. No resolver thread or unbounded getaddrinfo call is hidden here. */
DiamondVmStatus tcp_connect_nonblocking_helper(DiamondVm *vm,
        const DiamondString *address,int64_t port,DiamondSocketHandle **out_handle) {
    if(port<1||port>65535) {
        snprintf(vm->error,sizeof vm->error,"connect port must be between 1 and 65535");
        return DIAMOND_VM_TYPE_ERROR;
    }
    char service[6];
    snprintf(service,sizeof service,"%" PRId64,port);
    const struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM,
        .ai_flags=AI_NUMERICHOST|AI_NUMERICSERV};
    struct addrinfo *address_info=nullptr;
    struct in6_addr numeric_address;
    /* getaddrinfo treats an empty host as an unspecified address on macOS.
     * Its inet_pton also accepts scope suffixes. Require plain IP literals. */
    if(memchr(address->chars,'\0',address->length)!=nullptr||
       memchr(address->chars,'%',address->length)!=nullptr||
       (inet_pton(AF_INET,address->chars,&numeric_address)!=1&&
        inet_pton(AF_INET6,address->chars,&numeric_address)!=1)||
       getaddrinfo(address->chars,service,&hints,&address_info)!=0) {
        snprintf(vm->error,sizeof vm->error,"connect_nonblocking requires a numeric IPv4 or IPv6 address");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int fd=socket(address_info->ai_family,SOCK_STREAM,0);
    if(fd<0)goto failed;
    const int flags=fcntl(fd,F_GETFL,0);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0||fcntl(fd,F_SETFD,FD_CLOEXEC)<0)
        goto close_failed;
    const int result=connect(fd,address_info->ai_addr,address_info->ai_addrlen);
    if(result<0&&errno!=EINPROGRESS&&errno!=EINTR)goto close_failed;
    freeaddrinfo(address_info);
    DiamondSocketHandle *handle=allocate_socket_handle(vm,fd);
    if(handle==nullptr) {close(fd);return DIAMOND_VM_OUT_OF_MEMORY;}
    handle->connecting=result!=0;
    *out_handle=handle;
    return DIAMOND_VM_OK;
close_failed:;
    const int saved_errno=errno;
    close(fd);errno=saved_errno;
failed:;
    const int final_errno=errno;
    freeaddrinfo(address_info);
    snprintf(vm->error,sizeof vm->error,"nonblocking connect failed: %s",strerror(final_errno));
    return DIAMOND_VM_IO_ERROR;
}

/* SO_ERROR alone can be zero while a connection is still pending. Require
 * readiness and a connected peer before reporting success. Terminal errors
 * close the descriptor so consuming SO_ERROR cannot turn a retry into success. */
DiamondVmStatus socket_finish_connect_helper(DiamondVm *vm,DiamondSocketHandle *handle) {
    if(!handle->connecting)return DIAMOND_VM_OK;
    struct pollfd interest={.fd=handle->fd,.events=POLLOUT};
    const int ready=poll(&interest,1,0);
    if(ready==0||(ready<0&&errno==EINTR))goto pending;
    if(ready<0)goto failed;
    int error=0;socklen_t error_length=sizeof error;
    if(getsockopt(handle->fd,SOL_SOCKET,SO_ERROR,&error,&error_length)!=0)goto failed;
    if(error!=0) {errno=error;goto failed;}
    struct sockaddr_storage peer;socklen_t peer_length=sizeof peer;
    if(getpeername(handle->fd,(struct sockaddr *)&peer,&peer_length)!=0) {
        if(errno==ENOTCONN&&!(interest.revents&(POLLHUP|POLLERR|POLLNVAL)))goto pending;
        goto failed;
    }
    handle->connecting=false;
    return DIAMOND_VM_OK;
pending:
    snprintf(vm->error,sizeof vm->error,"connect would block");
    return DIAMOND_VM_WOULD_BLOCK;
failed:;
    const int saved_errno=errno;
    close(handle->fd);handle->fd=-1;
    snprintf(vm->error,sizeof vm->error,"connect failed: %s",strerror(saved_errno));
    return DIAMOND_VM_IO_ERROR;
}

/* Shared getaddrinfo/socket/bind/listen dance behind TCPServer.listen and
 * TCPServer.listen_nonblocking -- identical except for whether the
 * resulting fd is set O_NONBLOCK and which flavor of DiamondListenerHandle
 * comes out the other end. Follows the established helper-returns-status
 * convention (see stringify_value/regexp_new_helper) since VM_RETURN/
 * VM_PROPAGATE are only usable inside run_chunk's own dispatch loop.
 * `reuse_port` sets SO_REUSEPORT (opt-in, off by default -- unlike
 * SO_REUSEADDR below, which is unconditional and only affects rebinding
 * after close): lets more than one independent listening socket bind the
 * *same* port, with the kernel load-balancing new connections across them.
 * Exists for a multi-threaded server where each OS thread runs its own
 * independent accept loop against its own listener rather than sharing one
 * across a Thread boundary (Listener values can never cross one -- see
 * docs/threads.md) -- see packages/gremlin's own gremlin_worker. */
DiamondVmStatus tcp_listen_helper(DiamondVm *vm,int64_t port,
        bool nonblocking,bool reuse_port,DiamondListenerHandle **out_handle) {
    char port_text[32];
    (void)snprintf(port_text,sizeof port_text,"%" PRId64,port);
    struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM,
        .ai_flags=AI_PASSIVE};
    struct addrinfo *results=nullptr;
    const int resolve_status=getaddrinfo(nullptr,port_text,&hints,&results);
    if(resolve_status!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot listen on port %s: %s",
                 port_text,gai_strerror(resolve_status));
        return DIAMOND_VM_IO_ERROR;
    }
    int listening_fd=-1;
    int last_errno=0;
    for(struct addrinfo *candidate=results;candidate!=nullptr;
        candidate=candidate->ai_next) {
        const int fd=socket(candidate->ai_family,candidate->ai_socktype,
                             candidate->ai_protocol);
        if(fd<0) {last_errno=errno;continue;}
        const int yes=1;
        (void)setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
        if(reuse_port)(void)setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&yes,sizeof yes);
        /* No explicit address was requested (AI_PASSIVE with a nullptr
         * host above) -- the caller means "every interface", including an
         * IPv4 client connecting to 127.0.0.1, and getaddrinfo's own
         * candidate ordering commonly hands back the IPv6 wildcard (::)
         * first. Linux's default net.ipv6.bindv6only=0 makes that already
         * dual-stack (an IPv4 connection transparently reaches it) with no
         * code needed here -- which is exactly why this was invisible until
         * checked on a real BSD: FreeBSD (and OpenBSD) default
         * net.inet6.ip6.v6only to 1, so the identical bind only accepts
         * IPv6 there, and an IPv4 loopback connect gets ECONNREFUSED with
         * nothing about the failure pointing at IPv6 at all (confirmed
         * directly: tests/run.sh's TCP echo test, unchanged code, passes
         * on Linux/musl and fails this way on FreeBSD 15.1). Disabling
         * IPV6_V6ONLY unconditionally on the v6 candidate makes the
         * explicit behavior match Linux's default everywhere, rather than
         * leaving it to silently depend on a sysctl this code never
         * chose. Best-effort: a kernel without IPv6/dual-stack support at
         * all would fail this setsockopt, in which case bind() below is
         * left to fail or succeed exactly as it would have anyway. */
        if(candidate->ai_family==AF_INET6) {
            const int v6only_off=0;
            (void)setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,
                              &v6only_off,sizeof v6only_off);
        }
        if(bind(fd,candidate->ai_addr,candidate->ai_addrlen)==0) {
            listening_fd=fd;break;
        }
        last_errno=errno;close(fd);
    }
    freeaddrinfo(results);
    if(listening_fd<0) {
        snprintf(vm->error,sizeof vm->error,"cannot listen on port %s: %s",
                 port_text,strerror(last_errno));
        return DIAMOND_VM_IO_ERROR;
    }
    /* SOMAXCONN, not a small fixed number: the kernel already clamps this
     * against /proc/sys/net/core/somaxconn, so asking for more than the
     * system allows is harmless, while asking for too little (16, this
     * used to say) isn't -- a burst of concurrent connects past whatever
     * the backlog holds gets refused/dropped at the SYN queue before
     * accept() ever sees them, independent of how fast the accept loop
     * itself runs. Confirmed as the dominant cause of connection-refused
     * failures benchmarking packages/gremlin's reuse_port workers under
     * concurrency well above the old value. */
    if(listen(listening_fd,SOMAXCONN)!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot listen on port %s: %s",
                 port_text,strerror(errno));
        close(listening_fd);
        return DIAMOND_VM_IO_ERROR;
    }
    if(nonblocking) {
        const int flags=fcntl(listening_fd,F_GETFL,0);
        if(flags<0||fcntl(listening_fd,F_SETFL,flags|O_NONBLOCK)<0) {
            snprintf(vm->error,sizeof vm->error,
                     "cannot listen on port %s: %s",port_text,strerror(errno));
            close(listening_fd);
            return DIAMOND_VM_IO_ERROR;
        }
    }
    DiamondListenerHandle *listener_handle=
        allocate_listener_handle(vm,listening_fd,nonblocking);
    if(listener_handle==nullptr) {
        close(listening_fd);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *out_handle=listener_handle;
    return DIAMOND_VM_OK;
}

DiamondVmStatus tls_listen_helper(DiamondVm *vm,int64_t port,
        const char *cert_path,const char *key_path,const DiamondArray *alpn_protocols,
        const char *client_ca_path,DiamondListenerHandle **out_handle) {
    DiamondListenerHandle *listener_handle=nullptr;
    const DiamondVmStatus listen_status=tcp_listen_helper(vm,port,false,false,&listener_handle);
    if(listen_status!=DIAMOND_VM_OK)return listen_status;
    SSL_CTX *context=SSL_CTX_new(TLS_server_method());
    if(context==nullptr) {
        close(listener_handle->fd);listener_handle->fd=-1;
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,"cannot create TLS context: %s",detail);
        return DIAMOND_VM_IO_ERROR;
    }
    if(SSL_CTX_use_certificate_chain_file(context,cert_path)!=1) {
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,
            "cannot load TLS certificate '%s': %s",cert_path,detail);
        SSL_CTX_free(context);close(listener_handle->fd);listener_handle->fd=-1;
        return DIAMOND_VM_IO_ERROR;
    }
    if(SSL_CTX_use_PrivateKey_file(context,key_path,SSL_FILETYPE_PEM)!=1) {
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,
            "cannot load TLS private key '%s': %s",key_path,detail);
        SSL_CTX_free(context);close(listener_handle->fd);listener_handle->fd=-1;
        return DIAMOND_VM_IO_ERROR;
    }
    if(SSL_CTX_check_private_key(context)!=1) {
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,
            "TLS certificate and private key do not match: %s",detail);
        SSL_CTX_free(context);close(listener_handle->fd);listener_handle->fd=-1;
        return DIAMOND_VM_IO_ERROR;
    }
    if(client_ca_path!=nullptr) {
        /* Mutual TLS: request a client certificate during the handshake and
         * refuse the connection unless one arrives that chains to
         * client_ca. Loaded here, once, so a bad CA file fails at listen
         * time like a bad server cert does. SSL_load_client_CA_file also
         * gives the CertificateRequest its list of acceptable issuers. */
        STACK_OF(X509_NAME) *acceptable=SSL_load_client_CA_file(client_ca_path);
        if(SSL_CTX_load_verify_locations(context,client_ca_path,nullptr)!=1||
           acceptable==nullptr) {
            char detail[256];tls_format_error(detail,sizeof detail);
            snprintf(vm->error,sizeof vm->error,
                "cannot load client CA '%s': %s",client_ca_path,detail);
            if(acceptable!=nullptr)sk_X509_NAME_pop_free(acceptable,X509_NAME_free);
            SSL_CTX_free(context);close(listener_handle->fd);listener_handle->fd=-1;
            return DIAMOND_VM_IO_ERROR;
        }
        SSL_CTX_set_client_CA_list(context,acceptable);
        SSL_CTX_set_verify(context,
            SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT,nullptr);
    }
    listener_handle->tls_context=context;
    if(alpn_protocols!=nullptr) {
        unsigned char *encoded=nullptr;unsigned int encoded_length=0;
        const DiamondVmStatus encode_status=tls_encode_alpn_protocols(vm,
            "TLSServer.listen",alpn_protocols,&encoded,&encoded_length);
        if(encode_status!=DIAMOND_VM_OK) {
            SSL_CTX_free(context);listener_handle->tls_context=nullptr;
            close(listener_handle->fd);listener_handle->fd=-1;
            return encode_status;
        }
        listener_handle->alpn_protocols=encoded;
        listener_handle->alpn_protocols_length=encoded_length;
        /* `arg` is this listener_handle itself -- see
         * tls_alpn_select_callback's own comment. Registered after
         * alpn_protocols/alpn_protocols_length are already set, since a
         * client could in principle connect (and trigger the callback)
         * the instant .listen() returns and .accept() is called. */
        SSL_CTX_set_alpn_select_cb(context,tls_alpn_select_callback,listener_handle);
    }
    *out_handle=listener_handle;
    return DIAMOND_VM_OK;
}

void tls_abort_handshake(DiamondTlsSocketHandle *handle) {
    SSL_free(handle->ssl);handle->ssl=nullptr;
    if(handle->fd>=0)close(handle->fd);
    handle->fd=-1;
    if(handle->received_session!=nullptr)SSL_SESSION_free(handle->received_session);
    handle->received_session=nullptr;
}

/* Returns the readiness direction, or nullptr on completion. Application I/O
 * retains its existing blocking contract after the handshake succeeds. */
DiamondVmStatus tls_finish_handshake(DiamondVm *vm,
        DiamondTlsSocketHandle *handle,const char **direction) {
    *direction=nullptr;
    if(!handle->handshake_pending)return DIAMOND_VM_OK;
    ERR_clear_error();
    const int result=SSL_connect(handle->ssl);
    const int error=SSL_get_error(handle->ssl,result);
    if(result!=1) {
        if(error==SSL_ERROR_WANT_READ) {*direction="read";return DIAMOND_VM_OK;}
        if(error==SSL_ERROR_WANT_WRITE) {*direction="write";return DIAMOND_VM_OK;}
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,"TLS handshake failed: %s",detail);
        tls_abort_handshake(handle);return DIAMOND_VM_IO_ERROR;
    }
    if(SSL_get_verify_result(handle->ssl)!=X509_V_OK) {
        snprintf(vm->error,sizeof vm->error,"TLS certificate verification failed");
        tls_abort_handshake(handle);return DIAMOND_VM_IO_ERROR;
    }
    const int flags=fcntl(handle->fd,F_GETFL,0);
    if(flags<0||fcntl(handle->fd,F_SETFL,flags&~O_NONBLOCK)<0) {
        snprintf(vm->error,sizeof vm->error,"cannot restore blocking TLS I/O: %s",strerror(errno));
        tls_abort_handshake(handle);return DIAMOND_VM_IO_ERROR;
    }
    handle->handshake_pending=false;
    return DIAMOND_VM_OK;
}

/* SSL_read into a caller-supplied buffer, translating OpenSSL's own
 * error taxonomy into this VM's existing read-error/EOF conventions:
 * *out_eof true (with *out_read left 0) is the same "peer is done,
 * nothing more is ever coming" signal DIAMOND_OBJECT_SOCKET#read/
 * File#read already give a caller via a bare 0-byte-read/nil -- both a
 * clean TLS close_notify (SSL_ERROR_ZERO_RETURN) and the underlying TCP
 * connection just dropping without sending one (some peers do this) are
 * treated as EOF rather than an error, matching those two. No EINTR
 * retry here, same scope cut TCPSocket.connect and buffered File/stdin
 * reads already have (see docs/io.md's signals section) -- a signal
 * arriving mid-read makes the read fail rather than transparently
 * resuming. */
DiamondVmStatus tls_read_chunk(DiamondVm *vm,SSL *ssl,void *buffer,size_t want,
        size_t *out_read,bool *out_eof) {
    *out_read=0;*out_eof=false;
    if(want==0)return DIAMOND_VM_OK;
    const int capped=want>(size_t)INT_MAX?INT_MAX:(int)want;
    ERR_clear_error();
    const int got=SSL_read(ssl,buffer,capped);
    if(got>0) {*out_read=(size_t)got;return DIAMOND_VM_OK;}
    const int saved_errno=errno; /* read before any other call can clobber it */
    const int ssl_error=SSL_get_error(ssl,got);
    if(ssl_error==SSL_ERROR_ZERO_RETURN||(ssl_error==SSL_ERROR_SYSCALL&&got==0)) {
        *out_eof=true;return DIAMOND_VM_OK;
    }
    char detail[256];
    /* A blocking socket with SO_RCVTIMEO set can report a timed-out read
     * as SSL_ERROR_SYSCALL (the common case) or, depending on OpenSSL
     * version and exactly what protocol bookkeeping SSL_read was doing
     * when the underlying read() timed out, as SSL_ERROR_WANT_READ/WRITE
     * instead -- either way there's a real EAGAIN/EWOULDBLOCK sitting in
     * errno and no real OpenSSL protocol error on the queue, so check
     * errno directly rather than trusting one specific ssl_error value to
     * mean "this was a plain I/O failure". */
    const bool would_block=saved_errno==EAGAIN||saved_errno==EWOULDBLOCK;
    if((ssl_error==SSL_ERROR_SYSCALL||ssl_error==SSL_ERROR_WANT_READ||
        ssl_error==SSL_ERROR_WANT_WRITE)&&(would_block||ERR_peek_error()==0))
        (void)snprintf(detail,sizeof detail,"%s",strerror(saved_errno));
    else
        tls_format_error(detail,sizeof detail);
    snprintf(vm->error,sizeof vm->error,"TLS read error: %s",detail);
    return DIAMOND_VM_IO_ERROR;
}

/* Writes all of `length` bytes, looping over SSL_write as needed --
 * unlike DIAMOND_OBJECT_SOCKET#write (a non-blocking fd, where a partial
 * write is a normal outcome the caller retries), this fd is always
 * blocking, so a short SSL_write is only possible via a genuine error,
 * never "the buffer's full, try again later". */
DiamondVmStatus tls_write_all(DiamondVm *vm,SSL *ssl,const char *data,size_t length) {
    size_t written=0;
    while(written<length) {
        const size_t remaining=length-written;
        const int want=remaining>(size_t)INT_MAX?INT_MAX:(int)remaining;
        ERR_clear_error();
        const int got=SSL_write(ssl,data+written,want);
        if(got<=0) {
            const int saved_errno=errno;
            const int ssl_error=SSL_get_error(ssl,got);
            char detail[256];
            const bool would_block=saved_errno==EAGAIN||saved_errno==EWOULDBLOCK;
            if((ssl_error==SSL_ERROR_SYSCALL||ssl_error==SSL_ERROR_WANT_READ||
                ssl_error==SSL_ERROR_WANT_WRITE)&&(would_block||ERR_peek_error()==0))
                (void)snprintf(detail,sizeof detail,"%s",strerror(saved_errno));
            else
                tls_format_error(detail,sizeof detail);
            snprintf(vm->error,sizeof vm->error,"TLS write error: %s",detail);
            return DIAMOND_VM_IO_ERROR;
        }
        written+=(size_t)got;
    }
    return DIAMOND_VM_OK;
}

/* TLS analogue of read_line -- byte-at-a-time via tls_read_chunk rather
 * than fgets, since a raw SSL/fd has no libc stdio buffering underneath
 * it to lean on. Not as wasteful as it looks: SSL_read decrypts and
 * buffers a whole TLS record (up to 16KB) on the first call that needs
 * one, so a run of single-byte reads against an already-buffered record
 * costs one function call each, not one recv(2) each. Same trailing
 * \n/\r-stripping and *saw_any contract as read_line. */
DiamondVmStatus tls_read_line(DiamondVm *vm,SSL *ssl,StringBuilder *builder,
                                     bool *saw_any) {
    *saw_any=false;
    for(;;) {
        char byte=0;size_t got=0;bool eof=false;
        const DiamondVmStatus status=tls_read_chunk(vm,ssl,&byte,1,&got,&eof);
        if(status!=DIAMOND_VM_OK)return status;
        if(eof||got==0)break;
        *saw_any=true;
        if(!builder_append(builder,&byte,1))return DIAMOND_VM_OUT_OF_MEMORY;
        if(byte=='\n')break;
    }
    if(builder->length>0&&builder->chars[builder->length-1]=='\n') {
        builder->length--;
        if(builder->length>0&&builder->chars[builder->length-1]=='\r')builder->length--;
        builder->chars[builder->length]='\0';
    }
    return DIAMOND_VM_OK;
}
