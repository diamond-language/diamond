# Lifting the 180-class limit

Since 0.18.0 a Diamond program holds up to `DIAMOND_MAX_CLASSES` = 1,024 classes
and structs, **cuts included**. The following describes the original 180-class
limit and the migration that removed it.

Before the migration, a program held at most 180 classes and structs.
Past it, compilation stops with `too many classes`. A real application (Pheint, a GraphQL API on
`gremlin`, `graphql`, `mjolnir`, `arel`, `http` and `logger`) reaches the limit with ordinary code; it
only fits by turning its namespaces into `module`s (which do not count) and its tables into hash rows.
Modules are capped too (255), and have the same storage problem.

Modules and interfaces later moved to the same lazily allocated slot tables and are now capped at 1,024
each (`DIAMOND_MAX_MODULES`, `DIAMOND_MAX_INTERFACES`); the 255 and 32 figures below are historical.

This document is the plan for removing the limit. Stages 1-4 are implemented (the limit is now 1,024); stage 5 is
still optional.

## Why it was 180

Three independent things cap the count, and they all have to move:

1. **8-bit type ids.** A class is identified by `DIAMOND_TYPE_CLASS_BASE + class_index` in a `uint8_t`
   type id (`DiamondTypeId`). The byte also holds the primitives (0..9), `DIAMOND_TYPE_NATIVE` (190),
   generic variables (192..223) and interfaces (224..255). The class range is therefore 10..189.
   Type ids live in `known_types[]` (one per register), `DiamondTypeMember.id`, `DiamondFunction`
   parameter/return tables, `field_known_class[]`, `owner_class`, the JIT's `ivar_known_class[]` and
   `register_known_class[]`, and the LSP receiver analysis.
2. **8-bit class operands in bytecode.** `NEW`, `NEW_KEYWORDS`, `NEW_SPREAD`, `DEFINE_METHOD`,
   `REDEFINE_METHOD`, `COMPILE_METHOD`, the singleton-call family and `DIAMOND_VALUE_CLASS` read the
   class index with `READ_BYTE`/`emit_byte`. `DIAMOND_CLASS_FROM_SELF` is `0xFF`, and
   `DiamondFunction.owner_class` uses `0xFF`/`0xFD` as sentinels, so a class index may not reach them.
   `DiamondClass.superclass` and `DiamondValue.as.class_index` are `uint8_t` too.
3. **Fixed storage.** `DiamondProgram.classes[DIAMOND_MAX_CLASSES]` is an inline array of
   `DiamondClass`, each **63,672 bytes** (two 256-entry method tables, two 64-entry name tables, shapes,
   an inline 255-entry module index list). 180 of them are 11.5 MB of a 27.3 MB `DiamondProgram`;
   modules (255 x 57,744 bytes) are another 14.7 MB. `Compiler.classes[]` mirrors the array, and
   `DiamondVm.class_variables` is `calloc(MAX_CLASSES * MAX_FIELDS)` values on first use.

Runtime values do **not** depend on the id: a `DiamondInstance` holds a `const DiamondClass *`, so
raising the count needs no change to object layout or the GC.

Two costs make raising the constant alone unacceptable even if the ids were wider:

- `Thread.new` (`clone_program_from_chunk`) `memcpy`s the whole `classes[]` array, so every spawned
  thread and every `gremlin` worker pays 11.5 MB for classes it does not have. At 1,000 classes that
  would be 64 MB per thread.
- Every `.d6c` bytecode cache and the compiled-prelude snapshot is laid out around the fixed tables.

## Goals and non-goals

- Raise the limit to **1,024** classes at first, with the representation able to go to roughly 60,000 by
  changing one constant. (Interfaces stay at 32 and generic variables at 32; no program has needed more.)
- A program with few classes must get *cheaper*, not more expensive: no inline 11.5 MB array.
- No change to language semantics, `.d6` source, or the public cut API.
- Not a goal: lifting the module limit (255) or the 256-method-per-class limit. They share the storage
  rework, so the module table should move with the classes, but their ceilings stay put.

## Design

### 1. Widen type ids to 16 bits

`DiamondTypeId` becomes `uint16_t`. Primitives keep 0..9 and `CLASS_BASE` stays 10, so existing ids are
unchanged. The three bases above the class range move out of the way:

| range | ids |
| --- | --- |
| primitives | 0..9 |
| classes | 10..0xEFFF |
| native (one id; kind in `callable_arity`) | 0xF000 |
| generic variables | 0xF100..0xF11F |
| interfaces | 0xF200..0xF21F |

Every `uint8_t` that stores a type id or class index becomes `uint16_t`: `known_types`,
`DiamondTypeMember.id`, `field_known_class`, `discovered_field_known_class`, `owner_class`,
`superclass`, `range_class_index`, the JIT arrays, `DiamondValue.as.class_index`, `Compiler.current_class`
and the roughly dozen `(uint8_t)(DIAMOND_TYPE_CLASS_BASE + ...)` casts in `compiler.c`. The `static_assert`s become
`DIAMOND_TYPE_CLASS_BASE + DIAMOND_MAX_CLASSES <= DIAMOND_TYPE_NATIVE`. Sentinels move to `0xFFFF` and
`0xFFFD`; `DIAMOND_NATIVE_TYPE_OPERAND_BASE` moves above 0xFFFF's operand range.

Type ids are also emitted as single bytes in bytecode (type checks, union operands), so the
*operand* widening is part of stage 2. Stage 1 widens only the in-memory containers and keeps every value
and every emitted byte the same: the bases above stay where they are (the unknown sentinel stays 255), so
the bytecode of the whole test corpus must come out byte-identical. Raising the bases to the table above
happens together with the operand change in stage 2. Stage 1 landed with that check: 1,765 of 1,766
`--dump-bytecode` outputs identical, the remaining one differing only in program output.

