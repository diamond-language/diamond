# Generational GC: design map (not yet implemented)

This document maps out a design for turning `diamond_vm_collect`
(`src/vm.c:299`) into a generational collector. It is a plan for whoever
picks this up, not a description of shipped behavior — nothing here is
implemented (a first attempt at exactly this design was built, verified
correct, and reverted — see "Known flaw" immediately below before
starting another one). The prerequisite long-running workload now exists in
`bench/gc_churn`; its direct measurements also surfaced the flaw below. See
`CHANGELOG.md` for the concise history and `docs/roadmap.md` for possible future
collector directions.

## Known flaw: the write barrier below is the wrong granularity

The design as originally written (everything from "Object header" on)
remembers at **container granularity** — `gc_write_barrier(vm, owner)`
takes the whole `Array`/`Hash`/`Instance`/`Cell` that changed, not the
specific slot that changed. That's fine for a small object (an `Instance`
with a handful of fields, the shape most of this doc implicitly reasons
about) but actively harmful for a large, long-lived, frequently-mutated
container — exactly `bench/burn_in`'s and `bench/gc_churn`'s own
motivating workload (a `sessions`-style cache with thousands of entries,
one of which changes per request). A minor collection has to re-walk
*every* entry of a remembered container on *every* run
(`mark_remembered_set` → `mark_object_children`), since there's no
record of which entry actually changed — cost proportional to container
size, paid on nearly every minor collection once the container is
"remembered" continuously (which a frequently-written cache always is).
Measured on `bench/gc_churn/session_churn.di` at `live_set_size=20000,
iterations=200000`: 27,901 minor collections costing 64s combined,
against 2s for 27 major collections — a large net loss versus the
pre-generational collector's ~2.5s total GC time for a comparable run.

**Before implementing the rest of this doc as-is, redesign the write
barrier to remember at finer granularity** — the standard fix is card
marking (divide each large container's backing storage into fixed-size
"cards," and have the barrier record which card was touched rather than
the whole object, so a minor collection only re-scans the cards that
actually changed) or an equivalent per-entry/per-index scheme. Everything
else in this doc (object header, two-list structure, promotion-by-splice,
minor/major collection shape) held up fine under real measurement and
doesn't need to change — it's specifically `gc_write_barrier`'s
container-level granularity and `mark_remembered_set`'s whole-container
walk that need a different design.

## Baseline: what exists today

`diamond_vm_collect` is a single-generation, non-moving, stop-the-world
mark-sweep. One intrusive linked list (`vm->objects`), every object
individually `malloc`'d, a doubling `bytes_allocated`/`next_gc` threshold,
full root walk and full heap sweep on every collection regardless of how
much of the live set is actually garbage. `docs/design.md:288` documents
the collector as "non-generational and non-moving, so mutations do not
require a write barrier" — that invariant is exactly what this design
removes, deliberately and narrowly.

## The structural fact that makes this tractable

Every object here is already individually heap-allocated onto an intrusive
list — never bump-allocated out of an arena. That means **promotion can be
a pointer-preserving list splice** (unlink from the young list, relink onto
the old list) instead of a copying/compacting move.

This matters specifically for this codebase: `docs/design.md:68-70` notes
that raw C pointers to freshly allocated objects are routinely held across
subsequent allocation calls before being rooted (e.g. `array =
allocate_array(...)` followed by more work that can itself trigger a
collection). A copying nursery would invalidate those pointers on every
survived collection and require rewriting that rooting discipline across
most of `vm.c` and the embedding surface (`ProgramBuilder`) — a much
larger and riskier project than the GC change itself. Staying non-moving
sidesteps that entirely.

**Non-goal, stated explicitly: no moving/compacting collector.**
Fragmentation is accepted, exactly as it is today.

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

`vm->objects` splits into `vm->young_objects` / `vm->old_objects`, each
with its own byte counter. Minor collection triggers off a small nursery
threshold; major collection keeps today's doubling `next_gc` over total
bytes. `allocate_*` always allocates young.

## Write barrier

Only **old→young** pointer stores matter: an old object gaining a
reference to a young one is exactly the case a minor collection's normal
root walk wouldn't otherwise see. Barrier:

```c
if (owner->old && !owner->remembered) {
    remember(vm, owner);
    owner->remembered = true;
}
```

Unconditional on the stored value's own generation — cheaper branch, over-
remembers old→old writes, an acceptable tradeoff here.

### Audited mutation sites (current codebase)

- Instance field construction, `vm.c:6436-6437` — target is always
  freshly allocated (young), **no barrier needed**.
- Instance field store opcodes, `vm.c:8178`, `vm.c:8206` — **needs
  barrier**.
- `array_push`, `vm.c:4545`, and indexed element store, `vm.c:8341` —
  **needs barrier**.
