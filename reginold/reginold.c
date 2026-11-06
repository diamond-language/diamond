#include <stdlib.h>
#include "reginold.h"
#include "engine.h"  /* RegoldEngine, regold_*; transitively pulls in onigmo_compat.h */

/* ── Opaque handle ────────────────────────────────────────────────────────── */

struct reginold_regex {
    RegoldEngine *engine;
};

/* ── Onigmo init ──────────────────────────────────────────────────────────── */

/*
 * onig_init() calls onigenc_init() which is a no-op in Ruby's Onigmo build.
 * No Ruby VM or encoding-DB state is required. Safe to call before any Ruby
 * runtime is present. Repeated calls are harmless (onig_inited guard).
 */
static int s_initialized = 0;

static void
ensure_init(void)
{
    if (!s_initialized) {
        onig_init();
        s_initialized = 1;
    }
}

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static OnigOptionType
translate_options(unsigned int opts)
{
    OnigOptionType o = ONIG_OPTION_NONE;
    if (opts & REGINOLD_OPTION_IGNORECASE) o |= ONIG_OPTION_IGNORECASE;
    if (opts & REGINOLD_OPTION_MULTILINE)  o |= ONIG_OPTION_MULTILINE;
    if (opts & REGINOLD_OPTION_EXTENDED)   o |= ONIG_OPTION_EXTEND;
    return o;
}

static void
fill_error(reginold_error *err, int code, OnigErrorInfo *einfo,
           const char *pattern, size_t pattern_len)
{
    if (!err) return;
    err->code = code;
    int n = onig_error_code_to_str((UChar *)err->message, code, einfo);
    err->message_len = (n > 0) ? (size_t)n : 0;
    if (einfo && einfo->par) {
        const UChar *pat = (const UChar *)pattern;
        if (einfo->par >= pat && einfo->par <= pat + pattern_len)
            err->error_offset = (size_t)(einfo->par - pat);
        else
            err->error_offset = 0;
    } else {
        err->error_offset = 0;
    }
}

static reginold_status
fill_match(OnigRegion *region, reginold_match *out)
{
    out->overall.beg = region->beg[0];
    out->overall.end = region->end[0];

    size_t ncap = region->num_regs > 1 ? (size_t)(region->num_regs - 1) : 0;
    out->capture_count = ncap;

    if (ncap == 0) {
        out->captures = NULL;
        return REGINOLD_OK;
    }

    out->captures = malloc(ncap * sizeof(reginold_span));
    if (!out->captures) return REGINOLD_ERROR;

    for (size_t i = 0; i < ncap; i++) {
        out->captures[i].beg = region->beg[i + 1];
        out->captures[i].end = region->end[i + 1];
    }
    return REGINOLD_OK;
}

/* Onigmo always reserves at least ONIG_NREGION slots. Keep that common
 * case on the call stack; larger capture sets retain its heap allocation.
 * Both native Onigmo and our NFA resize to num_mem + 1, so this storage
 * cannot be passed to realloc. Nothing is cached on the shared regex. */
typedef struct {
    OnigRegion region;
    OnigPosition beg[ONIG_NREGION];
    OnigPosition end[ONIG_NREGION];
} MatchRegion;

static void
match_region_init(MatchRegion *scratch, const reginold_regex *re)
{
    onig_region_init(&scratch->region);
    if (re->engine->onig->num_mem < ONIG_NREGION) {
        scratch->region.allocated = ONIG_NREGION;
        scratch->region.beg = scratch->beg;
        scratch->region.end = scratch->end;
    }
}

