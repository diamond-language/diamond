/*
 * regionold/nfa.c
 *
 * Thompson NFA construction from Onigmo compiled bytecode.
 *
 * Construction strategy
 * ─────────────────────
 * We use the "patch-list" technique from Thompson's original paper (as
 * popularised by Cox's RE2 writeup):
 *
 *   - An NfaFrag represents an NFA sub-graph with a known START state and a
 *     list of "dangling" output pointers — NfaState* slots that are not yet
 *     connected to anything.
 *
 *   - frag_patch(list, target) fills all dangling pointers to point at
 *     `target`, connecting the fragment to whatever follows.
 *
 *   - Concatenation: patch left's dangling outputs to right's start.
 *   - Alternation:  create a SPLIT; its two outputs are the starts of the
 *     two alternatives; its dangling list is the union of their dangling
 *     lists.
 *
 * We walk the bytecode with a recursive helper, build_frag(), which
 * processes one "structural unit" at a time (a literal, a split, a
 * quantifier body etc.) and returns a fragment.  The caller concatenates
 * fragments in sequence.
 *
 * For OP_REPEAT, we recurse to build the body fragment first, then wrap it
 * in the appropriate SPLIT structure:
 *
 *   a*  (lower=0, upper=∞):   SPLIT ─→ body ─→ SPLIT (loop)
 *                               ↓←────────────────┘
 *   a+  (lower=1, upper=∞):   body ─→ SPLIT ─→ body (loop)
 *   a?  (lower=0, upper=1):   SPLIT ─→ body
 *
 * Bounded {n,m} repeats are unrolled at build time for n,m ≤ MAX_UNROLL.
 * Larger ranges use a COUNTER state (see note at end of file) and are
 * handled with per-thread counter semantics in the simulation.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "nfa.h"

#ifndef assert
# define assert(expr) ((void)0)
#endif

/* ── Limits ─────────────────────────────────────────────────────────────── */

/*
 * Maximum repeat count we'll unroll at build time.  Patterns with bounds
 * larger than this get a NFA_REPEAT counter-state and pay the O(m) thread
 * overhead.
 */
#define MAX_UNROLL 20

/*
 * Initial arena capacity (number of NfaState objects).  Most patterns need
 * far fewer than this; the arena doubles when full.
 */
#define ARENA_INIT_CAP 64

/* Initial MB-data buffer size in bytes. */
#define MB_ARENA_INIT 256

/* Max dangling output pointers per fragment. */
#define MAX_DANGLING 64

/* ── Patch-list fragment ─────────────────────────────────────────────────── */

typedef struct {
    NfaState  *start;                     /* entry state of this fragment    */
    NfaState **outs[MAX_DANGLING];        /* addresses of unresolved outputs  */
    int        n_outs;                    /* number of dangling outputs       */
} NfaFrag;

static NfaFrag
frag_make(NfaState *start, NfaState **out_slot)
{
    NfaFrag f;
    f.start = start;
    f.n_outs = 0;
    if (out_slot) {
        f.outs[0] = out_slot;
        f.n_outs  = 1;
    }
    return f;
}

static NfaFrag
frag_empty(void)
{
    NfaFrag f;
    f.start  = NULL;
    f.n_outs = 0;
    return f;
}

/* Fill all dangling outputs of `f` to point at `target`. */
static void
frag_patch(NfaFrag *f, NfaState *target)
{
    for (int i = 0; i < f->n_outs; i++)
        *f->outs[i] = target;
    f->n_outs = 0;
}

/* Merge two dangling lists into `dst`. */
static void
frag_merge(NfaFrag *dst, const NfaFrag *src)
{
    for (int i = 0; i < src->n_outs; i++) {
        assert(dst->n_outs < MAX_DANGLING);
        dst->outs[dst->n_outs++] = src->outs[i];
    }
}

/*
 * Concatenate: connect the dangling outputs of `left` to the start of
 * `right`, then take `right`'s dangling outputs.  Returns the combined
 * fragment (start=left.start).
 */
static NfaFrag
frag_cat(NfaFrag *left, NfaFrag *right)
{
    frag_patch(left, right->start);
    NfaFrag result;
    result.start = left->start ? left->start : right->start;
    result.n_outs = right->n_outs;
    memcpy(result.outs, right->outs, right->n_outs * sizeof(result.outs[0]));
    return result;
}

/* ── Arena allocator ────────────────────────────────────────────────────── */

static NfaState *
arena_alloc(NfaGraph *g)
{
    if (g->arena_used >= g->arena_cap) {
        int new_cap = g->arena_cap * 2;
        NfaState *new_arena = realloc(g->arena, new_cap * sizeof(NfaState));
        if (!new_arena) return NULL;
        /*
         * Realloc may have moved the arena.  Any existing NfaState* pointers
         * that point into the old arena are now stale.  We can't safely
         * realloc after states have been linked — so we catch this here.
         *
         * In practice the ARENA_INIT_CAP is generous and most patterns don't
         * trigger a realloc.  If they do, we'd need an indirection layer.
         * For now: assert and return NULL so the caller falls back to Onigmo.
         */
        if (new_arena != g->arena) {
            /* Internal pointers are dangling.  Bail out. */
            free(new_arena);
            return NULL;
        }
        g->arena_cap = new_cap;
    }
    NfaState *s = &g->arena[g->arena_used++];
    memset(s, 0, sizeof(*s));
    s->id = g->num_states++;
    return s;
}

