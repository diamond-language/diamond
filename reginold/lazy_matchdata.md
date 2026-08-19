# Lazy MatchData — Design Notes

## What "lazy" means here

Ruby's `MatchData` object wraps a `struct re_registers` (`regs`) that holds every
capture group's byte offsets, allocated and populated by Onigmo at search time.
Two things are already lazy:

- **`char_offset`**: Character-position counterparts of `regs` are computed on first
  access by `update_char_offset()` in `re.c`. ✓ Done.

The remaining opportunity:

- **`regs` itself**: For Tier 1 patterns (no captures), Onigmo always fills
  `regs.beg[0..num_regs-1]` even though only `beg[0]`/`end[0]` (the overall match)
  is ever meaningful. For Tier 2 patterns, the entire capture array is populated
  eagerly even when the caller only inspects `$&` (overall match) or never
  dereferences the MatchData at all (`str.match?(re)` discards it immediately).

---

## Current code path (`re.c`)

```
rb_reg_search_set_match()
  → rb_reg_onig_match()          ← calls Onigmo (populates full regs)
  → rm->regs = regs              ← store in MatchData
  → rb_backref_set(match)
```

`onig_search` fills `regs.beg[i]` / `regs.end[i]` for every capture group,
even those the caller never touches.

---

## What we can do with regionold

### Tier 1 (no captures)

Call `regold_search()` instead of `onig_search()`.  The result is:

```c
region->num_regs = 1;
region->beg[0]   = match_start;
region->end[0]   = match_end;
```

`rm->regs` ends up with `num_regs == 1`, saving any allocation for the unused
`beg[1..n]` / `end[1..n]` slots.  No further laziness is needed or possible —
the overall-match offsets must be available immediately (used by `$~`, `$&`,
`String#[]` with a Regexp argument, etc.).

### Tier 2 (captures present)

Two options:

**Option A — Eager, regionold path (immediate win)**

Call `regold_search()`.  We run the Laurikari tagged NFA (O(n·m·k)) instead of
Onigmo's backtracking engine.  All capture offsets are populated eagerly, just
like today, but without catastrophic-backtracking risk.  No MatchData changes
needed.  This is the minimum viable integration.

**Option B — Lazy capture population (deeper change)**

Defer writing individual capture positions until a capture is actually accessed.
Implementation sketch:

1. **New field in `rb_matchext_t`** (`include/ruby/internal/core/rmatch.h`):

   ```c
   typedef struct rb_matchext_struct {
       struct re_registers  regs;
       rmatch_offset       *char_offset;
       int                  char_offset_num_allocated;
       /* --- NEW --- */
       struct RegoldLazyState *lazy;  /* non-NULL while captures are deferred */
   } rb_matchext_t;
   ```

2. **`RegoldLazyState`** (defined in `regionold/engine.h`):

   ```c
   typedef struct RegoldLazyState {
       RegoldEngine       *engine;   /* borrowed — lives as long as the regex */
       VALUE               str;      /* GC root — the matched string           */
       long                match_start;
       long                match_end;
   } RegoldLazyState;
   ```

3. **At search time** (in `rb_reg_search_set_match`):

   For Tier 2, instead of calling `regold_search` with a full `OnigRegion`, call
   it without captures (just get overall match start/end), allocate a
   `RegoldLazyState`, and store it in `rm->lazy`.  Set `regs.num_regs = 1` with
   only `beg[0]`/`end[0]`.

4. **`ensure_captures(match)`** — a new helper in `re.c`:

   ```c
   static void
   ensure_captures(VALUE match)
   {
       rb_matchext_t *rm = RMATCH_EXT(match);
       if (!rm->lazy) return;   /* already populated */

       RegoldLazyState *ls = rm->lazy;
       /* Re-run the tagged NFA with capture tracking. */
       int num = ls->engine->onig->num_mem + 1;
       ExecCaptures caps;
       alloc_caps(&caps, num);
       exec_match(ls->engine->nfa, &ls->engine->cls,
                  (const unsigned char *)RSTRING_PTR(ls->str),
                  RSTRING_LEN(ls->str),
                  ls->match_start, &caps);
       populate_region(&rm->regs, &caps);
       free_caps(&caps);
       rm->lazy = NULL;
       free(ls);
   }
   ```

5. **Gate every capture accessor** in `re.c` through `ensure_captures(match)`:

   - `match_nth()` / `rb_reg_nth_match()`
   - `match_begin()` / `match_end()`
   - `match_captures()`, `match_named_captures()`, `match_values_at()`
   - `rb_reg_region_copy()` (called on MatchData#dup / backref copy)
   - `update_char_offset()` (already deferred, but needs the regs to be real)

---

## Cost / benefit

| Scenario | Today | With lazy |
|---|---|---|
| `str.match?(re)` — discard result | Onigmo full search | T1: bitset sim, no regs. T2: bitset sim for overall match only; lazy regs never materialised. |
| `str =~ re` / `$&` only | Full regs allocated | Same as today for beg[0]/end[0]; others deferred (T2) or absent (T1). |
| `m = str.match(re); m[1]` | Full regs | T2: overall match at search time; captures materialised on `m[1]`. |
| `m = str.match(re); m[1]; m[2]` | Full regs | T2: second access is free (already materialised). |
| Pattern with 20 groups, only [1] used | 20 groups allocated | 1 re-run of tagged NFA on first access; all 20 filled at that point. |

The double-run for Tier 2 (overall match + re-run on first capture access) adds
one tagged-NFA pass in the common case where at least one capture IS accessed.
This is a net win only when captures are frequently ignored — e.g. `str.match?(re)`
or `str =~ re` where only `$&` is used.

For patterns where captures are always accessed, Option A (eager tagged NFA, no
re-run) is strictly better.

---

## Recommended sequencing

1. **Integrate regionold into re.c** — swap `onig_search`/`onig_match` for
   `regold_search`/`regold_match`.  This delivers the linear-time guarantee for
   T1/T2 patterns with no MatchData changes.  (Option A above.)

2. **Profile** whether eager vs. deferred capture population matters for real
   workloads.  The GC pressure from large `regs` allocations that are immediately
   discarded is the main signal to watch.

3. **Implement Option B** only if profiling shows a meaningful gain.  The
   correctness surface is larger (every capture accessor must call
   `ensure_captures`; GC must keep `str` alive while `lazy` exists).

---

## Files to touch for Option A integration

| File | Change |
|---|---|
| `ruby/internal/core/rregexp.h` | Add `RegoldEngine *regionold_engine` to `struct RRegexp` |
| `ruby/re.c` | `rb_reg_initialize_m` → also call `regold_new`; `rb_reg_search_set_match` → call `regold_search`; `rb_reg_free` → call `regold_free` |
| `ruby/Makefile` / `ruby/common.mk` | Add regionold `.c` files to the build |
