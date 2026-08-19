/*
 * regionold/test_classify.c
 *
 * Smoke tests for the regex classifier.  Each test case compiles a pattern
 * with onig_new(), runs reg_classify(), and checks that the result matches
 * the expected tier and (where relevant) the tier3_reason code.
 *
 * Build: see Makefile (requires Ruby to be configured + built first).
 * Run:   ./test_classify
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "onigmo_compat.h"
#include "classify.h"

/* ── Test infrastructure ─────────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

static const char *
tier_name(RegTier t)
{
    switch (t) {
    case REG_TIER_1: return "TIER_1";
    case REG_TIER_2: return "TIER_2";
    case REG_TIER_3: return "TIER_3";
    default:         return "TIER_?";
    }
}

static const char *
reason_name(int r)
{
    switch (r) {
    case 0:                  return "(none)";
    case REG_T3_BACKREF:     return "BACKREF";
    case REG_T3_LOOKAHEAD:   return "LOOKAHEAD";
    case REG_T3_LOOKBEHIND:  return "LOOKBEHIND";
    case REG_T3_ABSENT:      return "ABSENT";
    case REG_T3_SUBEXP_CALL: return "SUBEXP_CALL";
    case REG_T3_CONDITION:   return "CONDITION";
    case REG_T3_STATE_CHECK: return "STATE_CHECK";
    case REG_T3_REPEAT_SG:   return "REPEAT_SG";
    case REG_T3_BYTECODE_ERR:return "BYTECODE_ERR";
    default:                 return "UNKNOWN";
    }
}

/*
 * compile_and_classify: compile 'pattern' with Onigmo (Ruby syntax, UTF-8),
 * classify the result, free the regex, and return the classification.
 * Returns a zeroed classification if compilation fails.
 */
static RegClassification
compile_and_classify(const char *pattern)
{
    OnigRegex     reg;
    OnigErrorInfo einfo;
    const UChar *pat = (const UChar *)pattern;
    int r;

    r = onig_new(&reg, pat, pat + strlen(pattern),
                 ONIG_OPTION_NONE,
                 ONIG_ENCODING_UTF8,
                 ONIG_SYNTAX_RUBY,
                 &einfo);
    if (r != ONIG_NORMAL) {
        UChar errbuf[ONIG_MAX_ERROR_MESSAGE_LEN];
        onig_error_code_to_str(errbuf, r, &einfo);
        fprintf(stderr, "  compile error for /%s/: %s\n", pattern,
                (char *)errbuf);
        RegClassification fail = {0};
        return fail;
    }

    RegClassification result = reg_classify(reg);
    onig_free(reg);
    return result;
}

static void
check(const char *pattern, RegTier expected_tier, int expected_reason)
{
    RegClassification c = compile_and_classify(pattern);
    int ok = (c.tier == expected_tier) &&
             (expected_reason == 0 || c.tier3_reason == expected_reason);

    if (ok) {
        printf("  PASS  /%s/  → %s\n", pattern, tier_name(c.tier));
        g_pass++;
    } else {
        printf("  FAIL  /%s/\n"
               "        expected %s(%s), got %s(%s)\n",
               pattern,
               tier_name(expected_tier), reason_name(expected_reason),
               tier_name(c.tier),        reason_name(c.tier3_reason));
        g_fail++;
    }
}

/* Shorthand helpers */
#define T1(pat)          check(pat, REG_TIER_1, 0)
#define T2(pat)          check(pat, REG_TIER_2, 0)
#define T3(pat, reason)  check(pat, REG_TIER_3, reason)

/* ── Test cases ──────────────────────────────────────────────────────────── */

int
main(void)
{
    onig_init();

    /* ── Tier 1: no captures, simple patterns ── */
    printf("Tier-1 patterns (DFA-eligible):\n");
    T1("foo");
    T1("^foo$");
    T1("[a-z]+");
    T1("\\d{3}-\\d{4}");
    T1("https?://\\S+");
    T1("(?:foo|bar)+");           /* non-capturing group */
    T1("\\A\\z");
    T1("\\b\\w+\\b");
    T1("(?:abc){2,5}");
    T1(".");
    T1(".*");
    T1("[^\\n]*");
    T1("(?i:foo)");               /* inline flag, no capture */
    T1("(?>atomic)");             /* atomic group, no capture */

    /* ── Tier 2: captures, no backrefs ── */
    printf("\nTier-2 patterns (tagged-NFA eligible):\n");
    T2("(foo)");
    T2("(\\d+)-(\\d+)");
    T2("(https?)://(\\S+)");
    T2("(?<year>\\d{4})-(?<month>\\d{2})-(?<day>\\d{2})");  /* named groups */
    T2("(a)(b)(c)");
    T2("(foo|bar)(baz)?");

    /* ── Tier 3: backreferences ── */
    printf("\nTier-3 patterns (backreferences):\n");
    T3("(a)\\1",          REG_T3_BACKREF);
    T3("(?<x>a)\\k<x>",  REG_T3_BACKREF);
    T3("(.)\\1",          REG_T3_BACKREF);

    /* ── Tier 3: lookahead ── */
    printf("\nTier-3 patterns (lookahead):\n");
    T3("foo(?=bar)",      REG_T3_LOOKAHEAD);
    T3("foo(?!bar)",      REG_T3_LOOKAHEAD);

    /* ── Tier 3: lookbehind ── */
    printf("\nTier-3 patterns (lookbehind):\n");
    T3("(?<=foo)bar",     REG_T3_LOOKBEHIND);
    T3("(?<!foo)bar",     REG_T3_LOOKBEHIND);

    /* ── Tier 3: subexpression calls ── */
    printf("\nTier-3 patterns (subexpression calls):\n");
    T3("(?<r>a\\g<r>?)", REG_T3_SUBEXP_CALL);  /* recursive */

    /* ── Tier 3: absent operator ── */
    printf("\nTier-3 patterns (absent operator):\n");
    T3("(?~;)",           REG_T3_ABSENT);

    /* ── Summary ── */
    printf("\n%d passed, %d failed\n", g_pass, g_fail);

    onig_end();
    return g_fail ? 1 : 0;
}
