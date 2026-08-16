# Generational GC: design map (not yet implemented)

This document maps out a design for turning `diamond_vm_collect`
(`src/vm.c:299`) into a generational collector. It is a plan for whoever
picks this up, not a description of shipped behavior — nothing here is
implemented. `docs/roadmap.md`'s "Generational or incremental GC" entry
scoped this as deliberately not started until a real long-running workload
(the roadmap suggests a `gremlin` server under sustained load) exists to
validate against; that prerequisite still stands. This doc exists so the
design work doesn't have to be re-derived when that benchmark lands.

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

Unchanged algorithm, walking both lists, plus clearing the remembered set
(every old object gets freshly re-scanned by a major collection, so
existing remembered entries are redundant until the next barrier fires).

## Testing

`DIAMOND_STRESS_GC=1` already forces a (major) collection before every
eligible allocation. This design needs a nursery-scoped counterpart
(`DIAMOND_STRESS_MINOR_GC=1`, naming TBD) that forces minor collections
aggressively. That's specifically to catch missing write-barrier sites —
the bug class this change introduces that the codebase doesn't have today.

## Prerequisite (unchanged from the roadmap)

Establish an actual long-running benchmark — a `gremlin` server under
sustained load is still the obvious candidate — before implementing any
of this. The whole point of generational collection is trading full-heap
rescans for cheaper, more frequent nursery-only ones; without a workload
with a large, mostly-stable live set and steady young-object churn, there's
no way to confirm this design actually buys anything over the current
collector's simplicity.
