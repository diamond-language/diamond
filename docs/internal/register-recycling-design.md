# Register recycling: design and status (Stage 1 landed, Stage 2+ planned)

**Status: Stage 1 implemented.** The compiler's register allocator
(`allocate_register`, `src/compiler.c`) never reuses a slot within one
function body -- every temporary and every local, however short-lived,
claims a fresh register number and keeps it for the rest of the function.
This document records why that costs real, measured time, what Stage 1
fixes and how it was made safe (including a real bug it caught along the
way), and the harder cases left for later stages.

## The problem, measured

A run_chunk call zeroes exactly `chunk->register_count` `DiamondValue`
slots (16 bytes each) on every invocation -- `register_count` is
`next_register`'s final value when the function finished compiling, i.e.
the *total distinct registers ever allocated*, never the number actually
live at once. Two things make that number bigger than the function's own
logic would suggest:

- **Every temporary result gets its own register, forever.** `x = x + 1`
  compiles to `CONSTANT r_a, 1` / `ADD_INT r_b, x, r_a` / `MOVE x, r_b` --
  three registers for one statement, none of them reusable even though
  `r_a`/`r_b` are dead the instant the MOVE runs.
- **A brand-new local costs a second register on top of its own
  expression's result.** `x = expr` evaluates `expr` into some register
  `value`, then `define_local` allocates a *second*, fresh register for
  `x` and emits `MOVE x, value` -- `value` is never touched again.

Measured directly (`make release`, this machine, alternating binaries
back-to-back per [[feedback_profiling_pitfalls]] to control for cold-start
noise): a straight-line function of N `total = total + 1`-shaped
statements needs roughly 3N registers, and once that crosses
`DIAMOND_INLINE_REGISTER_COUNT` (256, ~85 statements) every call also
heap-allocates its register file. A 1300-statement function (~3900
registers) took about 22.8 microseconds per call in a release build --
almost all of it the register file, not the arithmetic itself (an
equivalent function under 256 registers costs a small fraction of that).

## Why a general fix is a bigger project than it looks

The obvious fix -- recycle a temporary's register the instant its value is
consumed -- needs real liveness analysis: a CFG built from jump targets
and exception-handler edges, a def/use classification for (most of) the
176 opcodes this VM has, and care around every place a register's
identity matters beyond its value (see "Named locals and self" below).
Two paths were considered:

1. **A general post-pass** over already-emitted bytecode: build a CFG,
   compute liveness, recolor/coalesce registers. Architecturally the
   "right" answer, but it needs an audited def/use table for every opcode
   (only ~20 of 176 use one of disassemble.c's own generic
   `N_registers` shape helpers; the rest are bespoke) before it can even
   start, and every recycling decision has to be re-verified against
   named locals, `self`, closures and boxing (below). Not attempted:
   assessed as multi-session work on its own, and building the audit
   infrastructure without immediately wiring it into anything real risks
   losing the thread across sessions.
2. **Targeted compiler-level peepholes** for specific, well-understood
   shapes, each with its own narrow, provable safety argument, landed
   incrementally. Chosen for Stage 1 and recommended for Stage 2 (below):
   smaller, individually reviewable, and each stage's safety reasoning
   stays self-contained instead of depending on a large shared allocator
   being fully correct before any of it can ship.

## Named locals, self, and closures: the invariants a recycler must respect

Register 0 (an instance method's `self`) and every parameter are fixed
for the VM's whole calling convention (`run_chunk` copies
`arguments[i]` into `registers[i]`, `parameter_offset` accounts for
`self`) -- never renumber or recycle those. Two subtler invariants,
found by tracing actual codegen rather than assumed:

- **A captured local is boxed *in place*.** `BOX_LOCAL` turns whatever
  register a captured local already lives in into a `Cell`, at whatever
  point in the function first needs it -- the local's register number
  must stay the same for the rest of the function once that can happen,
  since every later read/write of it goes through that same slot.
