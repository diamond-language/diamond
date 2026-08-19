/*
 * regionold/exec.c
 *
 * NFA simulation — Tier 1 (bitset Thompson) and Tier 2 (Laurikari tagged).
 *
 * Tier 1: a bitset-indexed queue of active states.  For each input byte, we
 * advance every matching state and compute the epsilon closure of its
 * successor.  No per-thread state; memory is O(m) where m = num_states.
 *
 * Tier 2: one thread slot per NFA state (the "occupant" model).  Each slot
 * may hold a save-register vector.  On SPLIT, the vector is cloned.  On
 * conflict (two threads reach the same state), the new thread wins
 * (corresponds to leftmost-match semantics when alternatives are explored
 * left-to-right).  Memory is O(m · k) where k = num capture boundaries.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "exec.h"

/* ── §1  Character / anchor helpers ────────────────────────────────────── */

static int
cclass_matches_byte(const NfaState *s, unsigned char byte)
{
    int word = byte / 32, bit = byte % 32;
    int hit  = (s->u.cclass.bitmap[word] >> bit) & 1;
    return s->u.cclass.negative ? !hit : hit;
}

static int
ctype_matches_byte(const NfaState *s, unsigned char byte)
{
    int r = 0;
    switch (s->u.ctype.ctype) {
    case ONIGENC_CTYPE_WORD:   r = isalnum(byte) || byte == '_'; break;
    case ONIGENC_CTYPE_DIGIT:  r = isdigit(byte);  break;
    case ONIGENC_CTYPE_SPACE:  r = isspace(byte);  break;
    case ONIGENC_CTYPE_UPPER:  r = isupper(byte);  break;
    case ONIGENC_CTYPE_LOWER:  r = islower(byte);  break;
    case ONIGENC_CTYPE_ALPHA:  r = isalpha(byte);  break;
    case ONIGENC_CTYPE_ALNUM:  r = isalnum(byte);  break;
    case ONIGENC_CTYPE_PRINT:  r = isprint(byte);  break;
    case ONIGENC_CTYPE_PUNCT:  r = ispunct(byte);  break;
    case ONIGENC_CTYPE_CNTRL:  r = iscntrl(byte);  break;
    case ONIGENC_CTYPE_XDIGIT: r = isxdigit(byte); break;
    default: r = 0; break;
    }
    return s->u.ctype.negative ? !r : r;
}

/* Returns 1 if state `s` matches `byte`. */
static int
state_matches_byte(const NfaState *s, unsigned char byte)
{
    switch (s->type) {
    case NFA_CHAR:      return s->u.ch.byte == byte;
    case NFA_CHARCLASS: return cclass_matches_byte(s, byte);
    case NFA_ANY:       return byte != '\n';
    case NFA_ANY_ML:    return 1;
    case NFA_CTYPE:     return ctype_matches_byte(s, byte);
    default:            return 0;
    }
}

typedef struct {
    const unsigned char *str;
    long                 len;
    long                 pos;  /* position between characters */
} AnchorCtx;

static int
anchor_matches(NfaAnchorType which, const AnchorCtx *a)
{
    long pos = a->pos;
    const unsigned char *str = a->str;
    long len = a->len;

    switch (which) {
    case NFA_ANCH_BOF:    return pos == 0;
    case NFA_ANCH_EOF:    return pos == len;
    case NFA_ANCH_SEOF:   return pos == len || (pos == len-1 && str[pos] == '\n');
    case NFA_ANCH_BOL:    return pos == 0 || str[pos-1] == '\n';
    case NFA_ANCH_EOL:    return pos == len || str[pos] == '\n';
    case NFA_ANCH_POS:    return 1;
    case NFA_ANCH_WBOUND: {
        int pre = pos > 0   && (isalnum(str[pos-1]) || str[pos-1]=='_');
        int cur = pos < len && (isalnum(str[pos])   || str[pos]  =='_');
        return pre != cur;
    }
    case NFA_ANCH_NWBOUND: {
        int pre = pos > 0   && (isalnum(str[pos-1]) || str[pos-1]=='_');
        int cur = pos < len && (isalnum(str[pos])   || str[pos]  =='_');
        return pre == cur;
    }
    default: return 0;
    }
}

