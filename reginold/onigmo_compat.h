#ifndef REGINOLD_ONIGMO_COMPAT_H
#define REGINOLD_ONIGMO_COMPAT_H
/*
 * onigmo_compat.h — internal gateway for Onigmo types and bytecode internals.
 *
 * INTERNAL to reginold. Nothing in reginold.h includes this header.
 * Consumers of reginold only need reginold.h; they never see Onigmo types.
 *
 * Why regint.h and not the public include/ruby/onigmo.h:
 *   classify.c and nfa.c inspect Onigmo's compiled bytecode directly — they
 *   read the opcode stream inside regex_t. That requires regint.h. The public
 *   onigmo.h does not expose regex_t internals.
 *
 * Build requirement: CFLAGS must include -I$(RUBY_SRC) for regint.h to resolve.
 *   The Makefile sets this via ONIGMO_CFLAGS; it is never propagated to consumers.
 */

#include "regint.h"

#endif /* REGINOLD_ONIGMO_COMPAT_H */
