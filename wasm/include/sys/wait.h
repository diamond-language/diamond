#ifndef WASM_STUB_WAIT_H
#define WASM_STUB_WAIT_H
#include <sys/types.h>
#define WNOHANG 1
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s) ((s) & 0x7f)
pid_t waitpid(pid_t, int *, int);
pid_t wait(int *);
#endif