/* ── §2  Tier-1 bitset simulation ──────────────────────────────────────── */

/*
 * A state set is an array-as-queue of NfaState* plus a visited bitset.
 * The visited bitset prevents adding a state twice per epsilon closure.
 */
typedef struct {
    NfaState      **q;
    int             count;
    int             cap;
    unsigned char  *visited;   /* visited[id/8] >> (id%8) & 1 */
    int             nvis_bytes;
} T1Set;

static int
t1set_init(T1Set *s, int n_states)
{
    s->q          = malloc((size_t)n_states * sizeof(NfaState *));
    s->count      = 0;
    s->cap        = n_states;
    s->nvis_bytes = (n_states + 7) / 8;
    s->visited    = calloc((size_t)s->nvis_bytes, 1);
    return (s->q && s->visited) ? 0 : -1;
}

static void t1set_free(T1Set *s) { free(s->q); free(s->visited); }

static void
t1set_reset(T1Set *s)
{
    s->count = 0;
    memset(s->visited, 0, (size_t)s->nvis_bytes);
}

/*
 * Recursively follow epsilon transitions from `state`, adding
 * character-consuming states (and ACCEPT) to the queue.
 */
static void
t1_eps_add(T1Set *s, NfaState *state, const AnchorCtx *ctx)
{
    if (!state) return;
    int id = state->id;
    if (s->visited[id >> 3] & (1u << (id & 7))) return;
    s->visited[id >> 3] |= (unsigned char)(1u << (id & 7));

    switch (state->type) {
    case NFA_SPLIT:
        t1_eps_add(s, state->out1, ctx);
        t1_eps_add(s, state->out2, ctx);
        return;
    case NFA_SAVE:          /* no captures in Tier 1 — just pass through */
        t1_eps_add(s, state->out1, ctx);
        return;
    case NFA_ANCHOR:
        if (anchor_matches(state->u.anchor.which, ctx))
            t1_eps_add(s, state->out1, ctx);
        return;
    default:
        s->q[s->count++] = state;
        return;
    }
}

/*
 * run_t1: simulate the NFA from `start_pos`.
 * Returns 1 on match (sets *out_end), 0 on no match, -1 on alloc error.
 * Finds the LONGEST match starting at start_pos.
 */
static int
run_t1(const NfaGraph *g, const unsigned char *str, long len,
       long start_pos, long *out_end)
{
    int found = 0;

    T1Set cur, nxt;
    if (t1set_init(&cur, g->num_states) < 0) return -1;
    if (t1set_init(&nxt, g->num_states) < 0) { t1set_free(&cur); return -1; }

    AnchorCtx ctx = { str, len, start_pos };

    t1_eps_add(&cur, g->start, &ctx);

    for (long pos = start_pos; cur.count > 0; pos++) {
        /* Check for ACCEPT before consuming the next byte. */
        for (int i = 0; i < cur.count; i++) {
            if (cur.q[i]->type == NFA_ACCEPT) {
                *out_end = pos;
                found = 1;
            }
        }

        if (pos >= len) break;

        ctx.pos = pos + 1;
        unsigned char byte = str[pos];

        t1set_reset(&nxt);
        for (int i = 0; i < cur.count; i++) {
            NfaState *s = cur.q[i];
            if (state_matches_byte(s, byte))
                t1_eps_add(&nxt, s->out1, &ctx);
        }

        T1Set tmp = cur; cur = nxt; nxt = tmp;
    }

    t1set_free(&cur);
    t1set_free(&nxt);
    return found;
}

/* ── §3  Tier-2 tagged NFA ──────────────────────────────────────────────── */

/*
 * Each NFA state may have at most one "occupant" thread at a time.
 * The thread owns a malloc'd array of `num_saves` longs (capture registers).
 * -1 means "not yet matched".
 *
 * We keep two maps: `cur` (current step) and `nxt` (next step).
 * After each character, we clear cur and swap.
 */
