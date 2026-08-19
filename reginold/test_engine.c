/*
 * regionold/test_engine.c
 *
 * End-to-end tests for the RegoldEngine integration layer.
 *
 * Each test compiles a pattern via regold_new(), runs regold_search() or
 * regold_match(), and verifies the returned byte offsets.  Tier 3 patterns
 * are exercised too — they fall back to Onigmo and must still return the
 * correct answer.
 *
 * Build: make test_engine  (see Makefile)
 * Run:   ./test_engine
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "onigmo_compat.h"
#include "engine.h"

/* ── Test infrastructure ─────────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define PASS(fmt, ...) do { printf("  PASS  " fmt "\n", ##__VA_ARGS__); g_pass++; } while(0)
#define FAIL(fmt, ...) do { printf("  FAIL  " fmt "\n", ##__VA_ARGS__); g_fail++; } while(0)

static RegoldEngine *
compile(const char *pattern)
{
    RegoldEngine *e = NULL;
    OnigErrorInfo einfo;
    const UChar *p = (const UChar *)pattern;
    int r = regold_new(&e, p, p + strlen(pattern),
                       ONIG_OPTION_NONE,
                       ONIG_ENCODING_UTF8,
                       ONIG_SYNTAX_RUBY,
                       &einfo);
    if (r != ONIG_NORMAL) {
        UChar buf[ONIG_MAX_ERROR_MESSAGE_LEN];
        onig_error_code_to_str(buf, r, &einfo);
        fprintf(stderr, "  compile error for /%s/: %s\n", pattern, buf);
        return NULL;
    }
    return e;
}

/*
 * check_search: search for `pattern` in `input`, expect match at [beg, end).
 * Pass beg==-1 to expect ONIG_MISMATCH.
 */
static void
check_search(const char *label,
             const char *pattern, const char *input,
             int exp_beg, int exp_end)
{
    (void)label;
    RegoldEngine *e = compile(pattern);
    if (!e) { g_fail++; return; }

    const UChar *str   = (const UChar *)input;
    const UChar *end   = str + strlen(input);
    OnigRegion  *region = onig_region_new();

    int r = regold_search(e, str, end, str, end, region, ONIG_OPTION_NONE);

    if (exp_beg == -1) {
        if (r == ONIG_MISMATCH) {
            PASS("/%s/ in \"%s\" → no match", pattern, input);
        } else {
            FAIL("/%s/ in \"%s\" → expected no match, got r=%d [%d,%d)",
                 pattern, input, r,
                 r >= 0 ? region->beg[0] : -1,
                 r >= 0 ? region->end[0] : -1);
        }
    } else {
        if (r == exp_beg &&
            region->num_regs >= 1 &&
            region->beg[0] == exp_beg &&
            region->end[0] == exp_end) {
            PASS("/%s/ in \"%s\" → [%d,%d)", pattern, input, exp_beg, exp_end);
        } else {
            FAIL("/%s/ in \"%s\"\n"
                 "        expected [%d,%d), got r=%d region=[%d,%d)",
                 pattern, input, exp_beg, exp_end, r,
                 (region->num_regs >= 1 ? region->beg[0] : -1),
                 (region->num_regs >= 1 ? region->end[0] : -1));
        }
    }

    onig_region_free(region, 1);
    regold_free(e);
}

/*
 * check_match: anchored match of `pattern` at byte `pos` in `input`.
 * exp_len >= 0 for expected match length; -1 for MISMATCH.
 */
static void
check_match(const char *label,
            const char *pattern, const char *input, int pos,
            int exp_len)
{
    (void)label;
    RegoldEngine *e = compile(pattern);
    if (!e) { g_fail++; return; }

    const UChar *str = (const UChar *)input;
    const UChar *end = str + strlen(input);
    const UChar *at  = str + pos;
    OnigRegion  *region = onig_region_new();

    int r = regold_match(e, str, end, at, region, ONIG_OPTION_NONE);

    if (exp_len == -1) {
        if (r == ONIG_MISMATCH) {
            PASS("match /%s/ @%d in \"%s\" → no match", pattern, pos, input);
        } else {
            FAIL("match /%s/ @%d in \"%s\" → expected no match, got len=%d",
                 pattern, pos, input, r);
        }
    } else {
        if (r == exp_len) {
            PASS("match /%s/ @%d in \"%s\" → len=%d", pattern, pos, input, r);
        } else {
            FAIL("match /%s/ @%d in \"%s\" → expected len=%d, got %d",
                 pattern, pos, input, exp_len, r);
        }
    }

    onig_region_free(region, 1);
    regold_free(e);
}

/*
 * check_capture: search for `pattern`, verify capture group `grp` is [beg,end).
 */
static void
check_capture(const char *label,
              const char *pattern, const char *input,
              int grp, int exp_beg, int exp_end)
{
    (void)label;
    RegoldEngine *e = compile(pattern);
    if (!e) { g_fail++; return; }

    const UChar *str    = (const UChar *)input;
    const UChar *strend = str + strlen(input);
    OnigRegion  *region = onig_region_new();

    int r = regold_search(e, str, strend, str, strend, region, ONIG_OPTION_NONE);

    if (r < 0) {
        FAIL("/%s/ in \"%s\" → no match (expected capture[%d]=[%d,%d))",
             pattern, input, grp, exp_beg, exp_end);
    } else if (region->num_regs <= grp) {
        FAIL("/%s/ in \"%s\" → region has %d groups, want group %d",
             pattern, input, region->num_regs, grp);
    } else if (region->beg[grp] == exp_beg && region->end[grp] == exp_end) {
        PASS("/%s/ in \"%s\" capture[%d] → [%d,%d)",
             pattern, input, grp, exp_beg, exp_end);
    } else {
        FAIL("/%s/ in \"%s\" capture[%d] → expected [%d,%d), got [%d,%d)",
             pattern, input, grp, exp_beg, exp_end,
             region->beg[grp], region->end[grp]);
    }

    onig_region_free(region, 1);
    regold_free(e);
}

