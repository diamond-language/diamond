#include "vm_internal.h"
#include "tls_crypto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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
/* host is nullptr for every interface (AI_PASSIVE), or one address or
 * hostname to bind, as TCPServer.listen's host: option. */
DiamondVmStatus udp_socket_helper(DiamondVm *vm,bool bind_socket,
        int64_t port,const DiamondString *host,DiamondUdpSocketHandle **out_handle) {
    int fd=-1;
    if(bind_socket) {
        char port_text[32];
        (void)snprintf(port_text,sizeof port_text,"%" PRId64,port);
        char host_text[256];
        if(host!=nullptr) {
            if(host->length==0||host->length>=sizeof host_text||
               memchr(host->chars,'\0',host->length)!=nullptr) {
                snprintf(vm->error,sizeof vm->error,"UDPSocket.bind host: is not a valid address");
                return DIAMOND_VM_TYPE_ERROR;
            }
            memcpy(host_text,host->chars,host->length);
            host_text[host->length]='\0';
        }
        struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_DGRAM,
            .ai_flags=AI_PASSIVE};
        struct addrinfo *results=nullptr;
        const int resolve_status=getaddrinfo(host!=nullptr?host_text:nullptr,port_text,
                                             &hints,&results);
        if(resolve_status!=0) {
            snprintf(vm->error,sizeof vm->error,"cannot bind UDP port %s: %s",
                     port_text,gai_strerror(resolve_status));
            return DIAMOND_VM_IO_ERROR;
        }
        int last_errno=0;
        for(struct addrinfo *candidate=results;candidate!=nullptr;
            candidate=candidate->ai_next) {
            const int candidate_fd=diamond_socket(candidate->ai_family,candidate->ai_socktype,
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
        fd=diamond_socket(AF_INET,SOCK_DGRAM,0);
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

static DiamondVmStatus resolve_interruptible(DiamondVm *vm,const char *host,
    const char *service,int64_t port,struct addrinfo **results,int *gai_error);

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
        int64_t port,int64_t connect_timeout_ms,int64_t recv_buffer_bytes,
        int64_t send_buffer_bytes,int *out_fd) {
    char port_text[32];
    (void)snprintf(port_text,sizeof port_text,"%" PRId64,port);
    struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM};
    struct addrinfo *results=nullptr;
    int resolve_status=0;
    if(vm->interrupt_flag!=nullptr) {
        const DiamondVmStatus resolve_wait=resolve_interruptible(vm,host->chars,
            port_text,port,&results,&resolve_status);
        if(resolve_wait!=DIAMOND_VM_OK)return resolve_wait;
    } else
        resolve_status=getaddrinfo(host->chars,port_text,&hints,&results);
    if(resolve_status!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot connect to '%.*s:%s': %s",
                 (int)host->length,host->chars,port_text,gai_strerror(resolve_status));
        return DIAMOND_VM_IO_ERROR;
    }
    int connected_fd=-1;
    int last_errno=0;
    for(struct addrinfo *candidate=results;candidate!=nullptr;
        candidate=candidate->ai_next) {
        const int fd=diamond_socket(candidate->ai_family,candidate->ai_socktype,
                             candidate->ai_protocol);
        if(fd<0) {last_errno=errno;continue;}
        /* Buffer sizes have to be set before connect for the TCP window to
         * be negotiated from them. The kernel clamps (and on Linux doubles)
         * the value; failure to set one is not a connect failure. */
        if(recv_buffer_bytes>0) {
            const int size=recv_buffer_bytes>INT_MAX?INT_MAX:(int)recv_buffer_bytes;
            (void)setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&size,sizeof size);
        }
        if(send_buffer_bytes>0) {
            const int size=send_buffer_bytes>INT_MAX?INT_MAX:(int)send_buffer_bytes;
            (void)setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&size,sizeof size);
        }
        if(connect_timeout_ms>=0||vm->interrupt_flag!=nullptr) {
            const int flags=fcntl(fd,F_GETFL,0);
            if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) {
                last_errno=errno;close(fd);continue;
            }
            /* An interruptible VM connects non-blocking and waits in slices
             * even without a timeout, so a stop request ends a connect to an
             * unresponsive host. */
            if(connect(fd,candidate->ai_addr,candidate->ai_addrlen)!=0) {
                if(errno!=EINPROGRESS) {last_errno=errno;close(fd);continue;}
                struct pollfd interest={.fd=fd,.events=POLLOUT};
                DiamondVmStatus wait_status=DIAMOND_VM_OK;
                const int ready=poll_interruptible(vm,&interest,1,
                    connect_timeout_ms<0?-1:
                    (connect_timeout_ms>INT_MAX?INT_MAX:(int)connect_timeout_ms),
                    &wait_status);
                if(wait_status!=DIAMOND_VM_OK) {
                    close(fd);freeaddrinfo(results);return wait_status;
                }
                int socket_error=0;socklen_t error_length=sizeof socket_error;
                if(ready==0) {last_errno=ETIMEDOUT;close(fd);continue;}
                if(ready<0||getsockopt(fd,SOL_SOCKET,SO_ERROR,&socket_error,
                       &error_length)!=0) {
                    last_errno=errno;close(fd);continue;
                }
                if(socket_error!=0) {last_errno=socket_error;close(fd);continue;}
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

/* Resolver workers own only native data, never a VM, Diamond value, or a
 * pointer into the caller's heap. Cancellation abandons a reference rather
 * than joining libc's potentially unbounded getaddrinfo. Eight outstanding
 * workers bound the resources retained by abandoned lookups. */
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

/* getaddrinfo for an interruptible VM: the lookup runs on a detached resolver
 * thread (the one DNS.resolve uses) and the caller waits in slices, so a stop
 * request abandons it instead of waiting out the resolver. On success
 * *results is a getaddrinfo list with `port` filled in, owned by the caller.
 * Numeric hosts, and a full resolver pool, resolve inline as before. */
static DiamondVmStatus resolve_interruptible(DiamondVm *vm,const char *host,
        const char *service,int64_t port,struct addrinfo **results,int *gai_error) {
    const struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM};
    struct in6_addr numeric;
    unsigned active=atomic_load(&diamond_active_resolvers);
    bool slot=false;
    if(inet_pton(AF_INET,host,&numeric)!=1&&inet_pton(AF_INET6,host,&numeric)!=1) {
        while(active<DIAMOND_RESOLVER_LIMIT&&!slot)
            slot=atomic_compare_exchange_weak(&diamond_active_resolvers,&active,active+1);
    }
    DiamondResolverJob *job=nullptr;
    if(slot) {
        job=calloc(1,sizeof *job);
        if(job!=nullptr) {
            job->host=strdup(host);
            if(job->host==nullptr||pipe(job->pipe_fds)!=0) {
                free(job->host);free(job);job=nullptr;
            }
        }
        if(job==nullptr)atomic_fetch_sub(&diamond_active_resolvers,1);
    }
    if(job==nullptr) {
        *gai_error=getaddrinfo(host,service,&hints,results);
        return DIAMOND_VM_OK;
    }
    atomic_init(&job->references,2);
    atomic_init(&job->done,false);
    for(size_t i=0;i<2;i++)
        (void)fcntl(job->pipe_fds[i],F_SETFD,FD_CLOEXEC);
    pthread_attr_t attributes;
    int error=pthread_attr_init(&attributes);
    pthread_t thread;
    if(error==0) {
        pthread_attr_setdetachstate(&attributes,PTHREAD_CREATE_DETACHED);
        error=pthread_create(&thread,&attributes,resolver_worker,job);
        pthread_attr_destroy(&attributes);
    }
    if(error!=0) {
        /* The worker never ran: drop its reference and the slot, resolve inline. */
        atomic_fetch_sub(&job->references,1);
        resolver_release(job);atomic_fetch_sub(&diamond_active_resolvers,1);
        *gai_error=getaddrinfo(host,service,&hints,results);
        return DIAMOND_VM_OK;
    }
    DiamondVmStatus status=DIAMOND_VM_OK;
    while(!atomic_load_explicit(&job->done,memory_order_acquire)) {
        struct pollfd interest={.fd=job->pipe_fds[0],.events=POLLIN};
        (void)poll_interruptible(vm,&interest,1,-1,&status);
        if(status!=DIAMOND_VM_OK)break;
    }
    if(status==DIAMOND_VM_OK) {
        *gai_error=job->error;
        if(job->error==0) {
            *results=job->addresses;job->addresses=nullptr;
            for(struct addrinfo *entry=*results;entry!=nullptr;entry=entry->ai_next) {
                if(entry->ai_family==AF_INET)
                    ((struct sockaddr_in *)entry->ai_addr)->sin_port=htons((uint16_t)port);
                else if(entry->ai_family==AF_INET6)
                    ((struct sockaddr_in6 *)entry->ai_addr)->sin6_port=htons((uint16_t)port);
            }
        }
    }
    resolver_release(job);
    return status;
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
    const int fd=diamond_socket(address_info->ai_family,SOCK_STREAM,0);
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
DiamondVmStatus tcp_listen_helper(DiamondVm *vm,const char *host,int64_t port,
        bool nonblocking,bool reuse_port,int64_t backlog,DiamondListenerHandle **out_handle) {
    char port_text[32];
    (void)snprintf(port_text,sizeof port_text,"%" PRId64,port);
    struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM,
        .ai_flags=AI_PASSIVE};
    struct addrinfo *results=nullptr;
    const int resolve_status=getaddrinfo(host,port_text,&hints,&results);
    if(resolve_status!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot listen on %s%sport %s: %s",
                 host!=nullptr?host:"",host!=nullptr?" ":"",port_text,gai_strerror(resolve_status));
        return DIAMOND_VM_IO_ERROR;
    }
    int listening_fd=-1;
    int last_errno=0;
    for(struct addrinfo *candidate=results;candidate!=nullptr;
        candidate=candidate->ai_next) {
        const int fd=diamond_socket(candidate->ai_family,candidate->ai_socktype,
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
    if(listen(listening_fd,backlog>0?(backlog>INT_MAX?INT_MAX:(int)backlog):SOMAXCONN)!=0) {
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
    const DiamondVmStatus listen_status=tcp_listen_helper(vm,nullptr,port,false,false,0,&listener_handle);
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
DiamondVmStatus diamond_tls_finish_handshake(DiamondVm *vm,
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
    if(vm!=nullptr&&vm->interrupt_flag!=nullptr&&SSL_pending(ssl)==0) {
        const int descriptor=SSL_get_fd(ssl);
        if(descriptor>=0) {
            const DiamondVmStatus wait_status=
                wait_fd_interruptible(vm,descriptor,POLLIN);
            if(wait_status!=DIAMOND_VM_OK)return wait_status;
        }
    }
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

/* Runs one blocking-mode handshake step (SSL_connect or SSL_accept) so an
 * interruptible VM can leave it. The descriptor is this connection's own, so
 * for the call it is switched to O_NONBLOCK and a WANT_READ/WANT_WRITE result
 * waits for readiness in slices (wait_fd_interruptible) and retries. A VM
 * without an interrupt flag calls straight through. *result is the OpenSSL
 * return value; the status is DIAMOND_VM_INTERRUPTED if the wait was cut. */
DiamondVmStatus tls_handshake_interruptible(DiamondVm *vm,SSL *ssl,bool accept,
        int *result) {
    const int descriptor=SSL_get_fd(ssl);
    int saved_flags=-1;
    if(vm!=nullptr&&vm->interrupt_flag!=nullptr&&descriptor>=0) {
        const int flags=fcntl(descriptor,F_GETFL,0);
        if(flags>=0&&(flags&O_NONBLOCK)==0&&
           fcntl(descriptor,F_SETFL,flags|O_NONBLOCK)==0)
            saved_flags=flags;
    }
    DiamondVmStatus status=DIAMOND_VM_OK;
    for(;;) {
        ERR_clear_error();
        *result=accept?SSL_accept(ssl):SSL_connect(ssl);
        if(*result==1||saved_flags<0)break;
        const int error=SSL_get_error(ssl,*result);
        if(error!=SSL_ERROR_WANT_READ&&error!=SSL_ERROR_WANT_WRITE)break;
        status=wait_fd_interruptible(vm,descriptor,
            error==SSL_ERROR_WANT_READ?POLLIN:POLLOUT);
        if(status!=DIAMOND_VM_OK)break;
    }
    if(saved_flags>=0)(void)fcntl(descriptor,F_SETFL,saved_flags);
    return status;
}

/* Writes all of `length` bytes, looping over SSL_write as needed --
 * unlike DIAMOND_OBJECT_SOCKET#write (a non-blocking fd, where a partial
 * write is a normal outcome the caller retries), this fd is always
 * blocking, so a short SSL_write is only possible via a genuine error,
 * never "the buffer's full, try again later". In an interruptible VM the
 * connection's own descriptor is non-blocking for the call and a full send
 * buffer waits in slices, so an interrupt can end a write to a stalled peer. */
DiamondVmStatus tls_write_all(DiamondVm *vm,SSL *ssl,const char *data,size_t length) {
    const int descriptor=SSL_get_fd(ssl);
    int saved_flags=-1;
    if(vm!=nullptr&&vm->interrupt_flag!=nullptr&&descriptor>=0) {
        const int flags=fcntl(descriptor,F_GETFL,0);
        if(flags>=0&&(flags&O_NONBLOCK)==0&&
           fcntl(descriptor,F_SETFL,flags|O_NONBLOCK)==0)
            saved_flags=flags;
    }
    DiamondVmStatus status=DIAMOND_VM_OK;
    size_t written=0;
    while(written<length) {
        const size_t remaining=length-written;
        const int want=remaining>(size_t)INT_MAX?INT_MAX:(int)remaining;
        ERR_clear_error();
        const int got=SSL_write(ssl,data+written,want);
        if(got<=0) {
            const int saved_errno=errno;
            const int ssl_error=SSL_get_error(ssl,got);
            if(saved_flags>=0&&(ssl_error==SSL_ERROR_WANT_WRITE||
               ssl_error==SSL_ERROR_WANT_READ)) {
                status=wait_fd_interruptible(vm,descriptor,
                    ssl_error==SSL_ERROR_WANT_WRITE?POLLOUT:POLLIN);
                if(status!=DIAMOND_VM_OK)break;
                continue;
            }
            char detail[256];
            const bool would_block=saved_errno==EAGAIN||saved_errno==EWOULDBLOCK;
            if((ssl_error==SSL_ERROR_SYSCALL||ssl_error==SSL_ERROR_WANT_READ||
                ssl_error==SSL_ERROR_WANT_WRITE)&&(would_block||ERR_peek_error()==0))
                (void)snprintf(detail,sizeof detail,"%s",strerror(saved_errno));
            else
                tls_format_error(detail,sizeof detail);
            snprintf(vm->error,sizeof vm->error,"TLS write error: %s",detail);
            status=DIAMOND_VM_IO_ERROR;
            break;
        }
        written+=(size_t)got;
    }
    if(saved_flags>=0)(void)fcntl(descriptor,F_SETFL,saved_flags);
    return status;
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

/* Shared getaddrinfo/socket/connect dance behind TCPSocket.connect and
 * TLSSocket.connect -- both need a connected fd before doing anything
 * TLS-specific, so this is exactly the code TCP_CONNECT's own handler
 * used to have inline, unchanged, just callable from a second opcode
 * handler now too. No SA_RESTART-style signal-retry here, matching the
 * existing TCPSocket.connect scope cut documented in docs/io.md: a
 * signal arriving mid-connect makes the attempt fail rather than
 * transparently resuming, same as it always has. */
/* Connects one already-created, non-blocking `fd` to `candidate`, waiting
 * up to `timeout_ms` for the kernel to finish a connection already in
 * progress (EINPROGRESS is the expected, common case for a non-blocking
 * connect -- not a failure). Returns 0 on a successful connection, -1
 * with `errno` set otherwise (ETIMEDOUT for a real timeout, whatever
 * `connect`/`poll`/`getsockopt` themselves reported otherwise) --
 * matching plain blocking `connect`'s own return convention so callers
 * don't need a second success path. */
int connect_with_timeout(int fd,const struct addrinfo *candidate,
        int64_t timeout_ms) {
    if(connect(fd,candidate->ai_addr,candidate->ai_addrlen)==0)return 0;
    if(errno!=EINPROGRESS)return -1;
    struct pollfd poll_fd={.fd=fd,.events=POLLOUT};
    const int poll_timeout=timeout_ms<0?-1:
        (timeout_ms>INT_MAX?INT_MAX:(int)timeout_ms);
    const int poll_result=poll(&poll_fd,1,poll_timeout);
    if(poll_result==0) {errno=ETIMEDOUT;return -1;}
    if(poll_result<0)return -1;
    int socket_error=0;socklen_t error_length=sizeof socket_error;
    if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&socket_error,&error_length)!=0)return -1;
    if(socket_error!=0) {errno=socket_error;return -1;}
    return 0;
}

/* Applies SO_RCVTIMEO/SO_SNDTIMEO to an already-connected fd -- a
 * negative value leaves the corresponding OS default (block forever)
 * untouched, so a caller that only wants one of the two only pays for
 * that one setsockopt call. A read/write that later times out surfaces
 * exactly like any other failed read/write already does (EAGAIN/
 * EWOULDBLOCK via errno, or SSL_get_error's matching case for a TLS
 * socket) -- no new error path, just a new way for the existing one to
 * trigger. */
DiamondVmStatus apply_socket_timeouts_helper(DiamondVm *vm,int fd,
        int64_t read_timeout_ms,int64_t write_timeout_ms) {
    if(read_timeout_ms>=0) {
        const struct timeval tv={.tv_sec=(time_t)(read_timeout_ms/1000),
            .tv_usec=(suseconds_t)((read_timeout_ms%1000)*1000)};
        if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv)!=0) {
            snprintf(vm->error,sizeof vm->error,
                "cannot set read_timeout_ms: %s",strerror(errno));
            return DIAMOND_VM_IO_ERROR;
        }
    }
    if(write_timeout_ms>=0) {
        const struct timeval tv={.tv_sec=(time_t)(write_timeout_ms/1000),
            .tv_usec=(suseconds_t)((write_timeout_ms%1000)*1000)};
        if(setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv)!=0) {
            snprintf(vm->error,sizeof vm->error,
                "cannot set write_timeout_ms: %s",strerror(errno));
            return DIAMOND_VM_IO_ERROR;
        }
    }
    return DIAMOND_VM_OK;
}