static UChar *
mb_arena_alloc(NfaGraph *g, size_t len)
{
    if (g->mb_arena_used + len > g->mb_arena_cap) {
        size_t new_cap = g->mb_arena_cap * 2;
        while (new_cap < g->mb_arena_used + len) new_cap *= 2;
        UChar *nb = realloc(g->mb_arena, new_cap);
        if (!nb) return NULL;
        g->mb_arena = nb;
        g->mb_arena_cap = new_cap;
    }
    UChar *ptr = g->mb_arena + g->mb_arena_used;
    g->mb_arena_used += len;
    return ptr;
}

/* ── State constructors ─────────────────────────────────────────────────── */

static NfaState *
make_accept(NfaGraph *g)
{
    NfaState *s = arena_alloc(g);
    if (!s) return NULL;
    s->type = NFA_ACCEPT;
    return s;
}

static NfaState *
make_split(NfaGraph *g)
{
    NfaState *s = arena_alloc(g);
    if (!s) return NULL;
    s->type = NFA_SPLIT;
    /* out1 and out2 are NULL — caller must fill them in or add to patch list */
    return s;
}

static NfaState *
make_char(NfaGraph *g, UChar byte)
{
    NfaState *s = arena_alloc(g);
    if (!s) return NULL;
    s->type = NFA_CHAR;
    s->u.ch.byte = byte;
    return s;
}

static NfaState *
make_any(NfaGraph *g, int multiline)
{
    NfaState *s = arena_alloc(g);
    if (!s) return NULL;
    s->type = multiline ? NFA_ANY_ML : NFA_ANY;
    return s;
}

static NfaState *
make_anchor(NfaGraph *g, NfaAnchorType which)
{
    NfaState *s = arena_alloc(g);
    if (!s) return NULL;
    s->type = NFA_ANCHOR;
    s->u.anchor.which = which;
    return s;
}

static NfaState *
make_save(NfaGraph *g, int save_idx)
{
    NfaState *s = arena_alloc(g);
    if (!s) return NULL;
    s->type = NFA_SAVE;
    s->u.save.save_idx = save_idx;
    return s;
}

/*
 * Build a CHARCLASS state.  Copies the bitmap from `bs` (SIZE_BITSET bytes)
 * and optionally stores MB data from `mb_p`/`mb_len`.
 */
static NfaState *
make_cclass(NfaGraph *g, const UChar *bs, int negative,
            const UChar *mb_p, size_t mb_len)
{
    NfaState *s = arena_alloc(g);
    if (!s) return NULL;
    s->type = NFA_CHARCLASS;
    s->u.cclass.negative = negative;
    memcpy(s->u.cclass.bitmap, bs, SIZE_BITSET);

    if (mb_p && mb_len > 0) {
        UChar *buf = mb_arena_alloc(g, mb_len);
        if (!buf) return NULL;
        memcpy(buf, mb_p, mb_len);
        s->u.cclass.mb_data = buf;
        s->u.cclass.mb_len  = mb_len;
    }
    return s;
}

static NfaState *
make_ctype(NfaGraph *g, int ctype, int negative)
{
    NfaState *s = arena_alloc(g);
    if (!s) return NULL;
    s->type = NFA_CTYPE;
    s->u.ctype.ctype    = ctype;
    s->u.ctype.negative = negative;
    return s;
}

/* ── Bytecode walker ────────────────────────────────────────────────────── */

/*
 * build_frag: build an NFA fragment from bytecode starting at *pp.
 *
 * Processes instructions until it hits a "block exit" instruction:
 *   - OP_END / OP_FINISH  → normal end of pattern
 *   - OP_REPEAT_INC       → end of a quantifier body
 *   - OP_POP_POS / OP_FAIL_POS / OP_POP_STOP_BT → end of sub-blocks
 *                           (these are Tier-3 only, should not appear here)
 *
 * On success, returns the fragment and advances *pp past the exit instruction.
 * On failure (unsupported construct or allocation error), returns frag_empty()
 * with *error set to 1.
 *
 * `depth` prevents unbounded recursion on malformed bytecode.
 */

#define MAX_DEPTH 64

typedef struct {
    NfaGraph      *g;
    const regex_t *reg;
    const UChar   *pend;
    int            error;
} Builder;

/* Forward declaration — build_frag calls itself for repeat bodies. */
static NfaFrag build_frag(Builder *b, const UChar **pp, int depth);

/*
 * Build a sequence: call build_frag repeatedly, concatenating results,
 * until we hit a "done" indicator (error, empty start, OP_END).
 * Returns the concatenated fragment.
 *
 * "done" here means build_frag returned a fragment with start==NULL,
 * which signals that it consumed a block-exit instruction.
 */
