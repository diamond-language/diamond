/*
 * regionold/classify.c
 *
 * Bytecode classifier for compiled Onigmo regexes.
 *
 * Strategy: walk the compiled bytecode linearly, advancing the program
 * counter past each instruction's operands.  Stop as soon as a Tier-3
 * disqualifier is found; otherwise assign Tier 1 or 2 based on whether
 * the pattern has capturing groups (reg->num_mem).
 *
 * This mirrors the structure of count_num_cache_opcodes_inner() in
 * regexec.c, but without the cache-counting logic.  The key invariant is
 * that we never read an opcode byte from a position that is actually part
 * of an operand — the per-case advances keep p aligned correctly.
 *
 * Linear scanning is sufficient because:
 *
 *   - Tier-3 opcodes (BACKREF*, PUSH_POS*, LOOK_BEHIND*, etc.) cause an
 *     immediate return, so we never need to recurse into their bodies.
 *
 *   - Block-structured opcodes like OP_PUSH_STOP_BT (atomic groups) and
 *     OP_REPEAT are followed immediately by the inner code in the bytecode
 *     stream.  Scanning past their operands and continuing linearly reaches
 *     every instruction inside them without recursion.
 *
 *   - We may visit "dead" code reachable only via jump targets we don't
 *     follow, but this is conservative-safe: false Tier-3 positives fall
 *     back to Onigmo (correct), never producing wrong answers.
 */

#include "classify.h"

/* Convenience: build a Tier-3 result with a reason code. */
static RegClassification
tier3(int reason)
{
    RegClassification r;
    r.tier           = REG_TIER_3;
    r.has_captures   = 0;
    r.is_multibyte   = 0;
    r.is_ignorecase  = 0;
    r.tier3_reason   = reason;
    return r;
}