### 2. Widen class operands in bytecode (format break)

Each class operand becomes a big-endian 16-bit value (`emit_function_index`'s convention): `emit_byte`
becomes 16-bit emission at the nine class-operand sites in `compiler.c`, and `READ_BYTE` becomes
`READ_SHORT` at the matching interpreter, JIT, disassembler and `selfhost/parser.d6` entries (a first
count is nine `READ_BYTE` class reads; stage 2 starts by enumerating them properly). Instruction length
changes, so `DIAMOND_CACHE_FORMAT_VERSION` is bumped and every `.d6c` is rejected and rebuilt (a cache
miss, not an error). The JIT's `READ_*` decoding and its inline-cache
site tables change with the operand width.

### 3. Dynamic class and module storage

`DiamondProgram.classes` becomes `DiamondClass **classes` plus `class_count`/`class_capacity`, exactly
like `functions`. Each `DiamondClass` is allocated on first use. `Compiler.classes[]` goes the same way, and
`DiamondVm.class_variables` is sized by `class_count`. Modules follow.

`clone_program_from_chunk` stops being a `memcpy`: it allocates and copies only `class_count` classes. A
program with 25 classes now clones 1.6 MB instead of 11.5 MB, so thread and `gremlin`-worker start-up
gets cheaper at today's sizes.

`DiamondClass` itself stays large in this stage (63,672 bytes), which caps the practical ceiling at
roughly a thousand classes (64 MB for a program that uses them all, per thread). If that proves too
heavy, stage 5 shrinks it.

### 4. Raise the constant and fix every `class_count == MAX` check

`DIAMOND_MAX_CLASSES` = 1,024. Sites that compare against it (`compiler.c:2851`,
`vm_program_builder.c:663`, `compiled_prelude.c:544`, `vm_internal.h:1279`) become checks against the
real capacity of a growable table. The compiled-prelude fingerprint already records `class_size` and
`max_classes`, so the snapshot is invalidated and rebuilt automatically.

### 5. (Optional) shrink `DiamondClass`

Methods, singleton methods, field names, class-variable names, shapes and the module include list become
growable arrays hung off the class, deep-copied by the thread clone and written count-first by the
serializers (they already are). A typical class has a handful of each, so `DiamondClass` drops from
63 KB to well under 1 KB and the ceiling becomes a constant, not memory. Do this only if stage 4's
measured footprint is a problem.

## Order of work and verification

Each stage is its own branch and PR, and each ends green on all 26 CI checks (including TSan, ASan/UBSan,
the macOS builds and the differential fuzzer) before the next starts.

1. Widen the type-id containers (values and bytecode unchanged). Verification: the full suite; a
   `--dump-bytecode` comparison of the corpus before and after (identical bytecode proves it changed
   nothing). Hand-check every `malloc`/`memcpy` sized in bytes for a type array: the compiler does not
   flag those.
2. Widen class operands in bytecode and move the id bases. (Only class operands needed widening: `IS_TYPE` and the
   type-set operands were already 16 bits. Every `UINT8_MAX` that meant "no class", "no owner" or "unknown type"
   became a named 16-bit sentinel; one missed (the built-in `Exception`'s missing superclass) made every
   superclass walk loop forever.) Verification: suite; `.d6c` rejected by version mismatch and rebuilt; the
   differential fuzzer with class-heavy seeds; JIT and interpreter agree on a program that builds
   instances of 150+ classes.
3. Dynamic storage. Verification: suite; a `Thread.new` start-up benchmark before/after; peak RSS of a
   small program before/after; ASan on `ProgramBuilder` (which adopts foreign programs) and on
   `Thread.new` with 20 classes live.
4. Raise the limit. Verification: a generated program with 1,000 classes plus a 300-class
   cut-shaped dependency chain, run under the interpreter, the JIT, a `Thread` and a `gremlin` worker;
   the `too many classes` message tested at exactly 1,025.
5. Only if needed, shrink `DiamondClass`, with the same memory and clone benchmarks.

Pheint is the acceptance test: after stage 4 its `build_row` hash-row workarounds and the module
conversions can be reverted where the original class form is clearer, and a new feature must be able
to add classes without checking a budget.

## Risks

- **Silent truncation.** Any missed `uint8_t` becomes a wrong class id above 255, not a crash. Stage 1
  must therefore be done by changing the typedefs first and following the compiler's `-Wconversion`
  warnings (the build already uses `-Wconversion`), then grepping for `(uint8_t)` near `CLASS_BASE`.
- **Inline caches and the JIT.** Both key on class identity and have fixed-width site tables
  (`invoke_site_known_class_count`, the 4-way set-associative cache). They need widening in stage 1 or 2.
- **`ProgramBuilder` and foreign programs.** An instance that crosses a `ProgramBuilder#run` boundary
  resolves its class against an adopted program, so stage 3 must keep class pointers stable
  (individually allocated classes are, a reallocated inline array would not be).
- **The self-hosted parser** keeps its own opcode table (`selfhost/parser.d6`) and must change in step
  with stage 2.
- **Third-party `.d6c` files** are rebuilt on version mismatch; nothing is shipped as bytecode.

## Open questions

- Is 1,024 the right first limit, or should stage 4 wait for stage 5 and go straight to 4,096?
- Should interfaces and generic variables also be lifted while the type-id space is being redone? They
  fit in 32 slots today, and 16-bit ids make it free to widen later, so this plan leaves them alone.