/* ── Test suites ─────────────────────────────────────────────────────────── */

static void
test_tier1_search(void)
{
    printf("Tier-1 search (no captures):\n");

    check_search("literal",       "foo",        "hello foo world", 6,  9);
    check_search("anchored-beg",  "^foo",       "foobar",          0,  3);
    check_search("anchored-end",  "bar$",       "foobar",          3,  6);
    check_search("dot-star",      "f.*r",       "foobar",          0,  6);
    check_search("charclass",     "[0-9]+",     "abc123def",       3,  6);
    check_search("alternation",   "cat|dog",    "I have a dog",    9, 12);
    check_search("quantifier-?",  "colou?r",    "color",           0,  5);
    check_search("quantifier-+",  "[a-z]+",     "   hello   ",     3,  8);
    check_search("word-boundary", "\\bword\\b", "a word here",     2,  6);
    check_search("no-match",      "xyz",        "hello",          -1, -1);
    check_search("empty-str",     "a*",         "bbb",             0,  0);   /* zero-width at pos 0 */
    check_search("non-capturing", "(?:ab)+",    "ababc",           0,  4);
}

static void
test_tier1_match(void)
{
    printf("\nTier-1 anchored match:\n");

    check_match("exact",      "foo",    "foobar", 0,  3);
    check_match("offset",     "bar",    "foobar", 3,  3);
    check_match("mismatch",   "foo",    "foobar", 1, -1);
    check_match("star",       "[a-z]*", "foobar", 0,  6);
    check_match("empty",      "x*",     "abc",    0,  0);
}

static void
test_tier2_search(void)
{
    printf("\nTier-2 search (captures):\n");

    /* Overall match position via region->beg[0]/end[0] */
    check_search("simple-cap",   "(foo)",       "hello foo", 6, 9);
    check_search("multi-cap",    "(\\d+)-(\\d+)", "id:42-99", 3, 8);

    /* Capture group contents */
    check_capture("cap[0]",      "(foo)",          "hello foo", 0, 6, 9);
    check_capture("cap[1]",      "(foo)",          "hello foo", 1, 6, 9);
    check_capture("cap[1]-dig",  "(\\d+)-(\\d+)",  "id:42-99", 1, 3, 5);
    check_capture("cap[2]-dig",  "(\\d+)-(\\d+)",  "id:42-99", 2, 6, 8);
    check_capture("optional-hit","(foo)?(bar)",    "foobar",   1, 0, 3);
    check_capture("optional-mis","(foo)?(bar)",    "bar",      1,-1,-1);  /* unmatched group */
    check_capture("named-grp",   "(?<y>\\d{4})-(?<m>\\d{2})", "date:2024-07", 1, 5, 9);
    check_capture("named-grp2",  "(?<y>\\d{4})-(?<m>\\d{2})", "date:2024-07", 2, 10, 12);
}

static void
test_tier3_fallback(void)
{
    printf("\nTier-3 patterns (Onigmo fallback):\n");

    /* Backref */
    check_search("backref",    "(.)\\1",         "aabbcc", 0, 2);
    check_search("backref-nm", "(?<c>.)\\k<c>",  "xxyy",   0, 2);

    /* Lookahead */
    check_search("lookahead",  "foo(?=bar)",      "foobar", 0, 3);
    check_search("neg-look",   "foo(?!baz)",      "foobar", 0, 3);

    /* Lookbehind */
    check_search("lookbehind", "(?<=foo)bar",     "foobar", 3, 6);

    /* No match cases still work */
    check_search("backref-nm2","(.)\\1",          "abcd",  -1,-1);
}

static void
test_multibyte(void)
{
    printf("\nMultibyte (UTF-8):\n");

    /*
     * "café" = 63 61 66 c3 a9  (5 bytes, 4 codepoints)
     *
     * Known limitation: NFA_ANY is byte-level.  In the simulation each step
     * consumes exactly one byte, so NFA_ANY matches only the *first* byte of
     * a multi-byte character.  Patterns relying on `.` spanning a full UTF-8
     * codepoint (e.g. /caf./) produce a byte-level result.  Patterns that
     * spell out multi-byte literals byte-by-byte work correctly.
     */

    /* ASCII prefix in a multi-byte string — fine, byte-level is identical. */
    check_search("mb-literal", "caf",   "café", 0, 3);

    /* Multi-byte literal "é" encoded as two exact bytes 0xc3 0xa9. */
    check_search("mb-noanchor","é",     "café", 3, 5);

    /* Multibyte patterns fall back to Onigmo, so . correctly spans the full
     * two-byte é codepoint. */
    check_search("mb-any-onig","caf.",  "café", 0, 5);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int
main(void)
{
    onig_init();

    test_tier1_search();
    test_tier1_match();
    test_tier2_search();
    test_tier3_fallback();
    test_multibyte();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);

    onig_end();
    return g_fail ? 1 : 0;
}
