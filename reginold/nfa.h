#ifndef REGIONOLD_NFA_H
#define REGIONOLD_NFA_H
/*
 * regionold/nfa.h
 *
 * Thompson NFA data structures and construction interface.
 *
 * The NFA is built by translating Onigmo's compiled bytecode into an
 * explicit state graph.  Onigmo's bytecode is already structured as an NFA
 * program:
 *
 *   OP_PUSH [addr]     → SPLIT(next_pc, addr)
 *   OP_JUMP [addr]     → EPSILON(addr)
 *   OP_EXACT1 c        → CHAR(c, next_pc)
 *   OP_CCLASS ...      → CCLASS(bitset, next_pc)
 *   OP_ANYCHAR         → ANY(next_pc)
 *   OP_MEMORY_START n  → SAVE(2n,   next_pc)   — Tier-2 only
 *   OP_MEMORY_END   n  → SAVE(2n+1, next_pc)
 *   OP_END / OP_FINISH → ACCEPT
 *   anchors            → ANCHOR(type, next_pc)
 *
 * OP_REPEAT / OP_REPEAT_INC encode bounded quantifiers.  We unroll these at
 * build time: the mandatory prefix is emitted as sequential CHAR/CLASS
 * nodes, then the optional suffix as SPLIT chains, then a loop SPLIT for
 * star/plus tails.  This keeps per-thread state constant — no counter
 * registers needed during simulation.
 *
 * Build note: compile with -I$(RUBY_SRC) (set via ONIGMO_CFLAGS in the Makefile).
 */

#include <stdint.h>
#include <stddef.h>

#include "onigmo_compat.h"

/* ── State types ─────────────────────────────────────────────────────────── */

typedef enum {
    NFA_ACCEPT,      /* accepting state — match found                       */
    NFA_SPLIT,       /* epsilon fork: two outgoing edges (out1, out2)        */
    NFA_CHAR,        /* match one byte: u.byte                               */
    NFA_CHARCLASS,   /* match one char against a 256-bit bitmap (+ mb ext)  */
    NFA_ANY,         /* match any byte except '\n'  (OP_ANYCHAR)             */
    NFA_ANY_ML,      /* match any byte including '\n' (OP_ANYCHAR_ML)        */
    NFA_CTYPE,       /* match a character type: \d \w \s etc.               */
    NFA_ANCHOR,      /* zero-width assertion: ^  $  \A  \z  \b etc.         */
    NFA_SAVE,        /* capture register write (Tier 2 only)                */
} NfaStateType;

/* Anchor subtypes — mirror the ANCHOR_* constants in regint.h */
typedef enum {
    NFA_ANCH_BOL   = 0,  /* ^   begin of line                    */
    NFA_ANCH_EOL   = 1,  /* $   end of line                      */
    NFA_ANCH_BOF   = 2,  /* \A  begin of string                  */
    NFA_ANCH_EOF   = 3,  /* \z  end of string (strict)           */
    NFA_ANCH_SEOF  = 4,  /* \Z  end of string (optional newline) */
    NFA_ANCH_WBOUND = 5, /* \b  word boundary                    */
    NFA_ANCH_NWBOUND = 6,/* \B  non-word boundary                */
    NFA_ANCH_POS   = 7,  /* \G  begin-of-search-position         */
} NfaAnchorType;

/* Character-class payload — stored inline for the 256-bit bitmap,
 * with an optional pointer for multibyte extensions (MB). */
#define NFA_CCLASS_BITMAP_WORDS 8   /* 8 × 32 bits = 256 bits */

typedef struct NfaCClass {
    uint32_t    bitmap[NFA_CCLASS_BITMAP_WORDS]; /* single-byte bitmap   */
    int         negative;   /* 1 → invert the class (OP_CCLASS_NOT etc.)  */
    /* multibyte extension — NULL for pure single-byte classes */
    UChar      *mb_data;    /* raw bytes from Onigmo's MB cclass payload   */
    size_t      mb_len;
} NfaCClass;

/* ── NFA state ───────────────────────────────────────────────────────────── */

typedef struct NfaState {
    NfaStateType type;
    int          id;    /* unique within an NfaGraph, used for bitset indexing */

    /* Primary output — used by all non-SPLIT states. */
    struct NfaState *out1;

    /* Secondary output — SPLIT only. */
    struct NfaState *out2;

    union {
        /* NFA_CHAR */
        struct { UChar byte; } ch;

        /* NFA_CHARCLASS */
        NfaCClass cclass;

        /* NFA_CTYPE: mirrors OnigCtype values from onigmo.h */
        struct { int ctype; int negative; } ctype;

        /* NFA_ANCHOR */
        struct { NfaAnchorType which; } anchor;

        /* NFA_SAVE: Laurikari save index.
         * Index = 2*group for group START, 2*group+1 for group END. */
        struct { int save_idx; } save;
    } u;
} NfaState;

/* ── NFA graph ───────────────────────────────────────────────────────────── */

/*
 * A complete NFA compiled from one regex.
 *
 * All NfaState nodes are arena-allocated out of the memory block pointed
 * to by `arena`.  Callers free the whole graph with nfa_graph_free().
 */
typedef struct NfaGraph {
    NfaState  *start;    /* entry state                              */
    NfaState  *accept;   /* the single shared ACCEPT state           */

    int        num_states;
    int        num_saves;    /* 2 * reg->num_mem (Tier 2), 0 (Tier 1)  */

    /* Arena allocator — all states live here. */
    NfaState  *arena;
    int        arena_cap;
    int        arena_used;

    /* Opaque buffer for NfaCClass mb_data payloads. */
    UChar     *mb_arena;
    size_t     mb_arena_cap;
    size_t     mb_arena_used;

    /* Encoding, for character-width queries during simulation. */
    OnigEncoding enc;
} NfaGraph;

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * Build an NFA from a compiled, Tier-1 or Tier-2 regex.
 *
 * Returns a heap-allocated NfaGraph on success, NULL on failure (memory
 * exhaustion or an opcode the builder doesn't handle — which shouldn't
 * happen for patterns the classifier approved).
 *
 * The caller owns the returned graph and must free it with nfa_graph_free().
 */
NfaGraph *nfa_build(const regex_t *reg);

/*
 * Release all memory associated with an NfaGraph.
 */
void nfa_graph_free(NfaGraph *g);

#ifdef REGIONOLD_DEBUG
/*
 * Dump a textual description of the NFA to stderr.  Useful for verifying
 * the translation of specific patterns.
 */
void nfa_graph_dump(const NfaGraph *g);
#endif

#endif /* REGIONOLD_NFA_H */