/* One (key, kind, out-pointer) row per recognized `TCPSocket.connect`/
 * `TLSSocket.connect` options entry -- `kind` picks which one of
 * int_out/string_out/array_out the parse loop below writes into, so it
 * can type-check generically instead of repeating the same branches by
 * hand for every field. `DIAMOND_SOCKET_OPTION_ARRAY` (currently just
 * `alpn`) only validates "is this an Array" generically here -- its
 * element-by-element String check happens in the one caller that
 * actually interprets it (DIAMOND_OP_TLS_CONNECT), since encoding it
 * into OpenSSL's ALPN wire format is TLS-specific, not something a
 * TCPSocket.connect caller (which never reaches an array field at all,
 * since `allow_tls_fields` gates it out) needs to share. */
typedef enum {
    DIAMOND_SOCKET_OPTION_INT,
    DIAMOND_SOCKET_OPTION_STRING,
    DIAMOND_SOCKET_OPTION_ARRAY,
} DiamondSocketOptionKind;

typedef struct {
    const char *key;
    DiamondSocketOptionKind kind;
    int64_t *int_out;
    const DiamondString **string_out;
    const DiamondArray **array_out;
} DiamondSocketOptionField;

/* `options_value` is Nil (nothing set, every field keeps its default) or
 * a Hash of recognized String keys -- anything else, or an unrecognized
 * key, is a TypeError, matching this codebase's existing "an unrecognized
 * name is a TypeError, not silently ignored" stance (see Signal.trap).
 * `allow_tls_fields` is false for TCPSocket.connect (ca_file/ca_path/
 * cert/key/session/alpn make no sense on a connection with no TLS layer
 * at all) and true for TLSSocket.connect. */