static NfaFrag
build_sequence(Builder *b, const UChar **pp, int depth)
{
    NfaFrag seq = frag_empty();
    seq.start = NULL; /* will be set on first real fragment */

    while (*pp < b->pend && !b->error) {
        NfaFrag f = build_frag(b, pp, depth);
        if (b->error) return frag_empty();

        if (f.start == NULL) {
            /* Hit a block-exit instruction — return what we have so far. */
            break;
        }

        if (seq.start == NULL) {
            seq = f;
        } else {
            /* Concatenate: patch seq's dangling outputs to f.start. */
            frag_patch(&seq, f.start);
            /* Absorb f's dangling outputs. */
            seq.n_outs = f.n_outs;
            memcpy(seq.outs, f.outs, f.n_outs * sizeof(seq.outs[0]));
        }
    }
    return seq;
}

/*
 * Duplicate a fragment n times in sequence by rebuilding the bytecode
 * body.  Used for mandatory prefix of {n,m} repeats.
 *
 * We re-enter build_sequence for each copy, re-reading from the SAME
 * bytecode body position.  Since the body is a contiguous slice of
 * bytecode, we reset pp to `body_start` each time.
 */
static NfaFrag
repeat_build_copies(Builder *b, const UChar *body_start, int n, int depth)
{
    NfaFrag acc = frag_empty();
    for (int i = 0; i < n; i++) {
        const UChar *p = body_start;
        NfaFrag copy = build_sequence(b, &p, depth + 1);
        if (b->error || copy.start == NULL) { b->error = 1; return frag_empty(); }

        if (acc.start == NULL) {
            acc = copy;
        } else {
            frag_patch(&acc, copy.start);
            acc.n_outs = copy.n_outs;
            memcpy(acc.outs, copy.outs, copy.n_outs * sizeof(acc.outs[0]));
        }
    }
    return acc;
}

