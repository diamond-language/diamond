/*
 * regionold/engine.c
 *
 * Integration layer — routes compiled patterns to the correct execution tier.
 *
 * Design notes:
 *
 *   1. Onigmo is always compiled (onig_new).  For Tier 1/2, we additionally
 *      build our NFA so we can bypass Onigmo's backtracking engine during
 *      search.  The Onigmo regex is kept because:
 *        - It is the authoritative source of truth for Tier 3 patterns.
 *        - nfa_build() reads the bytecode from regex_t, so the regex must
 *          stay alive for the NFA's entire lifetime.
 *        - It makes the fallback path trivially correct.
 *
 *   2. Backward search (range < start) always uses the Onigmo path, because
 *      our simulation is forward-only.
 *
 *   3. OnigRegion uses `int` for byte offsets; our ExecCaptures uses `long`.
 *      We bridge the gap in populate_region() below.
 *
 *   4. ONIGENC_MBC_MAXLEN > 1 patterns are still Tier 1 or Tier 2 — the NFA
 *      handles multi-byte characters in exec.c — but the classifier sets
 *      cls.is_multibyte so the executor knows to do width-aware stepping.
 */

#include <stdlib.h>
#include <string.h>

#include "engine.h"

/* ── Region helpers ──────────────────────────────────────────────────────── */

/*
 * populate_region: copy ExecCaptures into an OnigRegion.
 *
 * ExecCaptures.beg/end are long[num]; OnigRegion.beg/end are int[num_regs].
 * onig_region_resize ensures the region has enough capacity.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
static int
populate_region(OnigRegion *region, const ExecCaptures *caps)
{
    int n = caps->num;

    if (onig_region_resize(region, n) != ONIG_NORMAL)
        return -1;

    region->num_regs = n;
    for (int i = 0; i < n; i++) {
        region->beg[i] = (int)caps->beg[i];
        region->end[i] = (int)caps->end[i];
    }
    return 0;
}

/*
 * alloc_caps: allocate an ExecCaptures for `num` groups (0 = overall match).
 * Returns 0 on success.
 */
static int
alloc_caps(ExecCaptures *caps, int num)
{
    caps->num = num;
    caps->beg = malloc((size_t)num * sizeof(long));
    caps->end = malloc((size_t)num * sizeof(long));
    if (!caps->beg || !caps->end) {
        free(caps->beg);
        free(caps->end);
        return -1;
    }
    /* -1 = not captured */
    for (int i = 0; i < num; i++) caps->beg[i] = caps->end[i] = -1;
    return 0;
}

static void
free_caps(ExecCaptures *caps)
{
    free(caps->beg);
    free(caps->end);
}

/* ── regold_new ──────────────────────────────────────────────────────────── */

int
regold_new(RegoldEngine       **out,
           const OnigUChar     *pattern,
           const OnigUChar     *pattern_end,
           OnigOptionType       options,
           OnigEncoding         enc,
           const OnigSyntaxType *syntax,
           OnigErrorInfo        *einfo)
{
    RegoldEngine *e = malloc(sizeof(*e));
    if (!e) return ONIGERR_MEMORY;

    /* Always compile with Onigmo — for correctness and Tier-3 fallback. */
    int r = onig_new(&e->onig, pattern, pattern_end, options, enc, syntax, einfo);
    if (r != ONIG_NORMAL) {
        free(e);
        return r;
    }

    e->cls = reg_classify(e->onig);

    /* Build our NFA for Tier 1 and Tier 2 patterns.
     *
     * Multibyte encodings (UTF-8, EUC-JP, …) require character-level
     * simulation for NFA_ANY / NFA_ANY_ML.  Our simulation is byte-level,
     * so fall back to Onigmo for any multibyte encoding for now. */
    if (e->cls.tier <= REG_TIER_2 && !e->cls.is_multibyte) {
        e->nfa = nfa_build(e->onig);
        if (!e->nfa) {
            /* NFA build failure — demote to Tier 3 (Onigmo fallback). */
            e->cls.tier = REG_TIER_3;
            e->cls.tier3_reason = REG_T3_BYTECODE_ERR;
        }
    } else {
        e->nfa = NULL;
    }

    *out = e;
    return ONIG_NORMAL;
}

/* ── regold_free ─────────────────────────────────────────────────────────── */

void
regold_free(RegoldEngine *e)
{
    if (!e) return;
    nfa_graph_free(e->nfa);   /* no-op if NULL */
    onig_free(e->onig);
    free(e);
}

/* ── Internal: run our engine for a match anchored at one start position ── */