- **`self` inside an ordinary method is *not* in `compiler->locals[]` at
  all.** Reading it (`parse_prefix`'s `DIAMOND_TOKEN_SELF` case) just
  returns the literal register 0, with no lookup through the named-local
  table -- that table only gets a `self` entry in the unrelated case of a
  nested closure capturing it from an enclosing method. A check that
  guards register reuse by scanning `compiler->locals[]` for a name match
  therefore *misses `self`* and will wrongly treat register 0 as a free
  temporary.

## Stage 1 (landed): reuse a new local's own RHS register

Scope: `x = <expression>` where `x` is being declared for the first time
(`compile_assignment_store`'s `local < 0` branch). If the expression's
result register is a genuine, freshly computed temporary -- not a bare
reference to something that already existed -- it simply *becomes* `x`'s
home instead of `define_local` allocating a second register and emitting
a `MOVE` into it. No opcode classification needed at all: this doesn't
care what produced the value, only that nothing else needs that specific
register once it's about to hold `x`.

Two independent guards must both hold:

- **`register_is_named`**: `value` isn't any existing local's, parameter's,
  or (when tracked) `self`'s register -- rules out `x = y`/`x = some_param`
  aliasing two variables onto one slot.
- **`value >= rhs_start_register`**, a register high-water mark captured
  by the caller immediately before the RHS started evaluating. Anything
  that existed before this statement began is, by this allocator's own
  strictly monotonic numbering, necessarily below that mark -- named in
  `compiler->locals[]` or not. This is what actually catches `self`.

**The bug this second guard was added for**: with `register_is_named`
alone, `v = self` inside a method passed the guard (self's register 0 has
no `compiler->locals[]` entry there), so `v` became register 0 directly.
Once `v` was later captured into a block and boxed, `BOX_LOCAL` converted
register 0 -- `self` itself -- into a `Cell` *in place*, and every later
`self.foo(...)` in that same method received the raw Cell as its receiver
instead of the instance. Caught by the existing test suite
(`tests/cases/active_record_batches.di`, `packages/arel`'s query
renderer) as `undefined method 'render_source' for Cell`; minimized to
`tests/cases/self_assigned_to_new_local_then_captured.di`, which
reproduces the exact crash with the guard reverted and is silent with it.

Every other call site of `compile_assignment_store` (`case`/pattern
bindings, destructuring targets, both `compile_compound_assignment`
branches) passes `compiler->next_register` *at that call* as
`rhs_start_register` -- a value that can never be `<= value`, so the
optimization is unconditionally disabled there. `compile_compound_assign
ment` can't reach `local < 0` at all (`x += y` requires `x` to already
exist), so this is a documentation-only no-op for it, kept for a uniform
contract rather than special-cased away.

**Measured impact** (60 chained fresh local declarations,
`v0 = seed + 1; v1 = v0 + 1; ...`, alternated binaries, 3 rounds): about
1200 ns/call before, about 900 ns/call after -- roughly 25% -- with the
compiled function's own `register_count` dropping from ~180 to ~122 for
that shape. Bounded by `DIAMOND_MAX_LOCALS` (64 distinct names per
function), unlike the *reassignment* case below, which has no such bound.

**Verified**: full suite (1659, including the two new regression tests),
all 17 examples, self-host smoke, the full self-host parser differential
corpus (253/253 positive; the 2 pre-existing `require_*_manifest` failures
are unrelated drift, see [[project_diamond_examples_batch]]), and
`make test-sanitize`, all clean.

## Stage 2 (planned, not implemented): reassigning an existing local

The pattern that actually motivated this investigation -- `total =
total + 1` in a loop, unboundedly repeated -- reassigns an *existing*
local, so Stage 1's approach doesn't apply: `destination` (the local's own
permanent register, fixed since its first declaration) is not something
a later statement can just relabel. The available fix there is different
in kind: **rewrite the producing instruction's own destination operand**
from the temporary to `destination` in place, delete the trailing `MOVE`,
and let `next_register` shrink back down -- safe only when:

- `value == compiler->next_register - 1` (it's the most recently
  allocated register; nothing higher still needs to survive).
- The most recently emitted instruction actually wrote `value` as its
  first operand (every value-producing opcode in this compiler writes its
  result there; confirmed via `emit_instruction`'s uniform 2-byte-operand
  encoding, `compiler->last_instruction_offset` would need to be added
  and updated in `emit_opcode` to make this checkable without unsafely
  scanning backward through variable-length bytecode).
- That instruction's own opcode is on an explicit, narrow allowlist,
  vetted opcode by opcode against `compiler->narrowing` and any other
  lasting per-register compiler state -- **not a blanket rule.** Already
  vetted: the plain arithmetic family compile_binary_op can emit (`ADD`,
  `SUBTRACT`, `MULTIPLY`, `DIVIDE`, `MODULO`, `SHIFT_LEFT`, `SHIFT_RIGHT`,
  `BITWISE_AND`/`_OR`/`_XOR`, and their `_INT` fast-path forms) --
  confirmed none of these touch `compiler->narrowing` (only `EQUAL`/
  `NOT_EQUAL`/`EQUAL_INT`/`NOT_EQUAL_INT` do, and only conditionally; the
  full comparison family is excluded from the allowlist for this reason,
  not because it's unsafe for some other reason not yet found).
- Given Stage 1's own near-miss: re-run the *same* register-provenance
  reasoning (does this register predate the statement, self included)
  before trusting any new allowlist entry, rather than assuming
  `register_is_named`-style name-table checks are complete.

## Stage 3+ (aspirational): the general post-pass

Still the eventual right answer for the cases Stage 1/2 don't reach
(reassignment via a non-arithmetic RHS, recycling across statement
boundaries generally) -- a real liveness-tracked allocator over the full
opcode set, as sketched in "Why a general fix is a bigger project than it
looks" above. Not scheduled; revisit once Stage 2 has shipped and this
document's own risk assessment has been through one more real landing.

Related: [[project_diamond_register_pool_gc_threshold_negative]] (the
*runtime* register-buffer pooling attempt this same investigation also
tried and reverted -- a different mechanism, aimed at the malloc/free
cost rather than the register-count itself), [[feedback_profiling_pitfalls]].
