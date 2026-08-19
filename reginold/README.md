# reginold

## ELI5 Summary

This project is a wrapper around a big, complicated regex engine so other programs can use it without having to understand all the guts inside. In kid terms, it is like building a safe power outlet: you still get the electricity, but the sharp parts are hidden behind a simple plug.

Right now reginold looks active and pretty focused. It already exposes a stable C API, it vendors the Onigmo pieces it needs so it can build on its own, and the latest local work moved it further toward fully standalone builds, with the repo currently one commit ahead of `origin/master`.

Embeddable regex library built over Onigmo. Provides a stable, opaque C API
that hides all Onigmo and MRI internals from consumers.

## Integration

Consumers need two things:

- **Header**: `reginold.h` — no Onigmo or Ruby headers required.
- **Library**: `libreginold.a` — compiles Onigmo from source under `-DNOT_RUBY`
  and bundles everything into one archive. No `libruby` or Ruby VM linkage needed.

```makefile
REGINOLD_DIR = ../reginold
CFLAGS  += -I$(REGINOLD_DIR)
LDFLAGS += $(REGINOLD_DIR)/libreginold.a -lm -ldl -lpthread
```

Build `libreginold.a` first. The repository vendors the exact Onigmo sources,
Unicode tables, and generated config it depends on, so no external Ruby source
tree is required:

```sh
make libreginold.a
```

## Public API

All types and functions are declared in `reginold.h`.

### Compile

```c
reginold_status reginold_compile(const char     *pattern,
                                 size_t          pattern_len,
                                 unsigned int    options,
                                 reginold_regex **out,
                                 reginold_error  *err);
```

Compiles a UTF-8 pattern under Ruby syntax. On `REGINOLD_OK`, `*out` is a
newly allocated handle that must be freed with `reginold_regex_free`. On
`REGINOLD_ERROR`, `err` (if non-NULL) is filled with the error code, a
human-readable message, and the byte offset in the pattern where the error
occurred.

**Options** (bitwise OR):

| Flag | Effect |
|---|---|
| `REGINOLD_OPTION_NONE` | Default |
| `REGINOLD_OPTION_IGNORECASE` | Case-insensitive matching |
| `REGINOLD_OPTION_MULTILINE` | `.` matches `\n` |
| `REGINOLD_OPTION_EXTENDED` | Whitespace and `#` comments ignored |

### Search

```c
reginold_status reginold_search(const reginold_regex *re,
                                const char           *bytes,
                                size_t                len,
                                size_t                start,
                                reginold_match       *out);
```

Finds the leftmost match in `bytes[0..len)`, beginning the search at byte
offset `start`. Returns `REGINOLD_OK` on match, `REGINOLD_MISMATCH` if no
match exists, `REGINOLD_ERROR` on internal failure.

Pass `NULL` for `out` to test for a match without allocating captures (useful
for `match?`-style checks).

### Anchored match

```c
reginold_status reginold_match_at(const reginold_regex *re,
                                  const char           *bytes,
                                  size_t                len,
                                  size_t                at,
                                  reginold_match       *out);
```

Attempts a match anchored at byte offset `at`. Returns `REGINOLD_OK` only if
the pattern matches at exactly that position.

### Results

```c
typedef struct {
    long beg;  /* start byte offset (inclusive); -1 if unmatched */
    long end;  /* end   byte offset (exclusive); -1 if unmatched */
} reginold_span;

typedef struct {
    reginold_span  overall;        /* span of the full match                 */
    size_t         capture_count;  /* number of capture groups (groups 1..n) */
    reginold_span *captures;       /* heap-allocated; [0]=group1, [1]=group2 */
} reginold_match;
```

On `REGINOLD_OK`, `out->captures` is heap-allocated and must be released:

```c
reginold_match_free(&m);   /* frees captures; does not free the struct itself */
```

`reginold_match_free` is safe to call even when `capture_count == 0` (captures
will be NULL).

### Free

```c
void reginold_regex_free(reginold_regex *re);
```

Safe to call with NULL.

## Byte-offset semantics

All positions are **byte offsets** into the input string, not character offsets.
For ASCII and single-byte encodings these are identical. For UTF-8 input,
`beg` and `end` index bytes, not codepoints.

`reginold_span.beg` is inclusive; `reginold_span.end` is exclusive. The
matched text is `bytes[span.beg .. span.end - 1]`.

An unmatched optional capture group has `{-1, -1}` for both `beg` and `end`.

`overall` in `reginold_match` always reflects the full match span (group 0).
`captures[i]` holds the span for capture group `i+1` (1-based in regex
notation).

## Execution tiers and fallback

Patterns are classified at compile time and routed to one of three execution
tiers:

**Tier 1** — no capturing groups. Executed by a Thompson bitset NFA simulation.
Linear time `O(n·m)`, no backtracking.

**Tier 2** — capturing groups present, no disqualifying features. Executed by
a Laurikari tagged NFA. Linear time `O(n·m·k)` where `k` is the number of
capture groups. No backtracking.

**Tier 3** — Onigmo fallback. Used when the pattern contains any of:
backreferences (`\1`, `\k<name>`), lookahead (`(?=...)`, `(?!...)`),
lookbehind (`(?<=...)`, `(?<!...)`), subexpression calls (`\g<name>`), the
absent operator (`(?~...)`), or conditional patterns.

Tier 3 patterns are fully supported — they produce correct results via Onigmo.
The tier is an implementation detail; the public API behaves identically across
all tiers. Callers do not need to know which tier a pattern uses.

### Multibyte note

The Tier 1/2 NFA simulation is byte-level. Patterns that require
character-level `.` semantics over multibyte input (e.g. `/caf./` matching
UTF-8 `"café"` with `.` spanning `é`) are classified as Tier 3 and handled
by Onigmo. Byte-literal patterns over multibyte strings work correctly in
all tiers.

## Example

```c
#include "reginold.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    reginold_regex *re  = NULL;
    reginold_error  err = {0};

    if (reginold_compile("(\\d+)-(\\d+)", 11, REGINOLD_OPTION_NONE,
                         &re, &err) != REGINOLD_OK) {
        fprintf(stderr, "compile error: %s\n", err.message);
        return 1;
    }

    const char     *input = "id:42-99";
    reginold_match  m     = {0};

    if (reginold_search(re, input, strlen(input), 0, &m) == REGINOLD_OK) {
        printf("match   [%ld,%ld)\n", m.overall.beg, m.overall.end);
        for (size_t i = 0; i < m.capture_count; i++)
            printf("group %zu [%ld,%ld)\n", i+1,
                   m.captures[i].beg, m.captures[i].end);
        reginold_match_free(&m);
    }

    reginold_regex_free(re);
    return 0;
}
```

Output:

```
match   [3,8)
group 1 [3,5)
group 2 [6,8)
```