/*
 * run_our_engine: call exec_match for Tier 1 or Tier 2.
 *
 * str/len/pos describe the input.  If `region` is non-NULL and the tier is
 * Tier 2, captures are populated.
 *
 * Returns:
 *   >= 0  match length
 *   ONIG_MISMATCH
 *   ONIGERR_MEMORY on allocation failure
 */
static int
run_our_engine(const RegoldEngine *e,
               const OnigUChar    *str,
               long                len,
               long                pos,
               OnigRegion         *region)
{
    if (e->cls.tier == REG_TIER_1) {
        /* No captures needed. */
        ExecResult r = exec_match(e->nfa, &e->cls, str, len, pos, NULL);
        if (r.status == EXEC_MATCH) {
            if (region) {
                /* Resize region to hold just the overall match. */
                if (onig_region_resize(region, 1) != ONIG_NORMAL)
                    return ONIGERR_MEMORY;
                region->num_regs = 1;
                region->beg[0]   = (int)r.match_start;
                region->end[0]   = (int)r.match_end;
            }
            return (int)(r.match_end - r.match_start);
        }
        if (r.status == EXEC_ERROR) return ONIGERR_MEMORY;
        return ONIG_MISMATCH;
    }

    /* Tier 2: allocate capture registers. */
    int num = e->onig->num_mem + 1;  /* +1 for overall match at index 0 */
    ExecCaptures caps;
    if (alloc_caps(&caps, num) != 0)
        return ONIGERR_MEMORY;

    ExecResult r = exec_match(e->nfa, &e->cls, str, len, pos, &caps);

    int ret;
    if (r.status == EXEC_MATCH) {
        /* Overall match is always caps[0]; exec.c fills it. */
        if (region && populate_region(region, &caps) != 0) {
            ret = ONIGERR_MEMORY;
        } else {
            ret = (int)(r.match_end - r.match_start);
        }
    } else if (r.status == EXEC_ERROR) {
        ret = ONIGERR_MEMORY;
    } else {
        ret = ONIG_MISMATCH;
    }

    free_caps(&caps);
    return ret;
}

/* ── regold_match ────────────────────────────────────────────────────────── */

int
regold_match(RegoldEngine       *e,
             const OnigUChar    *str,
             const OnigUChar    *end,
             const OnigUChar    *at,
             OnigRegion         *region,
             OnigOptionType      options)
{
    /* For Tier 3, or if options change what Onigmo compiled (e.g. NOTBOL),
     * use the Onigmo native path. */
    if (e->cls.tier == REG_TIER_3 || !e->nfa) {
        return onig_match(e->onig, str, end, at, region, options);
    }

    long len = (long)(end - str);
    long pos = (long)(at  - str);

    return run_our_engine(e, str, len, pos, region);
}

/* ── regold_search ───────────────────────────────────────────────────────── */

int
regold_search(RegoldEngine       *e,
              const OnigUChar    *str,
              const OnigUChar    *end,
              const OnigUChar    *start,
              const OnigUChar    *range,
              OnigRegion         *region,
              OnigOptionType      options)
{
    /* Backward search or Tier 3: fall through to Onigmo. */
    if (e->cls.tier == REG_TIER_3 || !e->nfa || range < start) {
        return onig_search(e->onig, str, end, start, range, region, options);
    }

    long len       = (long)(end   - str);
    long start_pos = (long)(start - str);

    /* exec_search finds the leftmost match by sliding the start position.
     * It mirrors what Onigmo does in its "unanchored" search loop. */
    if (e->cls.tier == REG_TIER_1) {
        ExecResult r = exec_search(e->nfa, &e->cls, str, len, start_pos, NULL);
        if (r.status == EXEC_MATCH) {
            if (region) {
                if (onig_region_resize(region, 1) != ONIG_NORMAL)
                    return ONIGERR_MEMORY;
                region->num_regs = 1;
                region->beg[0]   = (int)r.match_start;
                region->end[0]   = (int)r.match_end;
            }
            return (int)r.match_start;
        }
        if (r.status == EXEC_ERROR) return ONIGERR_MEMORY;
        return ONIG_MISMATCH;
    }

    /* Tier 2 search: allocate captures. */
    int num = e->onig->num_mem + 1;
    ExecCaptures caps;
    if (alloc_caps(&caps, num) != 0)
        return ONIGERR_MEMORY;

    ExecResult r = exec_search(e->nfa, &e->cls, str, len, start_pos, &caps);

    int ret;
    if (r.status == EXEC_MATCH) {
        if (region && populate_region(region, &caps) != 0) {
            ret = ONIGERR_MEMORY;
        } else {
            ret = (int)r.match_start;
        }
    } else if (r.status == EXEC_ERROR) {
        ret = ONIGERR_MEMORY;
    } else {
        ret = ONIG_MISMATCH;
    }

    free_caps(&caps);
    return ret;
}
