#ifndef REGINOLD_H
#define REGINOLD_H
/*
 * reginold.h — public API for the reginold regex library.
 *
 * This header has no Onigmo or Ruby dependencies.
 * Consumers need only: -I<reginold_dir> and -lreginold.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque compiled regex handle ────────────────────────────────────────── */

typedef struct reginold_regex reginold_regex;

/* ── Option flags ─────────────────────────────────────────────────────────── */

#define REGINOLD_OPTION_NONE        0u
#define REGINOLD_OPTION_IGNORECASE  (1u << 0)  /* case-insensitive matching   */
#define REGINOLD_OPTION_MULTILINE   (1u << 1)  /* . matches \n                */
#define REGINOLD_OPTION_EXTENDED    (1u << 2)  /* whitespace and # comments   */

/* ── Status ───────────────────────────────────────────────────────────────── */

typedef enum {
    REGINOLD_OK       =  0,
    REGINOLD_MISMATCH = -1,
    REGINOLD_ERROR    = -2
} reginold_status;

/* ── Error ────────────────────────────────────────────────────────────────── */

/* Buffer size matches Onigmo's ONIG_MAX_ERROR_MESSAGE_LEN. */
#define REGINOLD_ERROR_MSG_MAX 90

typedef struct {
    int    code;
    char   message[REGINOLD_ERROR_MSG_MAX];
    size_t message_len;
    size_t error_offset;  /* byte offset into pattern; 0 if not positional */
} reginold_error;

/* ── Match result ─────────────────────────────────────────────────────────── */

typedef struct {
    long beg;  /* start byte offset (inclusive); -1 if unmatched */
    long end;  /* end   byte offset (exclusive); -1 if unmatched */
} reginold_span;

typedef struct {
    reginold_span  overall;        /* span of the full match                  */
    size_t         capture_count;  /* number of capture groups (groups 1..n)  */
    reginold_span *captures;       /* heap-allocated; [0]=group1, [1]=group2… */
} reginold_match;

/* ── API ──────────────────────────────────────────────────────────────────── */

/*
 * reginold_compile: compile a UTF-8 pattern under Ruby syntax.
 *
 * On success sets *out and returns REGINOLD_OK.
 * On failure fills *err (if non-NULL) and returns REGINOLD_ERROR.
 * A successful *out must be released with reginold_regex_free.
 */
reginold_status reginold_compile(const char     *pattern,
                                 size_t          pattern_len,
                                 unsigned int    options,
                                 reginold_regex **out,
                                 reginold_error  *err);

/*
 * reginold_search: find the leftmost match in bytes[0..len), starting at `start`.
 *
 * If out is non-NULL, fills it on a successful match.
 * Returns REGINOLD_OK on match, REGINOLD_MISMATCH on no match, REGINOLD_ERROR
 * on internal failure.
 * On REGINOLD_OK, caller must release out->captures with reginold_match_free.
 */
reginold_status reginold_search(const reginold_regex *re,
                                const char           *bytes,
                                size_t                len,
                                size_t                start,
                                reginold_match       *out);

/*
 * reginold_match_at: attempt a match anchored at byte offset `at`.
 *
 * Returns REGINOLD_OK if the pattern matches at that exact position,
 * REGINOLD_MISMATCH if it does not, REGINOLD_ERROR on internal failure.
 * On REGINOLD_OK, caller must release out->captures with reginold_match_free.
 */
reginold_status reginold_match_at(const reginold_regex *re,
                                  const char           *bytes,
                                  size_t                len,
                                  size_t                at,
                                  reginold_match       *out);

/* Release the captures array inside a match result. */
void reginold_match_free(reginold_match *m);

/* Release a compiled regex. */
void reginold_regex_free(reginold_regex *re);

#ifdef __cplusplus
}
#endif

#endif /* REGINOLD_H */
