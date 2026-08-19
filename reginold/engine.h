#ifndef REGIONOLD_ENGINE_H
#define REGIONOLD_ENGINE_H
/*
 * regionold/engine.h
 *
 * Integration layer: Onigmo-compatible shim that routes patterns to the
 * appropriate execution tier.
 *
 * Tier 1 (no captures):  Thompson bitset NFA — O(n·m), no backtracking.
 * Tier 2 (captures):     Laurikari tagged NFA — O(n·m·k), no backtracking.
 * Tier 3 (everything else): Onigmo native fallback — full feature set.
 *
 * Usage from Ruby's re.c:
 *   Replace onig_new / onig_search / onig_match / onig_free with the
 *   regold_* equivalents.  The API surface mirrors Onigmo's exactly so
 *   that the call sites need only a mechanical substitution.
 */

#include "onigmo_compat.h"  /* regex_t, OnigRegion, OnigEncoding … */
#include "classify.h"
#include "nfa.h"
#include "exec.h"

/* ── Compiled engine handle ──────────────────────────────────────────────── */

typedef struct {
    regex_t          *onig;   /* always valid — owns the compiled Onigmo regex  */
    RegClassification cls;    /* tier + feature flags, computed at compile time  */
    NfaGraph         *nfa;    /* non-NULL for Tier 1/2; NULL for Tier 3          */
} RegoldEngine;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/*
 * regold_new: compile a pattern and build the NFA where possible.
 *
 * Parameters mirror onig_new().  On success, *out is set to a newly
 * allocated RegoldEngine and ONIG_NORMAL is returned.  On Onigmo compile
 * error the error code and einfo are forwarded as-is.
 */
int regold_new(RegoldEngine       **out,
               const OnigUChar     *pattern,
               const OnigUChar     *pattern_end,
               OnigOptionType       options,
               OnigEncoding         enc,
               const OnigSyntaxType *syntax,
               OnigErrorInfo        *einfo);

/*
 * regold_free: release all resources owned by the engine.
 */
void regold_free(RegoldEngine *e);

/* ── Search / match ──────────────────────────────────────────────────────── */

/*
 * regold_search: find the leftmost match in [str, end), beginning the
 * search at `start` and ending at `range`.
 *
 * Mirrors onig_search() exactly:
 *   - Returns the byte offset of the match start (>= 0) on success.
 *   - Returns ONIG_MISMATCH (-1) if no match exists.
 *   - Returns a negative error code on internal failure.
 *
 * If `region` is non-NULL it is populated with capture positions.
 * If `range < start` (backward search), routing falls through to Onigmo
 * for all tiers because our simulation is forward-only.
 */
int regold_search(RegoldEngine       *e,
                  const OnigUChar    *str,
                  const OnigUChar    *end,
                  const OnigUChar    *start,
                  const OnigUChar    *range,
                  OnigRegion         *region,
                  OnigOptionType      options);

/*
 * regold_match: attempt a match anchored at byte position `at`.
 *
 * Mirrors onig_match():
 *   - Returns the match length (>= 0) on success.
 *   - Returns ONIG_MISMATCH (-1) if the pattern does not match at `at`.
 *   - Returns a negative error code on internal failure.
 */
int regold_match(RegoldEngine       *e,
                 const OnigUChar    *str,
                 const OnigUChar    *end,
                 const OnigUChar    *at,
                 OnigRegion         *region,
                 OnigOptionType      options);

#endif /* REGIONOLD_ENGINE_H */