static void
match_region_free(MatchRegion *scratch)
{
    if (scratch->region.beg == scratch->beg)
        scratch->region.allocated = 0;
    /* Also releases capture history, when enabled, without freeing the
     * region itself or either of our stack buffers. */
    onig_region_free(&scratch->region, 0);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

reginold_status
reginold_compile(const char *pattern, size_t pattern_len,
                 unsigned int options,
                 reginold_regex **out,
                 reginold_error *err)
{
    ensure_init();

    reginold_regex *re = malloc(sizeof(*re));
    if (!re) return REGINOLD_ERROR;

    OnigErrorInfo einfo;
    const UChar *p = (const UChar *)pattern;
    int r = regold_new(&re->engine,
                       p, p + pattern_len,
                       translate_options(options),
                       ONIG_ENCODING_UTF8,
                       ONIG_SYNTAX_RUBY,
                       &einfo);
    if (r != ONIG_NORMAL) {
        fill_error(err, r, &einfo, pattern, pattern_len);
        free(re);
        return REGINOLD_ERROR;
    }

    *out = re;
    return REGINOLD_OK;
}

reginold_status
reginold_search(const reginold_regex *re,
                const char *bytes, size_t len,
                size_t start,
                reginold_match *out)
{
    const UChar *str    = (const UChar *)bytes;
    const UChar *end    = str + len;
    const UChar *sptr   = str + start;
    MatchRegion scratch;
    OnigRegion *region = NULL;
    if (out) {
        match_region_init(&scratch, re);
        region = &scratch.region;
    }

    int r = regold_search(re->engine, str, end, sptr, end, region, ONIG_OPTION_NONE);

    reginold_status status;
    if (r == ONIG_MISMATCH) {
        status = REGINOLD_MISMATCH;
    } else if (r < 0) {
        status = REGINOLD_ERROR;
    } else if (out) {
        status = fill_match(region, out);
    } else {
        status = REGINOLD_OK;
    }

    if (region) match_region_free(&scratch);
    return status;
}

reginold_status
reginold_search_spans(const reginold_regex *re,
                      const char *bytes, size_t len,
                      size_t start,
                      reginold_span *spans, size_t capacity,
                      size_t *group_count)
{
    const size_t groups = (size_t)re->engine->onig->num_mem + 1;
    *group_count = groups;
    if (groups > capacity) return REGINOLD_ERROR;

    const UChar *str  = (const UChar *)bytes;
    const UChar *end  = str + len;
    const UChar *sptr = str + start;
    MatchRegion scratch;
    match_region_init(&scratch, re);

    int r = regold_search(re->engine, str, end, sptr, end, &scratch.region,
                          ONIG_OPTION_NONE);

    reginold_status status;
    if (r == ONIG_MISMATCH) {
        status = REGINOLD_MISMATCH;
    } else if (r < 0) {
        status = REGINOLD_ERROR;
    } else {
        /* A region can report fewer registers than the pattern has groups
         * (the NFA's Tier 1 fills only the overall match); the rest did
         * not participate. */
        const size_t filled = scratch.region.num_regs > 0
            ? (size_t)scratch.region.num_regs : 0;
        for (size_t i = 0; i < groups; i++) {
            spans[i].beg = i < filled ? scratch.region.beg[i] : -1;
            spans[i].end = i < filled ? scratch.region.end[i] : -1;
        }
        status = REGINOLD_OK;
    }

    match_region_free(&scratch);
    return status;
}

reginold_status
reginold_match_at(const reginold_regex *re,
                  const char *bytes, size_t len,
                  size_t at,
                  reginold_match *out)
{
    const UChar *str    = (const UChar *)bytes;
    const UChar *end    = str + len;
    const UChar *atptr  = str + at;
    MatchRegion scratch;
    OnigRegion *region = NULL;
    if (out) {
        match_region_init(&scratch, re);
        region = &scratch.region;
    }

    int r = regold_match(re->engine, str, end, atptr, region, ONIG_OPTION_NONE);

    reginold_status status;
    if (r == ONIG_MISMATCH) {
        status = REGINOLD_MISMATCH;
    } else if (r < 0) {
        status = REGINOLD_ERROR;
    } else if (out) {
        status = fill_match(region, out);
    } else {
        status = REGINOLD_OK;
    }

    if (region) match_region_free(&scratch);
    return status;
}

void
reginold_match_free(reginold_match *m)
{
    if (!m) return;
    free(m->captures);
    m->captures = NULL;
}

void
reginold_regex_free(reginold_regex *re)
{
    if (!re) return;
    regold_free(re->engine);
    free(re);
}
