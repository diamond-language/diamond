#ifndef REGINOLD_INTERNAL_SANITIZERS_H
#define REGINOLD_INTERNAL_SANITIZERS_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef assert
# define assert(expr) ((void)0)
#endif

#ifndef NO_SANITIZE
# define NO_SANITIZE(x, y) y
#endif

#ifndef MEMCPY
# define MEMCPY(p1, p2, type, n) memcpy((p1), (p2), sizeof(type) * (n))
#endif

#ifndef nlz_intptr
static inline unsigned nlz_intptr(uintptr_t x)
{
    if (x == 0) return (unsigned)(sizeof(uintptr_t) * 8);
#if UINTPTR_MAX == 0xffffffffu
    return (unsigned)__builtin_clz((unsigned)x);
#else
    return (unsigned)__builtin_clzl((unsigned long)x);
#endif
}
#endif

#endif
