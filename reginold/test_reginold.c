/*
 * test_reginold.c
 *
 * Public API tests for reginold.
 *
 * This file includes ONLY reginold.h — no Onigmo, no regint.h, no Ruby headers.
 * That constraint is the definition-of-done for the first milestone.
 *
 * Build: make test_reginold
 * Run:   ./test_reginold
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reginold.h"

/* ── Test infrastructure ─────────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define PASS(fmt, ...) do { printf("  PASS  " fmt "\n", ##__VA_ARGS__); g_pass++; } while(0)
#define FAIL(fmt, ...) do { printf("  FAIL  " fmt "\n", ##__VA_ARGS__); g_fail++; } while(0)

static reginold_regex *
compile_ok(const char *pattern)
{
    reginold_regex *re = NULL;
    reginold_error err = {0};
    reginold_status s = reginold_compile(pattern, strlen(pattern),
                                         REGINOLD_OPTION_NONE, &re, &err);
    if (s != REGINOLD_OK) {
        fprintf(stderr, "  compile error /%s/: %s\n", pattern, err.message);
        g_fail++;
        return NULL;
    }
    return re;
}

/*
 * check_search: search for pattern in input, expect overall match at [beg,end).
 * Pass beg == -1 to expect REGINOLD_MISMATCH.
 */
static void
check_search(const char *pattern, const char *input,
             long exp_beg, long exp_end)
{
    reginold_regex *re = compile_ok(pattern);
    if (!re) return;

    reginold_match m = {0};
    reginold_status s = reginold_search(re, input, strlen(input), 0, &m);

    if (exp_beg == -1) {
        if (s == REGINOLD_MISMATCH) {
            PASS("/%s/ in \"%s\" → no match", pattern, input);
        } else {
            FAIL("/%s/ in \"%s\" → expected no match, got status=%d [%ld,%ld)",
                 pattern, input, s, m.overall.beg, m.overall.end);
            reginold_match_free(&m);
        }
    } else {
        if (s == REGINOLD_OK &&
            m.overall.beg == exp_beg && m.overall.end == exp_end) {
            PASS("/%s/ in \"%s\" → [%ld,%ld)", pattern, input, exp_beg, exp_end);
        } else {
            FAIL("/%s/ in \"%s\" → expected [%ld,%ld), got status=%d [%ld,%ld)",
                 pattern, input, exp_beg, exp_end, s,
                 m.overall.beg, m.overall.end);
        }
        reginold_match_free(&m);
    }

    reginold_regex_free(re);
}

/*
 * check_match_at: anchored match of pattern at byte offset pos in input.
 * exp_end >= 0 for expected end offset; -1 to expect REGINOLD_MISMATCH.
 */
static void
check_match_at(const char *pattern, const char *input, size_t pos,
               long exp_end)
{
    reginold_regex *re = compile_ok(pattern);
    if (!re) return;

    reginold_match m = {0};
    reginold_status s = reginold_match_at(re, input, strlen(input), pos, &m);

    if (exp_end == -1) {
        if (s == REGINOLD_MISMATCH) {
            PASS("match /%s/ @%zu in \"%s\" → no match", pattern, pos, input);
        } else {
            FAIL("match /%s/ @%zu in \"%s\" → expected no match, got [%ld,%ld)",
                 pattern, pos, input, m.overall.beg, m.overall.end);
            reginold_match_free(&m);
        }
    } else {
        if (s == REGINOLD_OK &&
            m.overall.beg == (long)pos && m.overall.end == exp_end) {
            PASS("match /%s/ @%zu in \"%s\" → [%zu,%ld)",
                 pattern, pos, input, pos, exp_end);
        } else {
            FAIL("match /%s/ @%zu in \"%s\" → expected [%zu,%ld), got status=%d [%ld,%ld)",
                 pattern, pos, input, pos, exp_end, s,
                 m.overall.beg, m.overall.end);
        }
        reginold_match_free(&m);
    }

    reginold_regex_free(re);
}

/*
 * check_capture: search for pattern in input, verify capture group `grp`
 * (1-based) has span [exp_beg, exp_end). Pass exp_beg == -1 for unmatched.
 */
