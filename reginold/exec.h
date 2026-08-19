#ifndef REGIONOLD_EXEC_H
#define REGIONOLD_EXEC_H
/*
 * regionold/exec.h
 *
 * NFA simulation engine.
 *
 * Tier 1 (no captures): Thompson simulation with a bitset of active states.
 *   Time: O(n · m) where n = input length, m = number of NFA states.
 *   No backtracking; no catastrophic blowup.
 *
 * Tier 2 (captures): Laurikari's tagged NFA.
 *   Each active NFA "thread" carries a save-register vector.
 *   On SPLIT, the register vector is copied to the new thread.
 *   Threads at the same state are merged (keeping the leftmost-longest
 *   match semantics, matching POSIX / Ruby leftmost-longest behaviour).
 *   Time: O(n · m · k) where k = number of capture groups.
 *
 * Both paths share the same NfaGraph representation from nfa.h.
 */

#include <stddef.h>
#include "nfa.h"
#include "classify.h"

/* ── Match result ────────────────────────────────────────────────────────── */

/*
 * Byte offsets into the input string.  -1 means "not matched / not captured".
 * Index 0 is always the overall match: [match_start, match_end).
 */
typedef struct {
    long *beg; /* beg[i] = start byte of capture group i (0-based)    */
    long *end; /* end[i] = one past end byte of capture group i        */
    int   num; /* number of entries in beg/end (= reg->num_mem + 1)   */
} ExecCaptures;

/*
 * ExecResult: returned by exec_search and exec_match.
 */
typedef enum {
    EXEC_MATCH    =  0,  /* match found                                    */
    EXEC_MISMATCH = -1,  /* no match                                       */
    EXEC_ERROR    = -2,  /* internal error (allocation failure etc.)        */
} ExecStatus;

typedef struct {
    ExecStatus status;
    long       match_start; /* byte offset of match start in original str  */
    long       match_end;   /* byte offset one past match end               */
} ExecResult;

/* ── Search / match interface ────────────────────────────────────────────── */

/*
 * exec_search: find the leftmost match in [str, str+len).
 *
 * If `caps` is non-NULL, populate it with capture positions.  `caps->beg`
 * and `caps->end` must point to arrays of at least `caps->num` longs
 * (caller allocates; typically mirrors OnigRegion).
 *
 * Returns ExecResult.  On EXEC_MATCH, match_start and match_end are set
 * to byte offsets from `str`.
 */
ExecResult exec_search(const NfaGraph *g, const RegClassification *cls,
                       const unsigned char *str, long len,
                       long start_pos,
                       ExecCaptures *caps);

/*
 * exec_match: attempt a match anchored at byte position `pos`.
 *
 * Like exec_search but does not slide the start position.
 */
ExecResult exec_match(const NfaGraph *g, const RegClassification *cls,
                      const unsigned char *str, long len,
                      long pos,
                      ExecCaptures *caps);

#endif /* REGIONOLD_EXEC_H */
