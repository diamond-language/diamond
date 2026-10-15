#ifndef WASM_STUB_NETDB_H
#define WASM_STUB_NETDB_H
#include <sys/socket.h>
#include <stddef.h>
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};
enum { AI_PASSIVE = 1, AI_CANONNAME = 2, AI_NUMERICHOST = 4, AI_NUMERICSERV = 0x400, AI_ADDRCONFIG = 0x20,
       NI_NUMERICHOST = 1, NI_NUMERICSERV = 2, NI_NAMEREQD = 8,
       EAI_AGAIN = -3, EAI_FAIL = -4, EAI_FAMILY = -6, EAI_MEMORY = -10, EAI_NONAME = -2, EAI_SERVICE = -8,
       EAI_SOCKTYPE = -7, EAI_SYSTEM = -11, EAI_OVERFLOW = -12 };
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
void freeaddrinfo(struct addrinfo *);
const char *gai_strerror(int);
int getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t, char *, socklen_t, int);
#endif
#define NI_MAXHOST 1025
#define NI_MAXSERV 32