static void
check_capture(const char *pattern, const char *input,
              size_t grp, long exp_beg, long exp_end)
{
    reginold_regex *re = compile_ok(pattern);
    if (!re) return;

    reginold_match m = {0};
    reginold_status s = reginold_search(re, input, strlen(input), 0, &m);

    if (s != REGINOLD_OK) {
        FAIL("/%s/ in \"%s\" → no match (expected capture[%zu]=[%ld,%ld))",
             pattern, input, grp, exp_beg, exp_end);
        reginold_regex_free(re);
        return;
    }

    size_t idx = grp - 1;  /* grp is 1-based; captures[] is 0-based */

    if (m.capture_count < grp) {
        FAIL("/%s/ in \"%s\" → only %zu captures, want group %zu",
             pattern, input, m.capture_count, grp);
    } else if (m.captures[idx].beg == exp_beg &&
               m.captures[idx].end == exp_end) {
        PASS("/%s/ in \"%s\" capture[%zu] → [%ld,%ld)",
             pattern, input, grp, exp_beg, exp_end);
    } else {
        FAIL("/%s/ in \"%s\" capture[%zu] → expected [%ld,%ld), got [%ld,%ld)",
             pattern, input, grp, exp_beg, exp_end,
             m.captures[idx].beg, m.captures[idx].end);
    }

    reginold_match_free(&m);
    reginold_regex_free(re);
}

/* ── Test suites ─────────────────────────────────────────────────────────── */

static void
test_compile(void)
{
    printf("Compile:\n");

    /* success */
    {
        reginold_regex *re = NULL;
        reginold_error  err = {0};
        reginold_status s = reginold_compile("foo", 3, REGINOLD_OPTION_NONE,
                                              &re, &err);
        if (s == REGINOLD_OK && re != NULL) {
            PASS("compile /foo/ → ok");
            reginold_regex_free(re);
        } else {
            FAIL("compile /foo/ → expected ok, got error: %s", err.message);
        }
    }

    /* failure: unmatched paren */
    {
        reginold_regex *re  = NULL;
        reginold_error  err = {0};
        reginold_status s = reginold_compile("(foo", 4, REGINOLD_OPTION_NONE,
                                              &re, &err);
        if (s == REGINOLD_ERROR && re == NULL && err.message_len > 0) {
            PASS("compile /(foo/ → REGINOLD_ERROR: \"%s\" at offset %zu",
                 err.message, err.error_offset);
        } else {
            FAIL("compile /(foo/ → expected REGINOLD_ERROR with message");
        }
    }

    /* failure with NULL err: must not crash */
    {
        reginold_regex *re = NULL;
        reginold_status s = reginold_compile("(foo", 4, REGINOLD_OPTION_NONE,
                                              &re, NULL);
        if (s == REGINOLD_ERROR && re == NULL) {
            PASS("compile /(foo/ with NULL err → no crash");
        } else {
            FAIL("compile /(foo/ with NULL err → unexpected result");
        }
    }
}

static void
test_search_no_captures(void)
{
    printf("\nSearch — no captures (Tier 1):\n");

    check_search("foo",        "hello foo world",  6,  9);
    check_search("^foo",       "foobar",            0,  3);
    check_search("bar$",       "foobar",            3,  6);
    check_search("[0-9]+",     "abc123def",         3,  6);
    check_search("cat|dog",    "I have a dog",      9, 12);
    check_search("colou?r",    "color",             0,  5);
    check_search("(?:ab)+",    "ababc",             0,  4);
    check_search("\\bword\\b", "a word here",       2,  6);
    check_search("xyz",        "hello",            -1, -1);
}

static void
test_search_start_offset(void)
{
    printf("\nSearch — start offset:\n");

    reginold_regex *re = compile_ok("foo");
    if (!re) return;

    /* from offset 0 → first occurrence at [0,3) */
    {
        reginold_match  m = {0};
        reginold_status s = reginold_search(re, "foofoo", 6, 0, &m);
        if (s == REGINOLD_OK && m.overall.beg == 0 && m.overall.end == 3) {
            PASS("/foo/ from start=0 → [0,3)");
        } else {
            FAIL("/foo/ from start=0 → expected [0,3), got status=%d [%ld,%ld)",
                 s, m.overall.beg, m.overall.end);
        }
        reginold_match_free(&m);
    }

    /* from offset 1 → skip first, find second at [3,6) */
    {
        reginold_match  m = {0};
        reginold_status s = reginold_search(re, "foofoo", 6, 1, &m);
        if (s == REGINOLD_OK && m.overall.beg == 3 && m.overall.end == 6) {
            PASS("/foo/ from start=1 → [3,6)");
        } else {
            FAIL("/foo/ from start=1 → expected [3,6), got status=%d [%ld,%ld)",
                 s, m.overall.beg, m.overall.end);
        }
        reginold_match_free(&m);
    }

    reginold_regex_free(re);
}