typedef struct {
    long       **regs;   /* regs[state_id] or NULL if no thread there  */
    int          n;      /* number of slots = num_states                */
    int          nsaves; /* number of save registers per thread         */
} T2Map;

static int
t2map_init(T2Map *m, int n_states, int nsaves)
{
    m->regs   = calloc((size_t)n_states, sizeof(long *));
    m->n      = n_states;
    m->nsaves = nsaves;
    return m->regs ? 0 : -1;
}

static void
t2map_free(T2Map *m)
{
    if (!m->regs) return;
    for (int i = 0; i < m->n; i++) {
        free(m->regs[i]);
        m->regs[i] = NULL;
    }
    free(m->regs);
    m->regs = NULL;
}

static void
t2map_clear(T2Map *m)
{
    for (int i = 0; i < m->n; i++) {
        free(m->regs[i]);
        m->regs[i] = NULL;
    }
}

static long *
regs_alloc(int nsaves)
{
    long *r = malloc((size_t)nsaves * sizeof(long));
    if (r) {
        for (int i = 0; i < nsaves; i++) r[i] = -1;
    }
    return r;
}

static long *
regs_clone(const long *src, int nsaves)
{
    long *r = malloc((size_t)nsaves * sizeof(long));
    if (r) memcpy(r, src, (size_t)nsaves * sizeof(long));
    return r;
}

/*
 * t2_eps_add: add a thread at `state` carrying `regs` (owned by caller),
 * following all epsilon transitions.
 *
 * `target` is the map being written to (cur on initial seed, nxt during step).
 * `src`    is used only for its allocator (so we can free displaced threads).
 *
 * Takes OWNERSHIP of `regs`; the thread either stores it or frees it.
 */
static void
t2_eps_add(T2Map *target, NfaState *state, long *regs,
           long pos, const AnchorCtx *ctx, int nsaves)
{
    if (!state || !regs) { free(regs); return; }

    switch (state->type) {
    case NFA_SPLIT: {
        long *regs2 = regs_clone(regs, nsaves);
        if (!regs2) { free(regs); return; }
        t2_eps_add(target, state->out1, regs,  pos, ctx, nsaves);
        t2_eps_add(target, state->out2, regs2, pos, ctx, nsaves);
        return;
    }
    case NFA_SAVE: {
        int idx = state->u.save.save_idx;
        if (idx >= 0 && idx < nsaves)
            regs[idx] = pos;
        t2_eps_add(target, state->out1, regs, pos, ctx, nsaves);
        return;
    }
    case NFA_ANCHOR:
        if (anchor_matches(state->u.anchor.which, ctx))
            t2_eps_add(target, state->out1, regs, pos, ctx, nsaves);
        else
            free(regs);
        return;
    default:
        break;
    }

    /* Character-consuming state or ACCEPT: place in target map. */
    int id = state->id;
    if (target->regs[id]) {
        /*
         * Conflict: keep new thread (leftmost-match semantics — the new
         * thread was enqueued first, corresponding to the left alternative).
         * Discard the existing one.
         */
        free(target->regs[id]);
    }
    target->regs[id] = regs;
}

/*
 * run_t2: tagged NFA simulation.
 * Returns 1 on match (writes match registers to `out_regs`), 0 on no match,
 * -1 on allocation error.
 *
 * `out_regs` must point to an array of `nsaves` longs (caller allocates).
 */