DiamondVmStatus parse_socket_connect_options_helper(DiamondVm *vm,
        DiamondValue options_value,bool allow_tls_fields,
        DiamondSocketConnectOptions *out) {
    *out=(DiamondSocketConnectOptions){.connect_timeout_ms=-1,
        .read_timeout_ms=-1,.write_timeout_ms=-1,
        .recv_buffer_bytes=-1,.send_buffer_bytes=-1};
    if(options_value.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OK;
    if(options_value.kind!=DIAMOND_VALUE_OBJECT||
       options_value.as.object->kind!=DIAMOND_OBJECT_HASH) {
        snprintf(vm->error,sizeof vm->error,
            "connect options must be a Hash or nil");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondHash *options=(const DiamondHash *)options_value.as.object;
    const DiamondSocketOptionField fields[]={
        {"connect_timeout_ms",DIAMOND_SOCKET_OPTION_INT,&out->connect_timeout_ms,nullptr,nullptr},
        {"read_timeout_ms",DIAMOND_SOCKET_OPTION_INT,&out->read_timeout_ms,nullptr,nullptr},
        {"write_timeout_ms",DIAMOND_SOCKET_OPTION_INT,&out->write_timeout_ms,nullptr,nullptr},
        {"recv_buffer_bytes",DIAMOND_SOCKET_OPTION_INT,&out->recv_buffer_bytes,nullptr,nullptr},
        {"send_buffer_bytes",DIAMOND_SOCKET_OPTION_INT,&out->send_buffer_bytes,nullptr,nullptr},
        {"ca_file",DIAMOND_SOCKET_OPTION_STRING,nullptr,&out->ca_file,nullptr},
        {"ca_path",DIAMOND_SOCKET_OPTION_STRING,nullptr,&out->ca_path,nullptr},
        {"cert",DIAMOND_SOCKET_OPTION_STRING,nullptr,&out->cert,nullptr},
        {"key",DIAMOND_SOCKET_OPTION_STRING,nullptr,&out->key,nullptr},
        {"session",DIAMOND_SOCKET_OPTION_STRING,nullptr,&out->session,nullptr},
        {"alpn",DIAMOND_SOCKET_OPTION_ARRAY,nullptr,nullptr,&out->alpn},
    };
    const size_t field_count=allow_tls_fields?
        sizeof fields/sizeof fields[0]:5;
    for(size_t index=0;index<options->count;index++) {
        const DiamondValue entry_key=options->entries[index].key;
        if(entry_key.kind!=DIAMOND_VALUE_OBJECT||
           entry_key.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,"connect options Hash keys must be Strings");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *entry_key_string=(const DiamondString *)entry_key.as.object;
        const DiamondSocketOptionField *matched=nullptr;
        for(size_t field_index=0;field_index<field_count;field_index++) {
            const size_t key_length=strlen(fields[field_index].key);
            if(entry_key_string->length==key_length&&
               memcmp(entry_key_string->chars,fields[field_index].key,key_length)==0) {
                matched=&fields[field_index];break;
            }
        }
        if(matched==nullptr) {
            snprintf(vm->error,sizeof vm->error,
                "unrecognized connect option '%.*s'",
                (int)entry_key_string->length,entry_key_string->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondValue entry_value=options->entries[index].value;
        if(matched->kind==DIAMOND_SOCKET_OPTION_STRING) {
            if(entry_value.kind!=DIAMOND_VALUE_OBJECT||
               entry_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
                snprintf(vm->error,sizeof vm->error,
                    "connect option '%s' must be a String",matched->key);
                return DIAMOND_VM_TYPE_ERROR;
            }
            *matched->string_out=(const DiamondString *)entry_value.as.object;
        } else if(matched->kind==DIAMOND_SOCKET_OPTION_ARRAY) {
            if(entry_value.kind!=DIAMOND_VALUE_OBJECT||
               entry_value.as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                snprintf(vm->error,sizeof vm->error,
                    "connect option '%s' must be an Array",matched->key);
                return DIAMOND_VM_TYPE_ERROR;
            }
            *matched->array_out=(const DiamondArray *)entry_value.as.object;
        } else {
            if(entry_value.kind!=DIAMOND_VALUE_INT) {
                snprintf(vm->error,sizeof vm->error,
                    "connect option '%s' must be an Int",matched->key);
                return DIAMOND_VM_TYPE_ERROR;
            }
            *matched->int_out=entry_value.as.integer;
        }
    }
    return DIAMOND_VM_OK;
}

/* Shared behind TLSServer.listen(port, cert_path, key_path): the same
 * plain-TCP listen tcp_listen_helper already does (a TLS listener is a
 * regular listening socket underneath -- the TLS part only starts once a
 * connection is actually accepted), plus loading the server's
 * certificate chain and private key into one SSL_CTX that every future
 * .accept() on this listener reuses (see DiamondListenerHandle's own
 * tls_context field comment for the lifetime story). Loaded once here
 * rather than per-connection specifically so a listener with a broken
 * cert/key pair fails loudly at TLSServer.listen time, not silently on
 * whichever connection happens to be first. */
/* TLSServer.listen's own (much smaller) options Hash: `alpn` (an Array of
 * protocol names) and `client_ca` (a String path to a PEM bundle of the
 * certificate authorities client certificates must chain to; giving it makes
 * the server request and require a verified client certificate). A
 * dedicated parser rather than reusing parse_socket_connect_options_helper's
 * table-driven approach, which exists to share seven fields across two call
 * sites; two fields nobody else shares don't earn that machinery. */
DiamondVmStatus parse_tls_listen_options_helper(DiamondVm *vm,
        DiamondValue options_value,const DiamondArray **out_alpn,
        const DiamondString **out_client_ca) {
    *out_alpn=nullptr;*out_client_ca=nullptr;
    if(options_value.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OK;
    if(options_value.kind!=DIAMOND_VALUE_OBJECT||
       options_value.as.object->kind!=DIAMOND_OBJECT_HASH) {
        snprintf(vm->error,sizeof vm->error,"TLSServer.listen options must be a Hash or nil");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondHash *options=(const DiamondHash *)options_value.as.object;
    for(size_t index=0;index<options->count;index++) {
        const DiamondValue entry_key=options->entries[index].key;
        if(entry_key.kind!=DIAMOND_VALUE_OBJECT||
           entry_key.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "TLSServer.listen options Hash keys must be Strings");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *entry_key_string=(const DiamondString *)entry_key.as.object;
        const DiamondValue entry_value=options->entries[index].value;
        if(entry_key_string->length==4&&memcmp(entry_key_string->chars,"alpn",4)==0) {
            if(entry_value.kind!=DIAMOND_VALUE_OBJECT||
               entry_value.as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                snprintf(vm->error,sizeof vm->error,"TLSServer.listen option 'alpn' must be an Array");
                return DIAMOND_VM_TYPE_ERROR;
            }
            *out_alpn=(const DiamondArray *)entry_value.as.object;
        } else if(entry_key_string->length==9&&
                  memcmp(entry_key_string->chars,"client_ca",9)==0) {
            if(entry_value.kind!=DIAMOND_VALUE_OBJECT||
               entry_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
                snprintf(vm->error,sizeof vm->error,
                    "TLSServer.listen option 'client_ca' must be a String path");
                return DIAMOND_VM_TYPE_ERROR;
            }
            *out_client_ca=(const DiamondString *)entry_value.as.object;
        } else {
            snprintf(vm->error,sizeof vm->error,
                "unrecognized TLSServer.listen option '%.*s'",
                (int)entry_key_string->length,entry_key_string->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    return DIAMOND_VM_OK;
}

/* The remote address of a connected socket as text ("203.0.113.7", "2001:db8::1"; an IPv4-mapped IPv6 peer
 * is reported as plain IPv4), or nil when it has none (not connected yet, or a Unix-domain peer). Raises
 * IOError when `fd` is not a socket at all. */
DiamondVmStatus peer_address_helper(DiamondVm *vm,int fd,DiamondValue *dest) {
    struct sockaddr_storage peer;socklen_t length=sizeof peer;
    if(getpeername(fd,(struct sockaddr *)&peer,&length)!=0) {
        if(errno==ENOTSOCK) {
            snprintf(vm->error,sizeof vm->error,"peer_address failed: not a socket");
            return DIAMOND_VM_IO_ERROR;
        }
        *dest=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    char text[INET6_ADDRSTRLEN]="";
    if(peer.ss_family==AF_INET) {
        const struct sockaddr_in *address=(const struct sockaddr_in *)&peer;
        inet_ntop(AF_INET,&address->sin_addr,text,sizeof text);
    } else if(peer.ss_family==AF_INET6) {
        const struct sockaddr_in6 *address=(const struct sockaddr_in6 *)&peer;
        if(IN6_IS_ADDR_V4MAPPED(&address->sin6_addr)) {
            inet_ntop(AF_INET,&address->sin6_addr.s6_addr[12],text,sizeof text);
        } else {
            inet_ntop(AF_INET6,&address->sin6_addr,text,sizeof text);
        }
    }
    if(text[0]=='\0') {*dest=DIAMOND_NIL;return DIAMOND_VM_OK;}
    DiamondString *string=allocate_string(vm,text,strlen(text));
    if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *dest=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
}

DiamondVmStatus socket_invoke_helper(DiamondVm *vm,
                                            const DiamondChunk *chunk, size_t depth,
                                            DiamondValue *registers, uint16_t recv,
                                            uint16_t base, uint8_t argc, uint16_t dest,
                                            const DiamondStringConstant *method_name) {
    DiamondSocketHandle *socket_handle=
        (DiamondSocketHandle *)registers[recv].as.object;
    const bool read_method=method_name->length==4&&
        memcmp(method_name->chars,"read",4)==0;
    const bool write_method=method_name->length==5&&
        memcmp(method_name->chars,"write",5)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    const bool finish_method=method_name->length==14&&
        memcmp(method_name->chars,"finish_connect",14)==0;
    const bool shutdown_write_method=method_name->length==14&&
        memcmp(method_name->chars,"shutdown_write",14)==0;
    const bool peer_address_method=method_name->length==12&&
        memcmp(method_name->chars,"peer_address",12)==0;
    if(!read_method&&!write_method&&!close_method&&!finish_method&&
       !shutdown_write_method&&!peer_address_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"Socket");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(socket_handle->fd>=0) {
            close(socket_handle->fd);
            socket_handle->fd=-1;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(socket_handle->fd<0) {
        snprintf(vm->error,sizeof vm->error,"socket is closed");
        return DIAMOND_VM_IO_ERROR;
    }
    if(peer_address_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        return peer_address_helper(vm,socket_handle->fd,&registers[dest]);
    }
    if((finish_method||shutdown_write_method)&&argc!=0)return DIAMOND_VM_ARITY_ERROR;
    const DiamondVmStatus finish_status=socket_finish_connect_helper(vm,socket_handle);
    if(finish_status!=DIAMOND_VM_OK)return finish_status;
    if(finish_method) {registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;}
    if(shutdown_write_method) {
        /* Half-close: the peer sees EOF, this side can still read. */
        if(shutdown(socket_handle->fd,SHUT_WR)!=0) {
            snprintf(vm->error,sizeof vm->error,"shutdown_write failed: %s",strerror(errno));
            return DIAMOND_VM_IO_ERROR;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(read_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[base].as.integer<0) {
            snprintf(vm->error,sizeof vm->error,
                "Socket#read argument must be a non-negative Int");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const size_t want=(size_t)registers[base].as.integer;
        if(want==0) {
            DiamondString *empty=allocate_string(vm,"",0);
            if(empty==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
            registers[dest]=DIAMOND_OBJECT(empty);return DIAMOND_VM_OK;
        }
        const DiamondVmStatus wait_status=
            wait_fd_interruptible(vm,socket_handle->fd,POLLIN);
        if(wait_status!=DIAMOND_VM_OK)return wait_status;
        char *buffer=malloc(want);
        if(buffer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        errno=0;
        const ssize_t read_count=read(socket_handle->fd,buffer,want);
        if(read_count<0) {
            const int saved_errno=errno;
            free(buffer);
            if(saved_errno==EAGAIN||saved_errno==EWOULDBLOCK) {
                snprintf(vm->error,sizeof vm->error,"read would block");
                return DIAMOND_VM_WOULD_BLOCK;
            }
            snprintf(vm->error,sizeof vm->error,"read error: %s",
                strerror(saved_errno));
            return DIAMOND_VM_IO_ERROR;
        }
        if(read_count==0) {
            /* Peer closed -- the same EOF-as-nil convention
             * File#read already uses, distinct from
             * WouldBlockError (nothing available *yet* vs.
             * nothing ever coming again). */
            free(buffer);
            registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
        }
        DiamondString *string=allocate_string(vm,buffer,(size_t)read_count);
        free(buffer);
        if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
    }
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    DiamondValue converted=DIAMOND_NIL;
    DiamondVmStatus stringify_status=stringify_value(vm,chunk,depth,
        registers[base],&converted);
    if(stringify_status!=DIAMOND_VM_OK)return stringify_status;
    const DiamondString *text=(const DiamondString *)converted.as.object;
    errno=0;
    const ssize_t written=write(socket_handle->fd,text->chars,text->length);
    if(written<0) {
        const int saved_errno=errno;
        if(saved_errno==EAGAIN||saved_errno==EWOULDBLOCK) {
            snprintf(vm->error,sizeof vm->error,"write would block");
            return DIAMOND_VM_WOULD_BLOCK;
        }
        snprintf(vm->error,sizeof vm->error,"write error: %s",
            strerror(saved_errno));
        return DIAMOND_VM_IO_ERROR;
    }
    /* A partial write is a normal, expected outcome on a
     * non-blocking socket (the send buffer filled up
     * mid-write) -- unlike File#write, which either writes
     * everything or raises, this returns the actual byte
     * count written so the caller (packages/gremlin's
     * NonblockingConnection#write) can retry the remainder. */
    registers[dest]=DIAMOND_INT((int64_t)written);return DIAMOND_VM_OK;
}

/* The local port fd is bound to, which is how a socket opened on port 0
 * (the kernel picks a free one) is found out. */
static DiamondVmStatus bound_port(DiamondVm *vm,int fd,const char *what,DiamondValue *out) {
    struct sockaddr_storage address;
    socklen_t address_length=sizeof address;
    if(getsockname(fd,(struct sockaddr *)&address,&address_length)!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot read the %s's port: %s",what,
            strerror(errno));
        return DIAMOND_VM_IO_ERROR;
    }
    const int port=address.ss_family==AF_INET6
        ?ntohs(((struct sockaddr_in6 *)&address)->sin6_port)
        :ntohs(((struct sockaddr_in *)&address)->sin_port);
    *out=DIAMOND_INT(port);
    return DIAMOND_VM_OK;
}

DiamondVmStatus listener_invoke_helper(DiamondVm *vm,
                                              const DiamondChunk *chunk, size_t depth,
                                              DiamondValue *registers, uint16_t recv,
                                              uint8_t argc, uint16_t dest,
                                              const DiamondStringConstant *method_name) {
    DiamondListenerHandle *listener=
        (DiamondListenerHandle *)registers[recv].as.object;
    const bool accept_method=method_name->length==6&&
        memcmp(method_name->chars,"accept",6)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    const bool port_method=method_name->length==4&&
        memcmp(method_name->chars,"port",4)==0;
    if(!accept_method&&!close_method&&!port_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"Listener");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    if(port_method) {
        if(listener->fd<0) {
            snprintf(vm->error,sizeof vm->error,"listener is closed");
            return DIAMOND_VM_IO_ERROR;
        }
        return bound_port(vm,listener->fd,"listener",&registers[dest]);
    }
    if(close_method) {
        if(listener->fd>=0) {
            close(listener->fd);
            listener->fd=-1;
        }
        if(listener->tls_context!=nullptr) {
            SSL_CTX_free(listener->tls_context);
            listener->tls_context=nullptr;
        }
        free(listener->alpn_protocols);
        listener->alpn_protocols=nullptr;
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(listener->fd<0) {
        snprintf(vm->error,sizeof vm->error,"listener is closed");
        return DIAMOND_VM_IO_ERROR;
    }
    if(!listener->nonblocking) {
        const DiamondVmStatus wait_status=
            wait_fd_interruptible(vm,listener->fd,POLLIN);
        if(wait_status!=DIAMOND_VM_OK)return wait_status;
    }
    errno=0;
    int client_fd=diamond_accept(listener->fd);
    /* A blocking accept() can sit here indefinitely with
     * nothing connecting -- exactly when a trapped signal
     * needs to actually interrupt it (see
     * diamond_signal_handler's own comment: no
     * SA_RESTART, specifically so this EINTR happens)
     * rather than waiting for a connection that may never
     * arrive before the handler ever gets to run. Handles
     * the pending signal(s), then transparently retries --
     * a blocking listener's own .accept() semantics
     * (blocks until a real connection or a real error)
     * are unchanged from the caller's perspective. */
    while(client_fd<0&&errno==EINTR) {
        bool signal_invoked=false;
        const DiamondVmStatus signal_status=
            dispatch_pending_signals(vm,chunk,depth,&signal_invoked);
        if(signal_status!=DIAMOND_VM_OK)return signal_status;
        errno=0;
        client_fd=diamond_accept(listener->fd);
    }
    if(client_fd<0) {
        if(listener->nonblocking&&(errno==EAGAIN||errno==EWOULDBLOCK)) {
            registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
        }
        snprintf(vm->error,sizeof vm->error,"accept failed: %s",strerror(errno));
        return DIAMOND_VM_IO_ERROR;
    }
    if(listener->nonblocking) {
        /* Unlike some other platforms, Linux's accept() never
         * inherits O_NONBLOCK from the listening socket -- the
         * accepted connection comes back blocking by default
         * and must be set non-blocking explicitly, same as the
         * listener itself was in tcp_listen_helper. */
        const int flags=fcntl(client_fd,F_GETFL,0);
        if(flags<0||fcntl(client_fd,F_SETFL,flags|O_NONBLOCK)<0) {
            snprintf(vm->error,sizeof vm->error,
                "accept failed: %s",strerror(errno));
            close(client_fd);
            return DIAMOND_VM_IO_ERROR;
        }
        DiamondSocketHandle *client_socket=
            allocate_socket_handle(vm,client_fd);
        if(client_socket==nullptr) {
            close(client_fd);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
            .as.object=(DiamondObject *)client_socket};
        return DIAMOND_VM_OK;
    }
    if(listener->tls_context!=nullptr) {
        /* No EINTR-retry around SSL_accept itself, unlike
         * the accept() above it -- the "server sits idle
         * with nothing connecting" indefinite-wait case is
         * accept()'s alone; once a connection exists, the
         * handshake that follows is bounded (a couple of
         * network round trips), the same category
         * TCPSocket.connect's own connect() call is in, and
         * gets the same documented scope cut (see
         * tcp_connect_helper). An interruptible VM still waits
         * for the handshake in slices (tls_handshake_interruptible)
         * so a stalled peer cannot pin a supervised worker. */
        SSL *ssl=SSL_new(listener->tls_context);
        if(ssl==nullptr) {
            close(client_fd);
            char detail[256];tls_format_error(detail,sizeof detail);
            snprintf(vm->error,sizeof vm->error,
                "cannot create TLS session: %s",detail);
            return DIAMOND_VM_IO_ERROR;
        }
        SSL_set_fd(ssl,client_fd);
        int accept_result=0;
        const DiamondVmStatus accept_status=
            tls_handshake_interruptible(vm,ssl,true,&accept_result);
        if(accept_status!=DIAMOND_VM_OK) {
            SSL_free(ssl);close(client_fd);
            return accept_status;
        }
        if(accept_result!=1) {
            char detail[256];tls_format_error(detail,sizeof detail);
            snprintf(vm->error,sizeof vm->error,
                "TLS handshake failed: %s",detail);
            SSL_free(ssl);close(client_fd);
            return DIAMOND_VM_IO_ERROR;
        }
        DiamondTlsSocketHandle *client_tls=
            allocate_tls_socket_handle(vm,ssl,client_fd);
        if(client_tls==nullptr) {
            SSL_free(ssl);close(client_fd);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
            .as.object=(DiamondObject *)client_tls};
        return DIAMOND_VM_OK;
    }
    FILE *client_stream=fdopen(client_fd,"r+");
    if(client_stream==nullptr) {
        close(client_fd);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondFileHandle *client_handle=allocate_file_handle(vm,client_stream);
    if(client_handle==nullptr) {
        fclose(client_stream);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
        .as.object=(DiamondObject *)client_handle};
    return DIAMOND_VM_OK;
    return DIAMOND_VM_OK;
}

/* UDPSocket#send(data, host, port): one datagram to the first resolved
 * address that accepts it. */
static DiamondVmStatus udp_send_method(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,
        DiamondUdpSocketHandle *udp_handle,DiamondValue *registers,uint16_t base,uint8_t argc,
        uint16_t dest) {
    if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
    DiamondValue converted=DIAMOND_NIL;
    DiamondVmStatus stringify_status=stringify_value(vm,chunk,depth,
        registers[base],&converted);
    if(stringify_status!=DIAMOND_VM_OK)return stringify_status;
    const DiamondString *text=(const DiamondString *)converted.as.object;
    if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,
            "UDPSocket#send arguments must be (data, String host, Int port)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *host=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    char port_text[32];
    (void)snprintf(port_text,sizeof port_text,"%" PRId64,
                   registers[(size_t)base+2].as.integer);
    struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_DGRAM};
    struct addrinfo *results=nullptr;
    const int resolve_status=
        getaddrinfo(host->chars,port_text,&hints,&results);
    if(resolve_status!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot resolve '%.*s:%s': %s",
                 (int)host->length,host->chars,port_text,
                 gai_strerror(resolve_status));
        return DIAMOND_VM_IO_ERROR;
    }
    /* Tries each resolved candidate against this same
     * existing socket until one succeeds -- host may
     * resolve to both IPv4 and IPv6 addresses, and
     * sendto fails outright on a family mismatch with
     * whichever family this socket happened to be
     * created with (see udp_socket_helper), so this is
     * the sendto-time equivalent of TCPSocket.connect's
     * own try-each-candidate resilience. */
    ssize_t sent=-1;
    int last_errno=0;
    for(struct addrinfo *candidate=results;candidate!=nullptr;
        candidate=candidate->ai_next) {
        errno=0;
        sent=sendto(udp_handle->fd,text->chars,text->length,0,
                    candidate->ai_addr,candidate->ai_addrlen);
        if(sent>=0)break;
        last_errno=errno;
    }
    freeaddrinfo(results);
    if(sent<0) {
        snprintf(vm->error,sizeof vm->error,"send error: %s",
            strerror(last_errno));
        return DIAMOND_VM_IO_ERROR;
    }
    registers[dest]=DIAMOND_INT((int64_t)sent);return DIAMOND_VM_OK;
}

/* UDPSocket#receive(max_bytes): waits for one datagram and returns
 * {"data" => String, "host" => String, "port" => Int}. */
static DiamondVmStatus udp_receive_method(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,
        DiamondUdpSocketHandle *udp_handle,DiamondValue *registers,uint16_t base,uint8_t argc,
        uint16_t dest) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[base].as.integer<0) {
        snprintf(vm->error,sizeof vm->error,
            "UDPSocket#receive argument must be a non-negative Int");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const size_t want=(size_t)registers[base].as.integer;
    /* File#read/Socket#read/TLSSocket#read/Process::Stream#read
     * all accept 0 (an immediate empty result, no syscall
     * needed for those) -- this used to reject 0 outright,
     * an undocumented asymmetry with no behavioral reason
     * behind it. Allocating at least 1 byte regardless of
     * `want` sidesteps malloc(0)'s implementation-defined
     * result (may be nullptr, indistinguishable from real
     * OOM just below) while still passing the real `want`
     * to recvfrom below -- unlike the read family, a UDP
     * `.receive(0)` is a meaningful, distinct operation
     * (consumes/discards a queued datagram without copying
     * any of it), so this fix aligns the *validation* with
     * the read family without changing send/receive
     * semantics. */
    /* recv_len (never 0) is what's actually requested from
     * recvfrom -- FreeBSD leaves source_addr untouched
     * (ss_family stays the {0} initializer's AF_UNSPEC) on
     * a genuinely zero-length recvfrom, later failing
     * getnameinfo below with EAI_FAMILY ("Address family
     * not recognized"); confirmed directly, a real
     * receive(0) call on a real FreeBSD 15.1 box. Linux has
     * no such requirement (a zero-length recvfrom there
     * still populates the source address correctly), which
     * is why this was invisible before. `want` itself
     * (0 for a genuine receive(0) call) stays the source of
     * truth for how many bytes of the datagram to actually
     * surface as `data` below -- recvfrom always consumes/
     * discards the whole queued datagram regardless of how
     * much of it fits in the buffer, so asking for 1 byte
     * here changes nothing about receive(0)'s own
     * documented "consumes without copying" contract; it
     * only obtains the source address FreeBSD would
     * otherwise skip. */
    const size_t recv_len=want==0?1:want;
    {
        const DiamondVmStatus wait_status=
            wait_fd_interruptible(vm,udp_handle->fd,POLLIN);
        if(wait_status!=DIAMOND_VM_OK)return wait_status;
    }
    char *buffer=malloc(recv_len);
    if(buffer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    struct sockaddr_storage source_addr={0};
    socklen_t source_addr_len=sizeof source_addr;
    errno=0;
    ssize_t received=recvfrom(udp_handle->fd,buffer,recv_len,0,
        (struct sockaddr *)&source_addr,&source_addr_len);
    /* Same reasoning as blocking accept()/IO.poll above:
     * a UDP server loop's own .receive() can block
     * indefinitely with nothing arriving, exactly when a
     * trapped signal needs to interrupt it promptly. */
    while(received<0&&errno==EINTR) {
        bool signal_invoked=false;
        const DiamondVmStatus signal_status=
            dispatch_pending_signals(vm,chunk,depth,&signal_invoked);
        if(signal_status!=DIAMOND_VM_OK) {
            free(buffer);
            return signal_status;
        }
        source_addr_len=sizeof source_addr;
        errno=0;
        received=recvfrom(udp_handle->fd,buffer,recv_len,0,
            (struct sockaddr *)&source_addr,&source_addr_len);
    }
    if(received<0) {
        const int saved_errno=errno;
        free(buffer);
        snprintf(vm->error,sizeof vm->error,"receive error: %s",
            strerror(saved_errno));
        return DIAMOND_VM_IO_ERROR;
    }
    char host_buffer[NI_MAXHOST];
    char port_buffer[NI_MAXSERV];
    const int name_status=getnameinfo((struct sockaddr *)&source_addr,
        source_addr_len,host_buffer,sizeof host_buffer,
        port_buffer,sizeof port_buffer,NI_NUMERICHOST|NI_NUMERICSERV);
    if(name_status!=0) {
        free(buffer);
        snprintf(vm->error,sizeof vm->error,
            "cannot resolve sender address: %s",gai_strerror(name_status));
        return DIAMOND_VM_IO_ERROR;
    }
    const int64_t source_port=strtoll(port_buffer,nullptr,10);
    /* Same GC-safety pattern as DIAMOND_OP_IO_POLL's own
     * Hash result (see docs/io.md): root the Hash in
     * registers[dest] immediately, then for any entry whose
     * value needs its own further allocation, root the key
     * first with a nil placeholder -- nil needs no
     * protection -- before allocating the real value and
     * overwriting it. "port" is a scalar Int with no
     * allocation of its own, so its key+value can go in
     * with one hash_set, no placeholder needed. */
    DiamondHash *receive_result=allocate_hash(vm);
    if(receive_result==nullptr) {
        free(buffer);return DIAMOND_VM_OUT_OF_MEMORY;
    }
    registers[dest]=DIAMOND_OBJECT(receive_result);
    DiamondString *data_key=allocate_string(vm,"data",4);
    if(data_key==nullptr) {
        free(buffer);return DIAMOND_VM_OUT_OF_MEMORY;
    }
    if(!hash_set(vm,receive_result,DIAMOND_OBJECT(data_key),DIAMOND_NIL)) {
        free(buffer);return DIAMOND_VM_OUT_OF_MEMORY;
    }
    /* want, not recv_len/received: a genuine receive(0)
     * reports zero bytes of data regardless of the 1 byte
     * recv_len above may have actually copied into buffer
     * to get FreeBSD to populate source_addr -- see that
     * comment. */
    const size_t reported_length=want==0?0:(size_t)received;
    DiamondString *data_string=allocate_string(vm,buffer,reported_length);
    free(buffer);
    if(data_string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!hash_set(vm,receive_result,DIAMOND_OBJECT(data_key),
            DIAMOND_OBJECT(data_string)))
        return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondString *host_key=allocate_string(vm,"host",4);
    if(host_key==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!hash_set(vm,receive_result,DIAMOND_OBJECT(host_key),DIAMOND_NIL))
        return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondString *host_string=
        allocate_string(vm,host_buffer,strlen(host_buffer));
    if(host_string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!hash_set(vm,receive_result,DIAMOND_OBJECT(host_key),
            DIAMOND_OBJECT(host_string)))
        return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondString *port_key=allocate_string(vm,"port",4);
    if(port_key==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!hash_set(vm,receive_result,DIAMOND_OBJECT(port_key),
            DIAMOND_INT(source_port)))
        return DIAMOND_VM_OUT_OF_MEMORY;
    return DIAMOND_VM_OK;
}

/* TLSSocket#session: the received session as a DER blob for resumption, or nil. */
static DiamondVmStatus tls_session_method(DiamondVm *vm,DiamondTlsSocketHandle *tls_handle,
        DiamondValue *registers,uint8_t argc,uint16_t dest) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    if(tls_handle->received_session==nullptr) {
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    const int encoded_length=i2d_SSL_SESSION(tls_handle->received_session,nullptr);
    if(encoded_length<=0) {registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;}
    unsigned char *buffer=malloc((size_t)encoded_length);
    if(buffer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    unsigned char *cursor=buffer;
    i2d_SSL_SESSION(tls_handle->received_session,&cursor);
    DiamondString *blob=allocate_string(vm,(const char *)buffer,
        (size_t)encoded_length);
    free(buffer);
    if(blob==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(blob);return DIAMOND_VM_OK;
}

/* TLSSocket#peer_subject / #peer_fingerprint: the peer certificate's RFC 2253
 * subject or SHA-256 fingerprint, or nil when the peer presented none. */
static DiamondVmStatus tls_peer_certificate_method(DiamondVm *vm,DiamondTlsSocketHandle *tls_handle,
        DiamondValue *registers,uint8_t argc,uint16_t dest,bool peer_subject_method) {
    /* The certificate the other side presented: for an
     * accepted connection on a listener with a
     * client_ca, the verified client certificate; for a
     * client connection, the server's. nil when the
     * peer presented none (an ordinary TLS server never
     * asks a client for one). */
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    X509 *peer=SSL_get1_peer_certificate(tls_handle->ssl);
    if(peer==nullptr) {registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;}
    char text[512];size_t text_length=0;
    if(peer_subject_method) {
        BIO *bio=BIO_new(BIO_s_mem());
        if(bio==nullptr) {
            X509_free(peer);return DIAMOND_VM_OUT_OF_MEMORY;
        }
        X509_NAME_print_ex(bio,X509_get_subject_name(peer),0,
            XN_FLAG_RFC2253);
        const int got=BIO_read(bio,text,(int)sizeof text);
        BIO_free(bio);
        text_length=got>0?(size_t)got:0;
    } else {
        unsigned char digest[EVP_MAX_MD_SIZE];unsigned int digest_length=0;
        if(X509_digest(peer,EVP_sha256(),digest,&digest_length)==1&&
           digest_length*2<sizeof text) {
            for(unsigned int byte=0;byte<digest_length;byte++) {
                (void)snprintf(text+text_length,3,"%02x",digest[byte]);
                text_length+=2;
            }
        }
    }
    X509_free(peer);
    DiamondString *result_string=allocate_string(vm,text,text_length);
    if(result_string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(result_string);
    return DIAMOND_VM_OK;
}

/* TLSSocket#read([max_bytes]): until EOF, or at most max_bytes. */
static DiamondVmStatus tls_read_method(DiamondVm *vm,DiamondTlsSocketHandle *tls_handle,
        DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest) {
    if(argc>1)return DIAMOND_VM_ARITY_ERROR;
    bool bounded=false;size_t limit=0;
    if(argc==1) {
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[base].as.integer<0) {
            snprintf(vm->error,sizeof vm->error,
                "TLSSocket#read argument must be a non-negative Int");
            return DIAMOND_VM_TYPE_ERROR;
        }
        bounded=true;limit=(size_t)registers[base].as.integer;
    }
    StringBuilder builder={};
    char chunk_buffer[4096];
    for(;;) {
        if(bounded&&builder.length>=limit)break;
        const size_t remaining=bounded?limit-builder.length:sizeof chunk_buffer;
        const size_t want=remaining<sizeof chunk_buffer?
            remaining:sizeof chunk_buffer;
        size_t got=0;bool eof=false;
        const DiamondVmStatus read_status=
            tls_read_chunk(vm,tls_handle->ssl,chunk_buffer,want,&got,&eof);
        if(read_status!=DIAMOND_VM_OK) {
            free(builder.chars);return read_status;
        }
        if(eof||got==0)break;
        if(!builder_append(&builder,chunk_buffer,got)) {
            free(builder.chars);return DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    DiamondString *string=allocate_string(vm,builder.chars,builder.length);
    free(builder.chars);
    if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
}

/* TLSSocket#gets: one line, or nil at EOF. */
static DiamondVmStatus tls_gets_method(DiamondVm *vm,DiamondTlsSocketHandle *tls_handle,
        DiamondValue *registers,uint8_t argc,uint16_t dest) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    StringBuilder builder={};
    bool saw_any=false;
    const DiamondVmStatus read_status=
        tls_read_line(vm,tls_handle->ssl,&builder,&saw_any);
    if(read_status!=DIAMOND_VM_OK) {
        free(builder.chars);return read_status;
    }
    if(!saw_any) {
        free(builder.chars);
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    DiamondString *string=allocate_string(vm,builder.chars,builder.length);
    free(builder.chars);
    if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
}

/* TLSSocket#write(value): the value's string form, in full. */
static DiamondVmStatus tls_write_method(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,
        DiamondTlsSocketHandle *tls_handle,DiamondValue *registers,uint16_t base,uint8_t argc,
        uint16_t dest) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    DiamondValue converted=DIAMOND_NIL;
    DiamondVmStatus stringify_status=stringify_value(vm,chunk,depth,
        registers[base],&converted);
    if(stringify_status!=DIAMOND_VM_OK)return stringify_status;
    const DiamondString *text=(const DiamondString *)converted.as.object;
    const DiamondVmStatus write_status=
        tls_write_all(vm,tls_handle->ssl,text->chars,text->length);
    if(write_status!=DIAMOND_VM_OK)return write_status;
    registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
}

DiamondVmStatus udp_socket_invoke_helper(DiamondVm *vm,
                                                const DiamondChunk *chunk, size_t depth,
                                                DiamondValue *registers, uint16_t recv,
                                                uint16_t base, uint8_t argc, uint16_t dest,
                                                const DiamondStringConstant *method_name) {
    DiamondUdpSocketHandle *udp_handle=
        (DiamondUdpSocketHandle *)registers[recv].as.object;
    const bool send_method=method_name->length==4&&
        memcmp(method_name->chars,"send",4)==0;
    const bool receive_method=method_name->length==7&&
        memcmp(method_name->chars,"receive",7)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    const bool port_method=method_name->length==4&&
        memcmp(method_name->chars,"port",4)==0;
    if(!send_method&&!receive_method&&!close_method&&!port_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"UDPSocket");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(udp_handle->fd>=0) {
            close(udp_handle->fd);
            udp_handle->fd=-1;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(udp_handle->fd<0) {
        snprintf(vm->error,sizeof vm->error,"UDP socket is closed");
        return DIAMOND_VM_IO_ERROR;
    }
    if(port_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        return bound_port(vm,udp_handle->fd,"UDP socket",&registers[dest]);
    }
    if(send_method)
        return udp_send_method(vm,chunk,depth,udp_handle,registers,base,argc,dest);
    return udp_receive_method(vm,chunk,depth,udp_handle,registers,base,argc,dest);
}

DiamondVmStatus tls_socket_invoke_helper(DiamondVm *vm,
                                                const DiamondChunk *chunk, size_t depth,
                                                DiamondValue *registers, uint16_t recv,
                                                uint16_t base, uint8_t argc, uint16_t dest,
                                                const DiamondStringConstant *method_name) {
    DiamondTlsSocketHandle *tls_handle=
        (DiamondTlsSocketHandle *)registers[recv].as.object;
    const bool read_method=method_name->length==4&&
        memcmp(method_name->chars,"read",4)==0;
    const bool gets_method=method_name->length==4&&
        memcmp(method_name->chars,"gets",4)==0;
    const bool write_method=method_name->length==5&&
        memcmp(method_name->chars,"write",5)==0;
    const bool abort_method=method_name->length==5&&
        memcmp(method_name->chars,"abort",5)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    const bool alpn_protocol_method=method_name->length==13&&
        memcmp(method_name->chars,"alpn_protocol",13)==0;
    const bool session_method=method_name->length==7&&
        memcmp(method_name->chars,"session",7)==0;
    const bool session_reused_method=method_name->length==15&&
        memcmp(method_name->chars,"session_reused?",15)==0;
    const bool finish_handshake_method=method_name->length==16&&
        memcmp(method_name->chars,"finish_handshake",16)==0;
    const bool server_name_method=method_name->length==11&&
        memcmp(method_name->chars,"server_name",11)==0;
    const bool peer_subject_method=method_name->length==12&&
        memcmp(method_name->chars,"peer_subject",12)==0;
    const bool peer_fingerprint_method=method_name->length==16&&
        memcmp(method_name->chars,"peer_fingerprint",16)==0;
    if(!read_method&&!gets_method&&!write_method&&!close_method&&!abort_method&&
       !alpn_protocol_method&&!session_method&&!session_reused_method&&
       !peer_subject_method&&!peer_fingerprint_method&&!finish_handshake_method&&
       !server_name_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"TLSSocket");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(abort_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        tls_abort_handshake(tls_handle);
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(tls_handle->ssl!=nullptr) {
            if(!tls_handle->handshake_pending)SSL_shutdown(tls_handle->ssl);
            SSL_free(tls_handle->ssl);
            tls_handle->ssl=nullptr;
            if(tls_handle->fd>=0)close(tls_handle->fd);
            tls_handle->fd=-1;
        }
        if(tls_handle->received_session!=nullptr) {
            SSL_SESSION_free(tls_handle->received_session);
            tls_handle->received_session=nullptr;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(tls_handle->ssl==nullptr) {
        snprintf(vm->error,sizeof vm->error,"TLS socket is closed");
        return DIAMOND_VM_IO_ERROR;
    }
    if(finish_handshake_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        const char *direction=nullptr;
        const DiamondVmStatus handshake_status=diamond_tls_finish_handshake(vm,tls_handle,&direction);
        if(handshake_status!=DIAMOND_VM_OK)return handshake_status;
        registers[dest]=DIAMOND_NIL;
        if(direction!=nullptr) {
            DiamondString *value=allocate_string(vm,direction,strlen(direction));
            if(value==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
            registers[dest]=DIAMOND_OBJECT(value);
        }
        return DIAMOND_VM_OK;
    }
    if(tls_handle->handshake_pending) {
        snprintf(vm->error,sizeof vm->error,"TLS handshake is not complete");
        return DIAMOND_VM_IO_ERROR;
    }
    if(alpn_protocol_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        const unsigned char *data=nullptr;unsigned int length=0;
        SSL_get0_alpn_selected(tls_handle->ssl,&data,&length);
        if(data==nullptr||length==0) {registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;}
        DiamondString *protocol=allocate_string(vm,(const char *)data,length);
        if(protocol==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(protocol);return DIAMOND_VM_OK;
    }
    if(session_method)
        return tls_session_method(vm,tls_handle,registers,argc,dest);
    if(session_reused_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_BOOL(SSL_session_reused(tls_handle->ssl)==1);
        return DIAMOND_VM_OK;
    }
    if(server_name_method) {
        /* On an accepted connection: the host name the client asked for in
         * the SNI extension, or nil when it sent none (it never does for an
         * IP address). On a client connection it is the name it requested. */
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        const char *name=SSL_get_servername(tls_handle->ssl,TLSEXT_NAMETYPE_host_name);
        if(name==nullptr) {registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;}
        DiamondString *value=allocate_string(vm,name,strlen(name));
        if(value==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(value);
        return DIAMOND_VM_OK;
    }
    if(peer_subject_method||peer_fingerprint_method)
        return tls_peer_certificate_method(vm,tls_handle,registers,argc,dest,peer_subject_method);
    if(read_method)
        return tls_read_method(vm,tls_handle,registers,base,argc,dest);
    if(gets_method)
        return tls_gets_method(vm,tls_handle,registers,argc,dest);
    return tls_write_method(vm,chunk,depth,tls_handle,registers,base,argc,dest);
}