static void
test_match_at(void)
{
    printf("\nAnchored match:\n");

    check_match_at("foo",    "foobar", 0,  3);
    check_match_at("bar",    "foobar", 3,  6);
    check_match_at("foo",    "foobar", 1, -1);  /* wrong position */
    check_match_at("[a-z]*", "foobar", 0,  6);
    check_match_at("x*",     "abc",    0,  0);  /* zero-width */
}

static void
test_captures(void)
{
    printf("\nCaptures (Tier 2):\n");

    /* overall match */
    check_search("(foo)",          "hello foo",  6, 9);
    check_search("(\\d+)-(\\d+)",  "id:42-99",   3, 8);

    /* group contents */
    check_capture("(foo)",             "hello foo",  1,  6,  9);
    check_capture("(\\d+)-(\\d+)",     "id:42-99",   1,  3,  5);
    check_capture("(\\d+)-(\\d+)",     "id:42-99",   2,  6,  8);

    /* named groups — accessible as positional captures */
    check_capture("(?<y>\\d{4})-(?<m>\\d{2})", "date:2024-07", 1,  5,  9);
    check_capture("(?<y>\\d{4})-(?<m>\\d{2})", "date:2024-07", 2, 10, 12);
}

static void
test_optional_captures(void)
{
    printf("\nOptional captures:\n");

    /* optional group present */
    check_capture("(foo)?(bar)", "foobar", 1,  0,  3);

    /* optional group absent — expect {-1,-1} */
    check_capture("(foo)?(bar)", "bar",    1, -1, -1);
}

static void
test_fallback(void)
{
    printf("\nFallback patterns (Tier 3):\n");

    check_search("(.)\\1",       "aabbcc",  0, 2);  /* backref */
    check_search("foo(?=bar)",   "foobar",  0, 3);  /* lookahead */
    check_search("(?<=foo)bar",  "foobar",  3, 6);  /* lookbehind */
    check_search("(.)\\1",       "abcd",   -1,-1);  /* backref, no match */
}

static void
test_options(void)
{
    printf("\nOption flags:\n");

    /* IGNORECASE */
    {
        reginold_regex *re  = NULL;
        reginold_error  err = {0};
        reginold_status s = reginold_compile("foo", 3, REGINOLD_OPTION_IGNORECASE,
                                              &re, &err);
        if (s != REGINOLD_OK) {
            FAIL("compile /foo/i → error: %s", err.message);
        } else {
            reginold_match m = {0};
            s = reginold_search(re, "FOO", 3, 0, &m);
            if (s == REGINOLD_OK && m.overall.beg == 0 && m.overall.end == 3) {
                PASS("/foo/i matches \"FOO\" → [0,3)");
            } else {
                FAIL("/foo/i in \"FOO\" → expected [0,3), got status=%d", s);
            }
            reginold_match_free(&m);
            reginold_regex_free(re);
        }
    }
}

static void
test_null_out(void)
{
    printf("\nNULL out (match-check only):\n");

    reginold_regex *re = compile_ok("foo");
    if (!re) return;

    reginold_status s = reginold_search(re, "hello foo", 9, 0, NULL);
    if (s == REGINOLD_OK) {
        PASS("/foo/ match-check hit → REGINOLD_OK");
    } else {
        FAIL("/foo/ match-check hit → expected REGINOLD_OK, got %d", s);
    }

    s = reginold_search(re, "hello bar", 9, 0, NULL);
    if (s == REGINOLD_MISMATCH) {
        PASS("/foo/ match-check miss → REGINOLD_MISMATCH");
    } else {
        FAIL("/foo/ match-check miss → expected REGINOLD_MISMATCH, got %d", s);
    }

    reginold_regex_free(re);
}

/* Exercise both sides of the small-region boundary, including Onigmo's
 * backreference fallback and byte offsets in multibyte subjects. Retained
 * captures must remain owned by the caller across later matches. */