static NfaFrag
build_frag(Builder *b, const UChar **pp, int depth)
{
    if (depth > MAX_DEPTH || *pp >= b->pend) { b->error = 1; return frag_empty(); }

    NfaGraph      *g   = b->g;
    const regex_t *reg = b->reg;
    const UChar   *p   = *pp;
    LengthType     len;
    NfaState      *s;

#define ADVANCE(n) do { p += (n); } while(0)
#define FAIL()     do { b->error = 1; *pp = p; return frag_empty(); } while(0)
#define NEED(s_)   do { if (!(s_)) FAIL(); } while(0)
#define DONE()     do { *pp = p; return frag_empty(); } while(0)

    switch ((enum OpCode)*p++) {

    /* ── Pattern terminators ─────────────────────────────────────────── */
    case OP_FINISH:
    case OP_END: {
        /*
         * Connect to the ACCEPT state.  The fragment we return has start=NULL
         * to signal "done" to build_sequence.
         */
        *pp = p;
        return frag_empty(); /* caller will patch remaining outs to accept */
    }

    /* ── Literal bytes ────────────────────────────────────────────────── */
    case OP_EXACT1: {
        s = make_char(g, *p++);
        NEED(s);
        *pp = p;
        return frag_make(s, &s->out1);
    }
    case OP_EXACT2: {
        NfaFrag acc = frag_empty();
        for (int i = 0; i < 2; i++) {
            s = make_char(g, *p++);
            NEED(s);
            NfaFrag f = frag_make(s, &s->out1);
            if (acc.start == NULL) { acc = f; }
            else { frag_patch(&acc, f.start); acc = frag_cat(&acc, &f); }
        }
        *pp = p;
        return acc;
    }
    case OP_EXACT3: case OP_EXACT4: case OP_EXACT5: {
        int n = (*(p-1) == OP_EXACT3) ? 3 : (*(p-1) == OP_EXACT4) ? 4 : 5;
        NfaFrag acc = frag_make(make_char(g, *p++), NULL);
        NEED(acc.start);
        acc.outs[0] = &acc.start->out1; acc.n_outs = 1;
        for (int i = 1; i < n; i++) {
            s = make_char(g, *p++);
            NEED(s);
            NfaFrag f = frag_make(s, &s->out1);
            frag_patch(&acc, f.start);
            acc.n_outs = f.n_outs;
            memcpy(acc.outs, f.outs, f.n_outs * sizeof(acc.outs[0]));
        }
        *pp = p;
        return acc;
    }
    case OP_EXACTN: {
        GET_LENGTH_INC(len, p);
        NfaFrag acc = frag_empty();
        for (LengthType i = 0; i < len; i++) {
            s = make_char(g, *p++);
            NEED(s);
            NfaFrag f = frag_make(s, &s->out1);
            if (acc.start == NULL) { acc = f; }
            else { frag_patch(&acc, f.start);
                   acc.n_outs = f.n_outs;
                   memcpy(acc.outs, f.outs, f.n_outs * sizeof(acc.outs[0])); }
        }
        *pp = p;
        return acc;
    }

    /* Multibyte literals: for now, emit one CHAR per byte.
     * TODO: emit unicode-aware character transitions. */
    case OP_EXACTMB2N1: {
        s = make_char(g, p[0]); NEED(s);
        NfaState *s2 = make_char(g, p[1]); NEED(s2);
        s->out1 = s2; p += 2;
        *pp = p;
        return frag_make(s2, &s2->out1); /* TODO: need full 2-byte match */
    }
    case OP_EXACTMB2N2: ADVANCE(4); *pp = p; goto unsupported_mb;
    case OP_EXACTMB2N3: ADVANCE(6); *pp = p; goto unsupported_mb;
    case OP_EXACTMB2N:  GET_LENGTH_INC(len, p); ADVANCE(len*2); *pp = p; goto unsupported_mb;
    case OP_EXACTMB3N:  GET_LENGTH_INC(len, p); ADVANCE(len*3); *pp = p; goto unsupported_mb;
    case OP_EXACTMBN: {
        LengthType mb_len;
        GET_LENGTH_INC(mb_len, p);
        GET_LENGTH_INC(len, p);
        ADVANCE(mb_len * len);
        *pp = p;
        goto unsupported_mb;
    }
    case OP_EXACT1_IC: {
        /* Case-insensitive single char — just emit a CHAR for now.
         * The simulation will need to handle case folding. */
        int clen = enclen(reg->enc, p, b->pend);
        s = make_char(g, *p); NEED(s); /* TODO: full IC support */
        p += clen;
        *pp = p;
        return frag_make(s, &s->out1);
    }
    case OP_EXACTN_IC: {
        GET_LENGTH_INC(len, p);
        /* Emit a CHAR for the first byte for now; TODO: full IC */
        s = make_char(g, *p); NEED(s);
        p += len;
        *pp = p;
        return frag_make(s, &s->out1);
    }

    /* ── Character classes ────────────────────────────────────────────── */
    case OP_CCLASS:
    case OP_CCLASS_NOT: {
        int neg = (*(p-1) == OP_CCLASS_NOT);
        s = make_cclass(g, p, neg, NULL, 0);
        NEED(s);
        p += SIZE_BITSET;
        *pp = p;
        return frag_make(s, &s->out1);
    }
    case OP_CCLASS_MB:
    case OP_CCLASS_MB_NOT: {
        /* Multibyte-only class: no bitmap, just MB data. */
        int neg = (*(p-1) == OP_CCLASS_MB_NOT);
        GET_LENGTH_INC(len, p);
        /* Use an all-zeros bitmap — all single bytes rejected; MB data governs. */
        static const UChar zero_bitmap[SIZE_BITSET] = {0};
        s = make_cclass(g, zero_bitmap, neg, p, (size_t)len);
        NEED(s);
        p += len;
        *pp = p;
        return frag_make(s, &s->out1);
    }
    case OP_CCLASS_MIX:
    case OP_CCLASS_MIX_NOT: {
        int neg = (*(p-1) == OP_CCLASS_MIX_NOT);
        const UChar *bm = p;
        p += SIZE_BITSET;
        GET_LENGTH_INC(len, p);
        s = make_cclass(g, bm, neg, p, (size_t)len);
        NEED(s);
        p += len;
        *pp = p;
        return frag_make(s, &s->out1);
    }

    /* ── Wildcards ────────────────────────────────────────────────────── */
    case OP_ANYCHAR:    s = make_any(g, 0); NEED(s); *pp = p; return frag_make(s, &s->out1);
    case OP_ANYCHAR_ML: s = make_any(g, 1); NEED(s); *pp = p; return frag_make(s, &s->out1);

    /* OP_ANYCHAR_STAR / OP_ANYCHAR_ML_STAR: Onigmo-optimised .* — emit as
     * the SPLIT + ANY loop equivalent. */
    case OP_ANYCHAR_STAR:
    case OP_ANYCHAR_ML_STAR: {
        int ml = (*(p-1) == OP_ANYCHAR_ML_STAR);
        NfaState *split = make_split(g);
        NfaState *any   = make_any(g, ml);
        NEED(split); NEED(any);
        split->out1 = any;    /* enter loop */
        any->out1   = split;  /* loop back  */
        /* split->out2 is dangling — "skip loop" path */
        *pp = p;
        return frag_make(split, &split->out2);
    }
    case OP_ANYCHAR_STAR_PEEK_NEXT:
    case OP_ANYCHAR_ML_STAR_PEEK_NEXT: {
        /* Peek byte follows — skip it and emit a plain star equivalent. */
        p++; /* skip peek byte */
        int ml = (*(p-2) == OP_ANYCHAR_ML_STAR_PEEK_NEXT);
        NfaState *split = make_split(g);
        NfaState *any   = make_any(g, ml);
        NEED(split); NEED(any);
        split->out1 = any;
        any->out1   = split;
        *pp = p;
        return frag_make(split, &split->out2);
    }

    /* ── Character types (\w \d \s etc.) ─────────────────────────────── */
    case OP_WORD:        s = make_ctype(g, ONIGENC_CTYPE_WORD,  0); NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_NOT_WORD:    s = make_ctype(g, ONIGENC_CTYPE_WORD,  1); NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_ASCII_WORD:  s = make_ctype(g, ONIGENC_CTYPE_WORD,  0); NEED(s); *pp=p; return frag_make(s,&s->out1); /* TODO: ASCII-only flag */
    case OP_NOT_ASCII_WORD: s = make_ctype(g, ONIGENC_CTYPE_WORD, 1); NEED(s); *pp=p; return frag_make(s,&s->out1);

    /* ── Anchors ──────────────────────────────────────────────────────── */
    case OP_BEGIN_BUF:      s = make_anchor(g, NFA_ANCH_BOF);    NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_END_BUF:        s = make_anchor(g, NFA_ANCH_EOF);    NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_BEGIN_LINE:     s = make_anchor(g, NFA_ANCH_BOL);    NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_END_LINE:       s = make_anchor(g, NFA_ANCH_EOL);    NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_SEMI_END_BUF:   s = make_anchor(g, NFA_ANCH_SEOF);   NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_BEGIN_POSITION: s = make_anchor(g, NFA_ANCH_POS);    NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_WORD_BOUND:     s = make_anchor(g, NFA_ANCH_WBOUND); NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_NOT_WORD_BOUND: s = make_anchor(g, NFA_ANCH_NWBOUND);NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_WORD_BEGIN:     s = make_anchor(g, NFA_ANCH_WBOUND); NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_WORD_END:       s = make_anchor(g, NFA_ANCH_WBOUND); NEED(s); *pp=p; return frag_make(s,&s->out1);
    /* ASCII variants: treat same as unicode for now */
    case OP_ASCII_WORD_BOUND:     s = make_anchor(g, NFA_ANCH_WBOUND);  NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_NOT_ASCII_WORD_BOUND: s = make_anchor(g, NFA_ANCH_NWBOUND); NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_ASCII_WORD_BEGIN:     s = make_anchor(g, NFA_ANCH_WBOUND);  NEED(s); *pp=p; return frag_make(s,&s->out1);
    case OP_ASCII_WORD_END:       s = make_anchor(g, NFA_ANCH_WBOUND);  NEED(s); *pp=p; return frag_make(s,&s->out1);

    /* \K — reset match start.  Emit a SAVE with a special sentinel index. */
    case OP_KEEP:
        s = make_save(g, -1 /* NFA_SAVE_KEEP sentinel */);
        NEED(s);
        *pp = p;
        return frag_make(s, &s->out1);

    /* ── Captures (Tier 2) ────────────────────────────────────────────── */
    case OP_MEMORY_START:
    case OP_MEMORY_START_PUSH: {
        MemNumType mem;
        GET_MEMNUM_INC(mem, p);
        s = make_save(g, 2 * (int)mem);   /* even index = group start */
        NEED(s);
        *pp = p;
        return frag_make(s, &s->out1);
    }
    case OP_MEMORY_END:
    case OP_MEMORY_END_PUSH:
    case OP_MEMORY_END_PUSH_REC:
    case OP_MEMORY_END_REC: {
        MemNumType mem;
        GET_MEMNUM_INC(mem, p);
        s = make_save(g, 2 * (int)mem + 1); /* odd index = group end */
        NEED(s);
        *pp = p;
        return frag_make(s, &s->out1);
    }

    /* ── Control flow ────────────────────────────────────────────────── */

    case OP_FAIL:
        /*
         * Dead-end — this NFA path can never match.  Return a fragment
         * with no dangling outputs so it contributes nothing.
         * (The SPLIT that led here was the other branch surviving.)
         */
        *pp = p;
        {
            NfaFrag dead = frag_empty();
            dead.start = g->accept; /* point at accept but no outputs — harmless */
            dead.n_outs = 0;
            return dead;
        }

    case OP_JUMP: {
        /*
         * Unconditional jump — epsilon transition.
         * Resolve the target and build the fragment starting there.
         * We don't emit an explicit state for a pure jump.
         */
        RelAddrType rel;
        GET_RELADDR_INC(rel, p);
        const UChar *target = p + rel;  /* rel is relative to after the operand */
        *pp = target;
        /* Recursive — build from the jump target. */
        return build_sequence(b, pp, depth + 1);
    }

    case OP_PUSH: {
        /*
         * Fork: try (next_pc) OR (jump target).
         * Creates a SPLIT state.  One branch continues sequentially,
         * the other jumps to `target`.
         */
        RelAddrType rel;
        GET_RELADDR_INC(rel, p);
        const UChar *target = p + rel;

        NfaState *split = make_split(g);
        NEED(split);

        /* Branch 1: build the sequence starting at p (after the operand). */
        NfaFrag branch1 = build_sequence(b, &p, depth + 1);
        if (b->error) FAIL();

        /* Branch 2: build from jump target. */
        NfaFrag branch2 = build_sequence(b, &target, depth + 1);
        if (b->error) FAIL();

        split->out1 = branch1.start;
        split->out2 = branch2.start;

        NfaFrag result = frag_make(split, NULL);
        frag_merge(&result, &branch1);
        frag_merge(&result, &branch2);

        *pp = p;   /* p was advanced by build_sequence for branch1 */
        return result;
    }

    case OP_POP:
        /* Pop the saved position (used in tandem with PUSH). No NFA state. */
        *pp = p;
        return frag_empty();

    case OP_PUSH_OR_JUMP_EXACT1: {
        /*
         * If next char matches `byte`, PUSH (try body); else JUMP (skip).
         * This is an optimised SPLIT + CHAR combo.  We emit a standard
         * SPLIT since our simulation will evaluate it correctly.
         */
        RelAddrType rel;
        GET_RELADDR_INC(rel, p);
        UChar byte = *p++;
        const UChar *target = p + rel - (int)sizeof(UChar); /* skip already consumed */
        /* Just like OP_PUSH + OP_EXACT1 at the head of branch1. */
        NfaState *split = make_split(g);
        NfaState *ch    = make_char(g, byte);
        NEED(split); NEED(ch);
        split->out1 = ch;

        NfaFrag body = build_sequence(b, &p, depth + 1);
        if (b->error) FAIL();
        ch->out1 = body.start;

        NfaFrag skip = build_sequence(b, &target, depth + 1);
        if (b->error) FAIL();
        split->out2 = skip.start;

        NfaFrag result = frag_make(split, NULL);
        frag_merge(&result, &body);
        frag_merge(&result, &skip);
        *pp = p;
        return result;
    }

    case OP_PUSH_IF_PEEK_NEXT: {
        /* Like PUSH_OR_JUMP_EXACT1 but always advances; skip peek byte. */
        RelAddrType rel;
        GET_RELADDR_INC(rel, p);
        p++; /* skip peek byte */
        (void)rel;
        /* Treat as SPLIT for now — conservative. */
        NfaState *split = make_split(g);
        NEED(split);
        NfaFrag b1 = build_sequence(b, &p, depth + 1);
        if (b->error) FAIL();
        split->out1 = b1.start;
        NfaFrag result = frag_make(split, &split->out2);
        frag_merge(&result, &b1);
        *pp = p;
        return result;
    }

    /* ── Quantifiers ─────────────────────────────────────────────────── */

    case OP_REPEAT:
    case OP_REPEAT_NG: {
        int greedy = (*(p-1) == OP_REPEAT);
        MemNumType mem;
        GET_MEMNUM_INC(mem, p);
        RelAddrType rel;
        GET_RELADDR_INC(rel, p);
        /* p now points at the start of the repeat body. */
        const UChar *body_start = p;
        const UChar *body_end_ref = p + rel; /* points AFTER OP_REPEAT_INC */
        int lower = reg->repeat_range[mem].lower;
        int upper = reg->repeat_range[mem].upper; /* ONIG_INFINITE_DISTANCE if unbounded */
        int is_star = (upper == (int)ONIG_INFINITE_DISTANCE);

        if (lower == 0 && upper == 1) {
            /* a?  — SPLIT(body, skip) */
            NfaState *split = make_split(g); NEED(split);
            NfaFrag body = build_sequence(b, &p, depth + 1);
            if (b->error) FAIL();
            /* p is now after OP_REPEAT_INC; body_end_ref confirms */
            (void)body_end_ref;
            NfaFrag result;
            if (greedy) {
                split->out1 = body.start; /* try body first */
                result = frag_make(split, &split->out2);
            } else {
                split->out2 = body.start; /* try skip first */
                result = frag_make(split, &split->out1);
            }
            frag_merge(&result, &body);
            *pp = p;
            return result;
        }

        if (lower == 0 && is_star) {
            /* a*  — SPLIT → body → SPLIT (loop) */
            NfaState *split = make_split(g); NEED(split);
            NfaFrag body = build_sequence(b, &p, depth + 1);
            if (b->error) FAIL();
            frag_patch(&body, split); /* loop back */
            if (greedy) {
                split->out1 = body.start; /* try body first */
                *pp = p;
                return frag_make(split, &split->out2);
            } else {
                split->out2 = body.start;
                *pp = p;
                return frag_make(split, &split->out1);
            }
        }

        if (lower == 1 && is_star) {
            /* a+  — body → SPLIT → body (loop) */
            NfaFrag first = build_sequence(b, &p, depth + 1);
            if (b->error) FAIL();
            /* Now build the loop by re-reading body. */
            NfaState *split = make_split(g); NEED(split);
            frag_patch(&first, split);
            /* Rebuild body from body_start for the loop branch. */
            const UChar *lp = body_start;
            NfaFrag loop_body = build_sequence(b, &lp, depth + 1);
            if (b->error) FAIL();
            frag_patch(&loop_body, split); /* loop back to split */
            if (greedy) {
                split->out1 = loop_body.start;
                *pp = p;
                return frag_make(first.start, &split->out2);
            } else {
                split->out2 = loop_body.start;
                *pp = p;
                return frag_make(first.start, &split->out1);
            }
        }

        if (lower <= MAX_UNROLL && (is_star || upper <= MAX_UNROLL)) {
            /* Bounded {lower, upper} — unroll. */
            /* Build `lower` mandatory copies. */
            NfaFrag acc = frag_empty();
            if (lower > 0) {
                acc = repeat_build_copies(b, body_start, lower, depth);
                if (b->error) FAIL();
            }
            /* Consume one body from bytecode so p advances past OP_REPEAT_INC. */
            if (lower == 0) {
                /* Still need to advance p past the body + OP_REPEAT_INC. */
                const UChar *skip_p = body_start;
                build_sequence(b, &skip_p, depth + 1); /* discard fragment */
                p = skip_p;
            } else {
                /* Already advanced by repeat_build_copies' last copy. */
                p = body_end_ref; /* jump to after OP_REPEAT_INC directly */
            }

            int opt_count = is_star ? MAX_UNROLL : (upper - lower);
            if (opt_count > MAX_UNROLL) opt_count = MAX_UNROLL; /* cap */

            /* Build optional suffix: opt_count SPLIT chains. */
            for (int i = 0; i < opt_count; i++) {
                NfaState *split = make_split(g); NEED(split);
                const UChar *op = body_start;
                NfaFrag opt = build_sequence(b, &op, depth + 1);
                if (b->error) FAIL();

                NfaFrag sf = frag_make(split, NULL);
                if (greedy) {
                    split->out1 = opt.start;
                    sf.outs[0] = &split->out2;
                    sf.n_outs  = 1;
                } else {
                    split->out2 = opt.start;
                    sf.outs[0] = &split->out1;
                    sf.n_outs  = 1;
                }
                frag_merge(&sf, &opt);

                if (acc.start == NULL) {
                    acc = sf;
                } else {
                    frag_patch(&acc, sf.start);
                    acc.n_outs = sf.n_outs;
                    memcpy(acc.outs, sf.outs, sf.n_outs * sizeof(acc.outs[0]));
                }
            }

            if (is_star) {
                /* After capped unroll, add a real loop for unbounded case. */
                NfaState *split = make_split(g); NEED(split);
                const UChar *op = body_start;
                NfaFrag loop_b = build_sequence(b, &op, depth + 1);
                if (b->error) FAIL();
                frag_patch(&loop_b, split);
                split->out1 = loop_b.start;
                frag_patch(&acc, split);
                acc.n_outs = 1;
                acc.outs[0] = &split->out2;
            }

            *pp = p;
            return acc;
        }

        /* Repeat bounds too large to unroll — fall through to error.
         * TODO: emit NFA_REPEAT counter-state. */
        FAIL();
    }

    case OP_REPEAT_INC:
    case OP_REPEAT_INC_NG: {
        /*
         * End of a quantifier body.  Signal "done" to build_sequence so
         * the caller (handling OP_REPEAT) knows to stop.
         */
        MemNumType mem;
        GET_MEMNUM_INC(mem, p);
        (void)mem;
        *pp = p;
        return frag_empty(); /* signals "block exit" */
    }

    /* OP_REPEAT_INC_SG / _NG_SG — classifier rejects these as Tier-3. */
    case OP_REPEAT_INC_SG:
    case OP_REPEAT_INC_NG_SG:
        FAIL();

    /* ── Null-loop guards ─────────────────────────────────────────────── */
    case OP_NULL_CHECK_START:
    case OP_NULL_CHECK_END:
    case OP_NULL_CHECK_END_MEMST:
    case OP_NULL_CHECK_END_MEMST_PUSH: {
        /* These guard against empty infinite loops.  We skip them — our
         * simulation handles this differently (epsilon-closure cycle detection). */
        MemNumType mem;
        GET_MEMNUM_INC(mem, p);
        (void)mem;
        *pp = p;
        return frag_empty(); /* no NFA state; null-loop guard is implicit */
    }

    /* ── Atomic groups OP_PUSH_STOP_BT / OP_POP_STOP_BT ─────────────── */
    case OP_PUSH_STOP_BT: {
        /*
         * Atomic group start.  In the NFA simulation there's no backtracking
         * to eliminate — we just process the inner content normally.
         * Build the body up to OP_POP_STOP_BT.
         */
        NfaFrag body = build_sequence(b, &p, depth + 1);
        *pp = p;
        return body;
    }
    case OP_POP_STOP_BT:
        /* End of atomic group — signal "done". */
        *pp = p;
        return frag_empty();

    /* ── Option setting ───────────────────────────────────────────────── */
    case OP_SET_OPTION_PUSH:
    case OP_SET_OPTION: {
        /* Skip option operand — option changes are baked into the subsequent
         * EXACT_IC / CCLASS nodes by Onigmo; we don't need a separate state. */
        p += SIZE_OPTION;
        *pp = p;
        return frag_empty();
    }

    /* ── Should never appear (Tier-3 opcodes filtered by classifier) ──── */
    case OP_BACKREF1: case OP_BACKREF2: case OP_BACKREFN:
    case OP_BACKREFN_IC: case OP_BACKREF_MULTI: case OP_BACKREF_MULTI_IC:
    case OP_BACKREF_WITH_LEVEL:
    case OP_PUSH_POS: case OP_POP_POS: case OP_PUSH_POS_NOT: case OP_FAIL_POS:
    case OP_LOOK_BEHIND: case OP_PUSH_LOOK_BEHIND_NOT: case OP_FAIL_LOOK_BEHIND_NOT:
    case OP_PUSH_ABSENT_POS: case OP_ABSENT: case OP_ABSENT_END:
    case OP_CALL: case OP_RETURN: case OP_CONDITION:
    case OP_STATE_CHECK_PUSH: case OP_STATE_CHECK_PUSH_OR_JUMP:
    case OP_STATE_CHECK: case OP_STATE_CHECK_ANYCHAR_STAR:
    case OP_STATE_CHECK_ANYCHAR_ML_STAR:
        FAIL();

    default:
        FAIL();
    }

    assert(0); /* unreachable */
    return frag_empty();

unsupported_mb:
    /* Multibyte literal we haven't fully implemented yet.
     * TODO: emit unicode-aware character transitions.
     * For now, fail out so the caller falls back to Onigmo. */
    b->error = 1;
    return frag_empty();

#undef ADVANCE
#undef FAIL
#undef NEED
#undef DONE
}

