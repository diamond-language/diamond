/* POSIX calls wasi-libc does not provide (sockets, name lookup, subprocesses, signal handlers,
 * temporary files). They fail the way an unsupported call fails everywhere else: -1 with errno
 * set, so the runtime reports an ordinary error ("cannot connect ...: Function not
 * implemented") instead of the page trapping on a missing import. Declarations come from the
 * headers in wasm/include. See docs/internal/wasm.md. */
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#define UNSUPPORTED_INT() do { errno = ENOSYS; return -1; } while (0)

int socket(int domain, int type, int protocol) { (void)domain; (void)type; (void)protocol; UNSUPPORTED_INT(); }
int setsockopt(int fd, int level, int name, const void *value, socklen_t length) {
    (void)fd; (void)level; (void)name; (void)value; (void)length; UNSUPPORTED_INT();
}
int bind(int fd, const struct sockaddr *address, socklen_t length) {
    (void)fd; (void)address; (void)length; UNSUPPORTED_INT();
}
int listen(int fd, int backlog) { (void)fd; (void)backlog; UNSUPPORTED_INT(); }
int connect(int fd, const struct sockaddr *address, socklen_t length) {
    (void)fd; (void)address; (void)length; UNSUPPORTED_INT();
}
int getpeername(int fd, struct sockaddr *address, socklen_t *length) {
    (void)fd; (void)address; (void)length; UNSUPPORTED_INT();
}
ssize_t sendto(int fd, const void *buffer, size_t length, int flags, const struct sockaddr *address,
               socklen_t address_length) {
    (void)fd; (void)buffer; (void)length; (void)flags; (void)address; (void)address_length;
    errno = ENOSYS; return -1;
}
ssize_t recvfrom(int fd, void *buffer, size_t length, int flags, struct sockaddr *address,
                 socklen_t *address_length) {
    (void)fd; (void)buffer; (void)length; (void)flags; (void)address; (void)address_length;
    errno = ENOSYS; return -1;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result) {
    (void)node; (void)service; (void)hints; *result = NULL; return EAI_FAIL;
}
void freeaddrinfo(struct addrinfo *result) { (void)result; }
const char *gai_strerror(int code) { (void)code; return "name lookup is not available in the browser"; }
int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, socklen_t host_length,
                char *service, socklen_t service_length, int flags) {
    (void)address; (void)length; (void)host; (void)host_length; (void)service;
    (void)service_length; (void)flags; return EAI_FAIL;
}

pid_t waitpid(pid_t pid, int *status, int options) { (void)pid; (void)status; (void)options; UNSUPPORTED_INT(); }
int kill(pid_t pid, int signal_number) { (void)pid; (void)signal_number; UNSUPPORTED_INT(); }
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attributes, char *const argv[], char *const environment[]) {
    (void)pid; (void)file; (void)actions; (void)attributes; (void)argv; (void)environment; return ENOSYS;
}
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions) { (void)actions; return 0; }
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions) { (void)actions; return 0; }
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int new_fd) {
    (void)actions; (void)fd; (void)new_fd; return 0;
}
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd) {
    (void)actions; (void)fd; return 0;
}
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions, int fd, const char *path,
                                     int flags, mode_t mode) {
    (void)actions; (void)fd; (void)path; (void)flags; (void)mode; return 0;
}

int sigemptyset(sigset_t *set) { (void)set; return 0; }
int sigaction(int signal_number, const struct sigaction *action, struct sigaction *previous) {
    (void)signal_number; (void)action; (void)previous; UNSUPPORTED_INT();
}

int mkstemp(char *pattern) { (void)pattern; UNSUPPORTED_INT(); }