static int
run_t2(const NfaGraph *g, const unsigned char *str, long len,
       long start_pos, long *out_regs, int nsaves)
{
    int found = 0;

    T2Map cur, nxt;
    if (t2map_init(&cur, g->num_states, nsaves) < 0) return -1;
    if (t2map_init(&nxt, g->num_states, nsaves) < 0) {
        t2map_free(&cur); return -1;
    }

    AnchorCtx ctx = { str, len, start_pos };

    /* Seed: one thread at the start state. */
    long *init = regs_alloc(nsaves);
    if (!init) { t2map_free(&cur); t2map_free(&nxt); return -1; }
    t2_eps_add(&cur, g->start, init, start_pos, &ctx, nsaves);

    for (long pos = start_pos; ; pos++) {
        /* Check ACCEPT. */
        long *accept_regs = cur.regs[g->accept->id];
        if (accept_regs) {
            memcpy(out_regs, accept_regs, (size_t)nsaves * sizeof(long));
            /* Indices 0,1 are never written by NFA_SAVE nodes (they represent
             * the overall match, not a numbered capture group).  Record the
             * current simulation boundaries here instead. */
            out_regs[0] = start_pos;
            out_regs[1] = pos;
            found = 1;
        }

        if (pos >= len) break;

        /* Count active threads (skip if none). */
        int active = 0;
        for (int i = 0; i < g->num_states && !active; i++)
            if (cur.regs[i]) active = 1;
        if (!active) break;

        unsigned char byte = str[pos];
        ctx.pos = pos + 1;

        t2map_clear(&nxt);

        for (int i = 0; i < g->num_states; i++) {
            long *regs = cur.regs[i];
            if (!regs) continue;
            NfaState *s = &g->arena[i];  /* state ID == arena index */

            if (state_matches_byte(s, byte)) {
                long *nr = regs_clone(regs, nsaves);
                if (!nr) { found = -1; goto done; }
                t2_eps_add(&nxt, s->out1, nr, pos+1, &ctx, nsaves);
            }
        }

        t2map_clear(&cur);

        T2Map tmp = cur; cur = nxt; nxt = tmp;
    }

done:
    t2map_free(&cur);
    t2map_free(&nxt);
    return found;
}

/* ── §4  Public API ──────────────────────────────────────────────────────── */

static ExecResult
make_result(ExecStatus status, long ms, long me)
{
    ExecResult r = { status, ms, me };
    return r;
}

ExecResult
exec_match(const NfaGraph *g, const RegClassification *cls,
           const unsigned char *str, long len,
           long pos, ExecCaptures *caps)
{
    if (cls->tier == REG_TIER_1) {
        long end = -1;
        int r = run_t1(g, str, len, pos, &end);
        if (r < 0) return make_result(EXEC_ERROR,    -1, -1);
        if (!r)    return make_result(EXEC_MISMATCH, -1, -1);
        if (caps)  { caps->beg[0] = pos; caps->end[0] = end; }
        return make_result(EXEC_MATCH, pos, end);
    }

    /* Tier 2 */
    int nsaves = g->num_saves ? g->num_saves : 2;
    long *regs = regs_alloc(nsaves);
    if (!regs) return make_result(EXEC_ERROR, -1, -1);

    int r = run_t2(g, str, len, pos, regs, nsaves);
    ExecResult result;

    if (r < 0) {
        result = make_result(EXEC_ERROR, -1, -1);
    } else if (!r) {
        result = make_result(EXEC_MISMATCH, -1, -1);
    } else {
        long ms = regs[0], me = regs[1];
        result = make_result(EXEC_MATCH, ms, me);
        if (caps) {
            caps->beg[0] = ms; caps->end[0] = me;
            int ngroups = caps->num - 1;
            for (int i = 0; i < ngroups; i++) {
                int s = 2 * (i + 1), e = s + 1;
                caps->beg[i+1] = (s < nsaves) ? regs[s] : -1;
                caps->end[i+1] = (e < nsaves) ? regs[e] : -1;
            }
        }
    }
    free(regs);
    return result;
}

ExecResult
exec_search(const NfaGraph *g, const RegClassification *cls,
            const unsigned char *str, long len,
            long start_pos, ExecCaptures *caps)
{
    long pos = start_pos;
    while (pos <= len) {
        ExecResult r = exec_match(g, cls, str, len, pos, caps);
        if (r.status != EXEC_MISMATCH) return r;
        if (pos >= len) break;
        pos++;
    }
    return make_result(EXEC_MISMATCH, -1, -1);
}
