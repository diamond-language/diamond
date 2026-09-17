# Generational GC: design and outcome (implemented)

**Status: implemented.** `diamond_vm_collect`/`diamond_vm_collect_minor`
(`src/vm.c`) are now a generational collector, on the design below, after
one prior attempt was built, measured, and reverted because of the flaw
documented in the next section. This document is kept as the design record
and the measured before/after evidence — see `CHANGELOG.md` for the
concise history and `bench/gc_churn/README.md` for the full benchmark
sweep this design was validated against.

## Known flaw in the first attempt: the write barrier was the wrong granularity

The first attempt (everything from "Object header" on, below) remembered at
**container granularity** — `gc_write_barrier(vm, owner)` took the whole
`Array`/`Hash`/`Instance`/`Cell` that changed, not the specific slot that
changed. That's fine for a small object (an `Instance` with a handful of
fields, the shape most of this doc implicitly reasons about) but actively
harmful for a large, long-lived, frequently-mutated container — exactly
`bench/burn_in`'s and `bench/gc_churn`'s own motivating workload (a
`sessions`-style cache with thousands of entries, one of which changes per
request). A minor collection had to re-walk *every* entry of a remembered
container on *every* run (`mark_remembered_set` → `mark_object_children`),
since there was no record of which entry actually changed — cost
proportional to container size, paid on nearly every minor collection once
the container was "remembered" continuously (which a frequently-written
cache always is). Measured on `bench/gc_churn/session_churn.di` at
`live_set_size=20000, iterations=200000`: 27,901 minor collections costing
64s combined, against 2s for 27 major collections — a large net loss versus
the pre-generational collector's ~2.5s total GC time for a comparable run.

The fix, implemented as "Card marking" below: divide `Array`/`Hash`'s
backing storage into fixed-size cards, and have the barrier record which
card was touched rather than the whole object, so a minor collection only
re-scans the cards that actually changed. Everything else in the original
design (object header, two-list structure, promotion-by-splice, minor/
major collection shape) held up under real measurement and didn't need to
change.

## Baseline: what existed before this

`diamond_vm_collect` was a single-generation, non-moving, stop-the-world
mark-sweep. One intrusive linked list (`vm->objects`), every object
individually `malloc`'d, a doubling `bytes_allocated`/`next_gc` threshold,
full root walk and full heap sweep on every collection regardless of how
much of the live set was actually garbage.

## The structural fact that made this tractable

Every object here is already individually heap-allocated onto an intrusive
list — never bump-allocated out of an arena. That means **promotion can be
a pointer-preserving list splice** (unlink from the young list, relink onto
the old list) instead of a copying/compacting move.

This mattered specifically for this codebase: raw C pointers to freshly
allocated objects are routinely held across subsequent allocation calls
before being rooted (e.g. `array = allocate_array(...)` followed by more
work that can itself trigger a collection). A copying nursery would have
invalidated those pointers on every survived collection and required
rewriting that rooting discipline across most of `vm.c` and the embedding
surface (`ProgramBuilder`) — a much larger and riskier project than the GC
change itself. Staying non-moving sidestepped that entirely.

**Non-goal, stated explicitly: no moving/compacting collector.**
Fragmentation is accepted, exactly as it was before.

## Object header

```c
typedef struct DiamondObject {
    struct DiamondObject *next;
    DiamondObjectKind kind;
    bool marked;
    bool old;         /* generation: false = nursery, true = tenured */
    bool remembered;  /* already in vm->remembered_set (old objects only) */
} DiamondObject;
```

## Two lists, two thresholds

`vm->objects` split into `vm->young_objects` / `vm->old_objects`. Minor
collection triggers off a small, fixed nursery threshold
(`vm->minor_gc_threshold_bytes`, checked as a snapshot/delta against
`vm->bytes_allocated` rather than a separately-incremented counter); major
collection keeps the original doubling `next_gc` over total bytes.
`allocate_*` always allocates young.

## Write barrier

Only **old→young** pointer stores matter: an old object gaining a
reference to a young one is exactly the case a minor collection's normal
root walk wouldn't otherwise see. Base barrier (`gc_write_barrier`, used by
`Instance`/`Cell`/`Fiber`/`Thread` mutation sites):

```c
if (owner->old && !owner->remembered) {
    remember(vm, owner);
    owner->remembered = true;
}
```

Unconditional on the stored value's own generation — cheaper branch, over-
remembers old→old writes, an acceptable tradeoff.

### Card marking (Array/Hash)

`Array`/`Hash` use `gc_write_barrier_index`/`gc_write_barrier_range`
instead: same remembered-set bookkeeping, plus marking the specific
`DIAMOND_GC_CARD_SIZE`-element card (64 elements) that the write touched
dirty in a lazily-allocated `dirty_cards` byte table. `mark_remembered_set`
then walks only the dirty cards of a remembered `Array`/`Hash` — clearing
each as it's scanned — instead of every element. `Instance`/`Cell`/`Fiber`/
`Thread` stay whole-object-remembered: bounded size, no motivating cost.

### Audited mutation sites

- Instance field construction (`DIAMOND_OP_NEW`, `DIAMOND_OP_NEW_SPREAD`'s
  own default-constructor fallback, `copy_value_into_vm`'s Array/Hash
  cases via `array_push`/`hash_set`) — target is always freshly allocated
  (young) with no allocation in between, **no barrier needed**.
- `DIAMOND_OP_SET_IVAR`, `DIAMOND_OP_SET_IVAR_NAME` — barrier.
- `array_push`, `DIAMOND_OP_INDEX_SET` (single index and range-write) —
  index/range barrier (card marking).
