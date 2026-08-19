#ifndef REGIONOLD_CLASSIFY_H
#define REGIONOLD_CLASSIFY_H
/*
 * regionold/classify.h
 *
 * Post-compilation regex classifier: reads a compiled OnigRegexType and
 * assigns it to one of three execution tiers.
 *
 * Build note: compile with -I$(RUBY_SRC) (set via ONIGMO_CFLAGS in the Makefile).
 */

#include "onigmo_compat.h"

/*
 * REG_TIER_1 — DFA-eligible.
 *   No capturing groups, no backreferences, no lookahead/lookbehind,
 *   no subexpression calls, no conditional or absent constructs.
 *   Guaranteed linear time via NFA→DFA (or direct NFA simulation without
 *   capture tracking).
 *
 * REG_TIER_2 — Tagged-NFA eligible.
 *   Has capturing groups, but none of the Tier-3 disqualifiers.
 *   Laurikari's tagged NFA gives linear-time simulation with capture
 *   positions. No catastrophic backtracking.
 *
 * REG_TIER_3 — Onigmo fallback.
 *   Backreferences, lookahead/lookbehind, subexpression calls, conditional
 *   patterns, or the absent operator. Must use the full Onigmo engine.
 */
typedef enum {
    REG_TIER_1 = 1,
    REG_TIER_2 = 2,
    REG_TIER_3 = 3,
} RegTier;

typedef struct {
    RegTier tier;

    /* Mirrors reg->num_mem > 0 — convenient for callers. */
    int has_captures;

    /*
     * True when the encoding is multibyte (max_enc_len > 1).
     * The NFA/DFA compiler needs this to emit correct character-step logic.
     */
    int is_multibyte;

    /*
     * True when ONIG_OPTION_IGNORECASE is set.
     * The NFA compiler emits case-folded transitions when this is set.
     */
    int is_ignorecase;

    /*
     * Reason code explaining why a Tier-3 result was returned.
     * Zero for Tier 1 and 2.  One of the REG_T3_* constants below.
     */
    int tier3_reason;
} RegClassification;

/* tier3_reason values */
#define REG_T3_BACKREF       1  /* \1, \k<name>, \k<name+n>, etc. */
#define REG_T3_LOOKAHEAD     2  /* (?=...) or (?!...) */
#define REG_T3_LOOKBEHIND    3  /* (?<=...) or (?<!...) */
#define REG_T3_ABSENT        4  /* (?~...) */
#define REG_T3_SUBEXP_CALL   5  /* \g<name> */
#define REG_T3_CONDITION     6  /* (?(cond)yes|no) */
#define REG_T3_STATE_CHECK   7  /* combination-explosion guard opcodes */
#define REG_T3_REPEAT_SG     8  /* OP_REPEAT_INC_SG / _NG_SG */
#define REG_T3_BYTECODE_ERR  9  /* unrecognised opcode (should not happen) */

/*
 * Classify a successfully compiled regex.
 * Must be called after onig_new() (or equivalent) has returned ONIG_NORMAL.
 * The regex_t is not modified.
 */
RegClassification reg_classify(const regex_t *reg);

#endif /* REGIONOLD_CLASSIFY_H */