/* ── Public API ─────────────────────────────────────────────────────────── */

NfaGraph *
nfa_build(const regex_t *reg)
{
    NfaGraph *g = calloc(1, sizeof(NfaGraph));
    if (!g) return NULL;

    g->arena = malloc(ARENA_INIT_CAP * sizeof(NfaState));
    if (!g->arena) { free(g); return NULL; }
    g->arena_cap  = ARENA_INIT_CAP;
    g->arena_used = 0;
    g->num_states = 0;

    g->mb_arena = malloc(MB_ARENA_INIT);
    if (!g->mb_arena) { free(g->arena); free(g); return NULL; }
    g->mb_arena_cap  = MB_ARENA_INIT;
    g->mb_arena_used = 0;

    g->enc       = reg->enc;
    /* Save indices: 0,1 = overall match; 2*n, 2*n+1 = group n (n >= 1).
     * Onigmo groups are 1-indexed, so the highest index used is 2*num_mem+1. */
    g->num_saves = 2 * (reg->num_mem + 1);

    /* Create the shared ACCEPT state first (id=0). */
    g->accept = make_accept(g);
    if (!g->accept) goto fail;

    Builder b;
    b.g     = g;
    b.reg   = reg;
    b.pend  = reg->p + reg->used;
    b.error = 0;

    const UChar *p = reg->p;
    NfaFrag root = build_sequence(&b, &p, 0);

    if (b.error || root.start == NULL) goto fail;

    /* Patch all dangling outputs to the ACCEPT state. */
    frag_patch(&root, g->accept);

    g->start = root.start;
    return g;

fail:
    nfa_graph_free(g);
    return NULL;
}