- `hash_set`, `vm.c:3739` — **needs barrier**.
- `fiber->result` / `fiber->resume_value`, `vm.c:709`, `vm.c:726`,
  `vm.c:8478` — **needs barrier**, see risk note below.
- `thread->result`, `vm.c:984`, `vm.c:988`, `vm.c:7332` — **needs
  barrier**, see risk note below.
- Closure `captures[]`, `vm.c:886` — set exactly once at construction,
  never mutated afterward. **No barrier needed** — one fewer site to get
  wrong.
- **Two real gaps in this list, found during the first implementation
  attempt, not by design:** `Cell#value`, mutated post-construction by
  `DIAMOND_OP_SET_CAPTURE` and `DIAMOND_OP_SET_CELL` — **needs barrier**,
  keyed off the `Cell` object itself. And the `super()`-into-built-in-
  Exception-constructor path (the fallback in the `SUPER` opcode handler
  that assigns `self->fields[0]`/`[1]` directly when no user-defined
  `initialize` exists up the chain) — **needs barrier**, keyed off
  `self`; unlike `DIAMOND_OP_NEW`'s own exception-class field write
  (truly always-fresh, no barrier needed, confirmed no allocation
  happens between construction and that write), this one runs from deep
  inside a possibly-long-running `initialize` call chain where the
  receiver may well have already been promoted. Re-audit this list from
  scratch rather than trusting it as complete — line numbers above are
  already stale, and if this list missed two sites once, it can miss
  others.

### Sharpest risk in the whole design

`fiber->result`/`resume_value` and `thread->result` mutate the
`DiamondFiber`/`DiamondThread` payload struct directly, not the
`DiamondObject` header — the barrier at each of those sites has to reach
back to the owning `DiamondFiberHandle`/`DiamondThreadHandle` to check
`old`/`remembered`. A missed barrier here doesn't fail loudly: it's a live
young object silently reachable only through an old fiber/thread handle,
collected out from under a running program under load. This is the one
area worth extra scrutiny (and extra test coverage) whenever this is
implemented.

## Minor collection

Roots = the existing root walk (registers, frame chains, fibers, VM-level
exception/namespace-constant/signal-handler slots — reused unchanged) plus
every entry in the remembered set. Trace only into young objects; an old
object reached during trace is a dead end for this pass (its own young
pointers are already covered by the remembered set, so don't recurse into
it). Sweep only `vm->young_objects`.

Survivors promote immediately — list splice to `vm->old_objects`, `old =
true`. No age counter needed, since promotion is free here (unlike a
copying collector, there's no cost to promoting too eagerly). If premature
promotion of medium-lived objects shows up as a real problem once there's
a benchmark to measure it against, a 1-2 cycle survival threshold can be
added later; not designed in up front since it's unmotivated without data.

## Major collection

Unchanged algorithm, walking both lists — but **do not** unconditionally
clear the remembered set afterward. That was this doc's own original
text here, and it's wrong: an old→young edge established before a major
collection and never written to again has no future write-barrier firing
to rediscover it, so clearing makes it silently invisible to every later
minor collection (confirmed as a real bug during the first implementation
attempt, not just a theoretical concern). Filter instead: after the mark phase,
keep a remembered entry
iff its object is still `marked` (about to survive the sweep below);
drop it otherwise. A surviving old object's own `remembered` bit needs
no change — whatever young object it still points to was necessarily
also marked by this same full, unrestricted recursive pass, so it
survives too. (Also: do this filtering *before* sweeping, not after —
touching `->remembered` on an entry that sweep already freed is a
straightforward use-after-free, the other real bug the first attempt
hit.)

## Testing

`DIAMOND_STRESS_GC=1` already forces a (major) collection before every
eligible allocation. This design needs a nursery-scoped counterpart
(`DIAMOND_STRESS_MINOR_GC=1` — used under exactly that name in the first
implementation attempt, worked well, no reason to rename) that forces
minor collections aggressively. That's specifically to catch missing
write-barrier sites — the bug class this change introduces that the
codebase doesn't have today. In practice this worked as intended: it's
what caught the two mutation-site gaps above, though the two remembered-
set bugs (see "Major collection") needed `DIAMOND_STRESS_GC=1` combined
with AddressSanitizer to surface, not `DIAMOND_STRESS_MINOR_GC=1` alone
— run both stress flags together, and under ASan, not just one or the
other.

## Prerequisite (discharged)

This used to call for establishing a long-running benchmark before
implementing any of this — done: `bench/gc_churn` is exactly that workload,
short and non-networked
rather than a live `gremlin` server, and precise enough to have caught
the write-barrier granularity flaw above directly. Re-run it
(`bench/gc_churn/session_churn.di`, swept across live-set size) against
any future implementation before considering it done — it's what will
show whether a card-marked barrier actually fixes the regression, not
just whether the collector is correct.