static void
test_region_storage(void)
{
    static const struct {
        const char *pattern;
        const char *subject;
        size_t captures;
    } cases[] = {
        {"(a)(b)(c)", "abc", 3},
        {"(a)(b)(c)(d)", "abcd", 4},
        {"(a)(b)(c)(d)(e)(f)(g)(h)", "abcdefgh", 8},
        {"(a)(b)(c)\\3", "abcc", 3},
        {"(a)(b)(c)(d)\\4", "abcdd", 4},
        {"(é)(β)(🙂)", "éβ🙂", 3},
        {"(a)(b)?(c)", "ac", 3},
    };
    printf("\nTemporary region storage:\n");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reginold_regex *re = compile_ok(cases[i].pattern);
        if (!re) continue;
        for (int anchored = 0; anchored < 2; anchored++) {
            reginold_status (*run)(const reginold_regex *, const char *,
                size_t, size_t, reginold_match *) = anchored ?
                reginold_match_at : reginold_search;
            char padded[64];
            snprintf(padded, sizeof padded, "_%s", cases[i].subject);
            size_t len = strlen(cases[i].subject);
            reginold_match first = {0}, second = {0};
            int ok = run(re, padded, len + 1, 1, &first) == REGINOLD_OK;
            ok = ok && run(re, cases[i].subject, len, 0, NULL) == REGINOLD_OK;
            ok = ok && run(re, "z", 1, 0, NULL) == REGINOLD_MISMATCH;
            ok = ok && run(re, cases[i].subject, len, 0, &second) == REGINOLD_OK;
            ok = ok && first.capture_count == cases[i].captures &&
                second.capture_count == cases[i].captures &&
                first.overall.beg == 1 && first.overall.end == (long)len + 1 &&
                second.overall.beg == 0 && second.overall.end == (long)len &&
                first.captures[0].beg == 1 && second.captures[0].beg == 0;
            if (ok)
                PASS("/%s/ %s retains captures across later calls",
                     cases[i].pattern, anchored ? "match_at" : "search");
            else
                FAIL("/%s/ %s temporary region handling",
                     cases[i].pattern, anchored ? "match_at" : "search");
            reginold_match_free(&first);
            reginold_match_free(&second);
        }
        reginold_regex_free(re);
    }
}

/* reginold_search_spans agrees with reginold_search span for span, reports
 * the group count on a mismatch, and refuses a buffer that is too small
 * without writing to it. */
static void
test_search_spans(void)
{
    static const struct {
        const char *pattern;
        const char *subject;
    } cases[] = {
        {"b", "abc"},
        {"(a)(b)(c)", "_abc"},
        {"(a)(b)?(c)", "xac"},
        {"^(\\w+)-(\\d+)@(\\w+)\\.com$", "user-42@site3.com"},
        {"(?<year>\\d+)-(?<month>\\d+)", "on 2026-11"},
        {"(é)(β)(🙂)", "xéβ🙂"},
        {"(a)(b)(c)\\3", "abcc"},
    };
    printf("\nCaller-buffer spans:\n");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reginold_regex *re = compile_ok(cases[i].pattern);
        if (!re) continue;
        const size_t len = strlen(cases[i].subject);
        reginold_match m = {0};
        reginold_span spans[8];
        size_t groups = 0;
        int ok = reginold_search(re, cases[i].subject, len, 0, &m) == REGINOLD_OK &&
            reginold_search_spans(re, cases[i].subject, len, 0, spans, 8, &groups) == REGINOLD_OK &&
            groups == m.capture_count + 1 &&
            spans[0].beg == m.overall.beg && spans[0].end == m.overall.end;
        for (size_t g = 0; ok && g < m.capture_count; g++)
            ok = spans[g + 1].beg == m.captures[g].beg &&
                 spans[g + 1].end == m.captures[g].end;
        reginold_span untouched[1] = {{.beg = 7, .end = 7}};
        size_t needed = 0;
        if (ok && groups > 1)
            ok = reginold_search_spans(re, cases[i].subject, len, 0, untouched, 1,
                                       &needed) == REGINOLD_ERROR &&
                 needed == groups && untouched[0].beg == 7 && untouched[0].end == 7;
        size_t mismatch_groups = 0;
        ok = ok && reginold_search_spans(re, "zz", 2, 0, spans, 8, &mismatch_groups) ==
                REGINOLD_MISMATCH && mismatch_groups == groups;
        if (ok)
            PASS("/%s/ spans match reginold_search", cases[i].pattern);
        else
            FAIL("/%s/ spans differ from reginold_search", cases[i].pattern);
        reginold_match_free(&m);
        reginold_regex_free(re);
    }
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int
main(void)
{
    test_compile();
    test_search_no_captures();
    test_search_start_offset();
    test_match_at();
    test_captures();
    test_optional_captures();
    test_fallback();
    test_options();
    test_null_out();
    test_region_storage();
    test_search_spans();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