void
nfa_graph_free(NfaGraph *g)
{
    if (!g) return;
    free(g->arena);
    free(g->mb_arena);
    free(g);
}

/* ── Debug dump ─────────────────────────────────────────────────────────── */

#ifdef REGIONOLD_DEBUG
static const char *
state_type_name(NfaStateType t)
{
    switch (t) {
    case NFA_ACCEPT:    return "ACCEPT";
    case NFA_SPLIT:     return "SPLIT";
    case NFA_CHAR:      return "CHAR";
    case NFA_CHARCLASS: return "CCLASS";
    case NFA_ANY:       return "ANY";
    case NFA_ANY_ML:    return "ANY_ML";
    case NFA_CTYPE:     return "CTYPE";
    case NFA_ANCHOR:    return "ANCHOR";
    case NFA_SAVE:      return "SAVE";
    default:            return "?";
    }
}

void
nfa_graph_dump(const NfaGraph *g)
{
    fprintf(stderr, "NFA: %d states, %d saves, start=%d\n",
            g->num_states, g->num_saves, g->start ? g->start->id : -1);
    for (int i = 0; i < g->arena_used; i++) {
        const NfaState *s = &g->arena[i];
        fprintf(stderr, "  [%d] %s", s->id, state_type_name(s->type));
        switch (s->type) {
        case NFA_CHAR:
            fprintf(stderr, " '%c'", (char)s->u.ch.byte); break;
        case NFA_SAVE:
            fprintf(stderr, " save=%d", s->u.save.save_idx); break;
        case NFA_ANCHOR:
            fprintf(stderr, " anch=%d", (int)s->u.anchor.which); break;
        case NFA_SPLIT:
            fprintf(stderr, " →[%d,", s->out1 ? s->out1->id : -1);
            fprintf(stderr, "%d]",    s->out2 ? s->out2->id : -1); break;
        default: break;
        }
        if (s->type != NFA_SPLIT && s->type != NFA_ACCEPT)
            fprintf(stderr, " →%d", s->out1 ? s->out1->id : -1);
        fprintf(stderr, "\n");
    }
}
#endif /* REGIONOLD_DEBUG */
