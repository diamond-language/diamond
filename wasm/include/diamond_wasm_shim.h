#ifndef WASM_SHIM_H
#define WASM_SHIM_H
#include <stdlib.h>
int mkstemp(char *);

char *realpath(const char *, char *);
#endif
#ifndef WASM_SHIM_EXTRA
#define WASM_SHIM_EXTRA
#include <__typedef_sigset_t.h>
#include <signal.h>
#include <sys/socket.h>
#ifndef SO_ERROR
#define SO_ERROR 4
#define SO_REUSEADDR 2
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_KEEPALIVE 9
#define SO_LINGER 13
#define SO_BROADCAST 6
#define SO_REUSEPORT 15
#define SOMAXCONN 128
#define SO_RCVBUF 8
#define SO_SNDBUF 7
#endif
#include <sys/socket.h>
#include <sys/types.h>
struct sigaction { void (*sa_handler)(int); sigset_t sa_mask; int sa_flags; };
#define SA_RESTART 0x10000000
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
int sigemptyset(sigset_t *); int sigaddset(sigset_t *, int); int sigfillset(sigset_t *);
int sigaction(int, const struct sigaction *, struct sigaction *);
int sigprocmask(int, const sigset_t *, sigset_t *);
int pthread_sigmask(int, const sigset_t *, sigset_t *);
int kill(pid_t, int);
int socket(int, int, int);
int setsockopt(int, int, int, const void *, socklen_t);
int getsockopt(int, int, int, void *, socklen_t *);
int bind(int, const struct sockaddr *, socklen_t);
int listen(int, int);
int connect(int, const struct sockaddr *, socklen_t);
int getsockname(int, struct sockaddr *, socklen_t *);
int getpeername(int, struct sockaddr *, socklen_t *);
ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
#endif