- `hash_set` (both the existing-key-update path and the insert path) —
  index barrier (card marking).
- `DIAMOND_OP_SET_CAPTURE`, `DIAMOND_OP_SET_CELL` — barrier, keyed off the
  `Cell` object.
- `DIAMOND_OP_SUPER`'s built-in-Exception-constructor fallback
  (`self->fields[0]`/`[1]` when no user `initialize` exists up the chain)
  — barrier, keyed off `self`.
- Fiber `#resume` (`fiber->result`/`resume_value`) and Thread `#join`
  (`thread->result`) — barrier, keyed off the owning
  `DiamondFiberHandle`/`DiamondThreadHandle` (see "sharpest risk" below).
- Closure `captures[]` — set exactly once at construction, never mutated
  afterward. **No barrier needed.**
- **Raw C-level field writes outside any opcode**, found during this
  implementation's own `DIAMOND_STRESS_MINOR_GC=1` + ASan verification
  (not anticipated by the original design): several helpers root a
  freshly-allocated object, then call *another* allocator (which can
  itself trigger a minor GC and promote the now-rooted object) before
  writing one of its fields directly in C — bypassing the interpreter's
  own opcode-level barrier entirely. Fixed in `catch_runtime_error`
  (exception message field), `raise_capture_backtrace_helper` (backtrace
  field), `copy_value_into_vm`'s Instance-copy loop, the Thread-join
  internal-failure error path, and `process_run_helper`'s stdout/stderr
  fields. The pattern to watch for in any future such helper: "allocate
  object A, root it, allocate object B (can trigger GC), then `A->field =
  B` directly in C" always needs its own explicit `gc_write_barrier` call.

### Sharpest risk in the whole design

`fiber->result`/`resume_value` and `thread->result` mutate the
`DiamondFiber`/`DiamondThread` payload struct directly, not the
`DiamondObject` header — the barrier at each of those sites has to reach
back to the owning `DiamondFiberHandle`/`DiamondThreadHandle` to check
`old`/`remembered`. A missed barrier here doesn't fail loudly: it's a live
young object silently reachable only through an old fiber/thread handle,
collected out from under a running program under load. Implemented by
calling the barrier once, on the handle, immediately after
`diamond_fiber_run`/the thread-join copy — every mutation of these payload
fields happens strictly within the dynamic extent of that one call, so a
single barrier call there covers all of them (including a nested
yield/resume chain, since each nested `.resume()` goes through this same
call site for its own fiber).

## Minor collection

Roots = the existing root walk (registers, frame chains, fibers, VM-level
exception/namespace-constant/signal-handler slots — reused unchanged) plus
every entry in the remembered set. Trace only into young objects; an old
object reached during trace is a dead end for this pass (its own young
pointers are already covered by the remembered set, so don't recurse into
it). Sweep only `vm->young_objects`.

Survivors promote immediately — list splice to `vm->old_objects`, `old =
true`. No age counter: promotion is free here (unlike a copying collector,
there's no cost to promoting too eagerly).

## Major collection

Unchanged algorithm, walking both lists — but does **not** unconditionally
clear the remembered set afterward (the first attempt's own bug: an
old→young edge established before a major collection and never written to
again has no future write-barrier firing to rediscover it, so clearing
makes it silently invisible to every later minor collection). Filtered
instead: after the mark phase, a remembered entry is kept iff its object is
still `marked` (about to survive the sweep below); dropped otherwise. This
filtering happens *before* sweeping, not after — touching `->remembered` on
an entry that sweep already freed would be a straightforward
use-after-free, the other real bug the first attempt hit.

## Testing

`DIAMOND_STRESS_GC=1` forces a major collection before every eligible
allocation; `DIAMOND_STRESS_MINOR_GC=1` forces a minor one. Both, run
together and under ASan/UBSan, are what actually caught every real bug in
this implementation (both the two remembered-set bugs the first attempt
hit, and the raw-C-field-write gaps found this time) — neither stress flag
alone was sufficient.

## Results

`bench/gc_churn/session_churn.di` (live_set=20000, iterations=200000),
compared across all three points in this project's history:

| collector | major collections | major GC time | minor collections | minor GC time |
|---|---:|---:|---:|---:|
| pre-generational (baseline) | 27 | ~2.5s (total) | n/a | n/a |
| first attempt (whole-object remembering, reverted) | 27 | ~2s | 27,901 | 64s |
| this implementation, before card marking (Phase 2) | 27 | 2.3s | 53,820 | 325s |
| **this implementation, after card marking (Phase 4)** | 27 | 2.1s | 3,446 | **0.4s** |

Card marking cut minor-collection time by three orders of magnitude versus
the whole-object-remembering version, and total GC time is now close to
the pre-generational baseline. More importantly, per the original
motivation (bounding *pause length*, not aggregate CPU): total GC time is
now roughly flat (~2.0-2.5s) across a live-set-size sweep from 1,000 to
40,000 entries, rather than growing with the live set the way a single
generation's full-heap collection did. See `bench/gc_churn/README.md` for
the complete sweep and the pre-generational baseline it's compared against.

Wall time is still higher than the pre-generational baseline at comparable
settings (roughly 12-13s vs. ~7.3-7.9s across the live-set sweep) — some
combination of the per-object `old`/`remembered` bookkeeping now present on
every allocation and mutation path, and every minor-collection survivor
being promoted (so the old generation grows faster than it would with a
survival threshold). Not chased further here since it wasn't the stated
goal; a future pass could revisit whether a 1-2 cycle survival threshold
before promotion (mentioned as a possible follow-up in the original design
below) recovers some of that gap.