RegClassification
reg_classify(const regex_t *reg)
{
    RegClassification result;
    const UChar *p    = reg->p;
    const UChar *pend = reg->p + reg->used;
    LengthType len;

    /*
     * Fast path: subexpression calls are recorded in reg->num_call,
     * which is set by the compiler without needing a bytecode scan.
     */
    if (reg->num_call > 0)
        return tier3(REG_T3_SUBEXP_CALL);

    while (p < pend) {
        switch ((enum OpCode)*p++) {

        /* ── Terminators ──────────────────────────────────────────────── */
        case OP_FINISH:
        case OP_END:
            goto done;

        /* ── Literal strings ──────────────────────────────────────────── */
        case OP_EXACT1:       p += 1; break;
        case OP_EXACT2:       p += 2; break;
        case OP_EXACT3:       p += 3; break;
        case OP_EXACT4:       p += 4; break;
        case OP_EXACT5:       p += 5; break;
        case OP_EXACTN:
            GET_LENGTH_INC(len, p);
            p += len;
            break;

        case OP_EXACTMB2N1:   p += 2;  break;  /* 1 × 2-byte char */
        case OP_EXACTMB2N2:   p += 4;  break;  /* 2 × 2-byte chars */
        case OP_EXACTMB2N3:   p += 6;  break;  /* 3 × 2-byte chars */
        case OP_EXACTMB2N:
            GET_LENGTH_INC(len, p);
            p += len * 2;
            break;
        case OP_EXACTMB3N:
            GET_LENGTH_INC(len, p);
            p += len * 3;
            break;
        case OP_EXACTMBN: {
            /* Two length fields: per-char byte-width, then char count. */
            LengthType mb_len;
            GET_LENGTH_INC(mb_len, p);
            GET_LENGTH_INC(len, p);
            p += mb_len * len;
            break;
        }
        case OP_EXACT1_IC:
            /* Single multibyte char; size depends on encoding. */
            p += enclen(reg->enc, p, pend);
            break;
        case OP_EXACTN_IC:
            GET_LENGTH_INC(len, p);
            p += len;
            break;

        /* ── Character classes ────────────────────────────────────────── */
        case OP_CCLASS:
        case OP_CCLASS_NOT:
            p += SIZE_BITSET;
            break;
        case OP_CCLASS_MB:
        case OP_CCLASS_MB_NOT:
            GET_LENGTH_INC(len, p);
            p += len;
            break;
        case OP_CCLASS_MIX:
        case OP_CCLASS_MIX_NOT:
            /* Bitmap followed by multibyte extension. */
            p += SIZE_BITSET;
            GET_LENGTH_INC(len, p);
            p += len;
            break;

        /* ── Wildcards ────────────────────────────────────────────────── */
        case OP_ANYCHAR:
        case OP_ANYCHAR_ML:
        case OP_ANYCHAR_STAR:
        case OP_ANYCHAR_ML_STAR:
            break;
        case OP_ANYCHAR_STAR_PEEK_NEXT:
        case OP_ANYCHAR_ML_STAR_PEEK_NEXT:
            p += 1;   /* peek byte */
            break;

        /* ── Word / character-type checks (no operands) ───────────────── */
        case OP_WORD:
        case OP_NOT_WORD:
        case OP_WORD_BOUND:
        case OP_NOT_WORD_BOUND:
        case OP_WORD_BEGIN:
        case OP_WORD_END:
        case OP_ASCII_WORD:
        case OP_NOT_ASCII_WORD:
        case OP_ASCII_WORD_BOUND:
        case OP_NOT_ASCII_WORD_BOUND:
        case OP_ASCII_WORD_BEGIN:
        case OP_ASCII_WORD_END:
            break;

        /* ── Anchors (no operands) ────────────────────────────────────── */
        case OP_BEGIN_BUF:
        case OP_END_BUF:
        case OP_BEGIN_LINE:
        case OP_END_LINE:
        case OP_SEMI_END_BUF:
        case OP_BEGIN_POSITION:
            break;

        /* ── \K — reset match start.  Kept in Tier 1/2 for now. ──────── */
        case OP_KEEP:
            break;

        /* ── Tier-3: backreferences ───────────────────────────────────── */
        case OP_BACKREF1:
        case OP_BACKREF2:
        case OP_BACKREFN:
        case OP_BACKREFN_IC:
        case OP_BACKREF_MULTI:
        case OP_BACKREF_MULTI_IC:
        case OP_BACKREF_WITH_LEVEL:
            return tier3(REG_T3_BACKREF);

        /* ── Captures — informational, not a disqualifier ────────────── */
        case OP_MEMORY_START:
        case OP_MEMORY_START_PUSH:
        case OP_MEMORY_END_PUSH:
        case OP_MEMORY_END_PUSH_REC:
        case OP_MEMORY_END:
        case OP_MEMORY_END_REC:
            p += SIZE_MEMNUM;
            break;

        /* ── Control flow ────────────────────────────────────────────── */
        case OP_FAIL:
            break;
        case OP_JUMP:
            p += SIZE_RELADDR;
            break;
        case OP_PUSH:
            p += SIZE_RELADDR;
            break;
        case OP_POP:
            break;
        case OP_PUSH_OR_JUMP_EXACT1:
        case OP_PUSH_IF_PEEK_NEXT:
            p += SIZE_RELADDR + 1;
            break;

        /* ── Quantifiers ─────────────────────────────────────────────── */
        case OP_REPEAT:
        case OP_REPEAT_NG:
            /* [memnum][reladdr] then inner body then OP_REPEAT_INC */
            p += SIZE_MEMNUM + SIZE_RELADDR;
            break;
        case OP_REPEAT_INC:
        case OP_REPEAT_INC_NG:
            p += SIZE_MEMNUM;
            break;
        case OP_REPEAT_INC_SG:
        case OP_REPEAT_INC_NG_SG:
            /* "search and get" stack variants — not handled by our engine. */
            return tier3(REG_T3_REPEAT_SG);

        /* ── Null-loop guards ─────────────────────────────────────────── */
        case OP_NULL_CHECK_START:
        case OP_NULL_CHECK_END:
        case OP_NULL_CHECK_END_MEMST:
        case OP_NULL_CHECK_END_MEMST_PUSH:
            p += SIZE_MEMNUM;
            break;

        /* ── Tier-3: lookahead / lookbehind ──────────────────────────── */
        case OP_PUSH_POS:
        case OP_PUSH_POS_NOT:
            return tier3(REG_T3_LOOKAHEAD);
        case OP_POP_POS:
        case OP_FAIL_POS:
            /*
             * These are the terminating opcodes for lookahead blocks.
             * If we reach them without a preceding PUSH_POS, the bytecode
             * has a structure we don't understand — treat as Tier 3.
             */
            return tier3(REG_T3_LOOKAHEAD);
        case OP_LOOK_BEHIND:
        case OP_PUSH_LOOK_BEHIND_NOT:
            return tier3(REG_T3_LOOKBEHIND);
        case OP_FAIL_LOOK_BEHIND_NOT:
            return tier3(REG_T3_LOOKBEHIND);

        /* ── Atomic groups — fine for Tier 1/2 ──────────────────────── */
        case OP_PUSH_STOP_BT:
        case OP_POP_STOP_BT:
            break;

        /* ── Tier-3: absent operator ─────────────────────────────────── */
        case OP_PUSH_ABSENT_POS:
        case OP_ABSENT:
        case OP_ABSENT_END:
            return tier3(REG_T3_ABSENT);

        /* ── Tier-3: subexpression calls ─────────────────────────────── */
        case OP_CALL:
        case OP_RETURN:
            return tier3(REG_T3_SUBEXP_CALL);

        /* ── Tier-3: conditional ─────────────────────────────────────── */
        case OP_CONDITION:
            return tier3(REG_T3_CONDITION);

        /* ── Tier-3: combination-explosion state checks ──────────────── */
        case OP_STATE_CHECK_PUSH:
        case OP_STATE_CHECK_PUSH_OR_JUMP:
        case OP_STATE_CHECK:
        case OP_STATE_CHECK_ANYCHAR_STAR:
        case OP_STATE_CHECK_ANYCHAR_ML_STAR:
            return tier3(REG_T3_STATE_CHECK);

        /* ── Option-setting (inline flag changes) ────────────────────── */
        case OP_SET_OPTION_PUSH:
        case OP_SET_OPTION:
            p += SIZE_OPTION;
            break;

        default:
            /*
             * Unknown opcode.  The bytecode is from a version of Onigmo we
             * don't fully understand — fall back conservatively.
             */
            return tier3(REG_T3_BYTECODE_ERR);
        }
    }

done:
    result.has_captures  = (reg->num_mem > 0);
    result.is_multibyte  = (ONIGENC_MBC_MAXLEN(reg->enc) > 1);
    result.is_ignorecase = ((reg->options & ONIG_OPTION_IGNORECASE) != 0);
    result.tier3_reason  = 0;
    result.tier          = result.has_captures ? REG_TIER_2 : REG_TIER_1;
    return result;
}
