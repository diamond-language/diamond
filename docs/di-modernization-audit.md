# Diamond Code Modernization Audit

This is a maintainability audit of the current Diamond source, not a behavior
change. It records where older naming and syntax can be modernized now, and
where the existing spelling is intentional.

## Summary

The largest naming smell is the collection helper family in `lib/core.di`:
`array_*`, `hash_*`, and `enumerable_*`. These are mostly receiver operations
that should eventually live on `Array`, `Hash`, or `Enumerable` as ordinary
methods. The migration should be staged because the current compiler resolves
bare top-level calls in source order and the core prelude is still assembled
from ordered embedded modules.

The rest of the prefixed functions fall into two different categories:
application/domain helpers, which should be renamed locally as their owning
application grows, and infrastructure functions whose prefix communicates a
real boundary and should remain.

## Status

Array and Hash receiver methods are done. The mechanism was already in place:
`vm.c`'s `INVOKE` handling for `Array`/`Hash` receivers keeps a name-forwarding
table (around `vm.c`'s `receiver_kind==DIAMOND_OBJECT_ARRAY||...HASH` branch)
that maps a method name straight to the matching `lib/core.di` free function
via `find_top_level_function`, so most candidates in this doc (`map`,
`select`, `sum`, `reject`, `find`, `each_with_index`, `sort_by`, `min_by`/
`max_by`, `take`, `drop`, `flat_map`, `partition`, `group_by`, `zip`,
`each_slice`, `each_cons`, `tally`, `join`) were already wired before this
audit. What was missing has been added the same way: Array gets `first`,
`first_or`, `last`, `last_or`, `empty?`, `include?`, `reverse`, `concat`,
`compact`, `uniq`, `flatten`, `delete_at`; Hash gets `fetch`, `empty?`,
`keys`, `values`, `include_key?`, `map_values`, `merge`. The free functions
are unchanged and remain the compatibility-wrapper implementations the
receiver methods forward to. Internal `lib/core.di` and package call sites
(`packages/rack`, `packages/gremlin`, `packages/arel`) were migrated to the
new receiver syntax, including the `unless`/`include?` rewrite this doc's
own "Truthiness and `unless`" section uses as its worked example — that
example is now the real `array_compact`/`array_uniq` source, not a
hypothetical. `array_map_int`, `array_map_string`, `array_map_typed`, and
`array_sort` were deliberately left as free functions per the caveats
below (typed contracts, not drop-in receiver renames). Enumerable is next.

Compound assignment (step 4) and the `unless`/truthiness rewrite (step 5)
have landed in `packages/arel/lib/arel.di`, and now also in `lib/core.di`,
`lib/core/numeric.di`, `lib/core/json_codec.di`, `lib/minitest.di`, and
`packages/gremlin/gremlin.di`. Both passes were applied out of the
Recommended Order below (ahead of Enumerable) because they're purely local,
behavior-preserving syntax edits with no call-site or compilation-order
implications, unlike the receiver-method migrations. Two categories were
deliberately left alone rather than rewritten:

- string-concatenation accumulators such as `result = result + ch` in
  `lib/core/json_codec.di`, matching the same exclusion already applied in
  `arel.di` (a separate StringBuilder/O(n^2) concern, not a syntax question);
- `if callback(...) == false` guards in `lib/core.di`'s `enumerable_all` and
  `array_reject` (around lines 334, 341, 397), where `callback` is an
  untyped, caller-supplied `Callable[1]`. Unlike `array_include`'s
  guaranteed-`Bool` return in this doc's own worked example, a predicate
  that returns `nil` instead of `false` would change behavior under
  `unless callback(...)` (nil is falsy) but not under `== false` (nil isn't
  `false`) — the same nil-vs-false trap this doc's "Truthiness and `unless`"
  section already warns about, just via `== false` instead of `!= nil`.
  `gremlin.di`'s `@eof == false` inside a compound `while` condition was
  left for the same reason it's out of the documented if-guard→`unless`
  pattern in the first place: it's a `while` condition, not a single-branch
  `if` guard.

`packages/http/http.di` has also been swept: 6 of its 7 `if x != nil`-shaped
guards converted to `unless`; the 7th (`http_request`'s Content-Length check,
around line 220) has an `else` branch and was left as `if` for the same
elsif/else reason as `arel.di`'s remaining cases. A repo-wide re-scan after
this pass found no further unconverted compound-assignment or single-branch
nil-guard candidates in `lib/` or `packages/`.

Enumerable (step 3) turned out to need less than this doc originally
assumed, and something different from what it assumed: `vm.c`'s native
`INVOKE` dispatch already forwards `select`/`count`/`any?`/`all?`/`map`/
`reduce` (Array and Hash) and `sort`/`sort_by`/`min`/`max` (Array only)
straight to the matching `enumerable_*` function, with no leftover direct
free-function call sites to migrate -- that part predates this audit. The
actual gap was `module Enumerable` itself (the mixin `Range` and any other
`each`-implementing class gets via `include Enumerable`): it only defined
six methods, missing `sort`/`sort_by`/`min`/`max`/`min_by`/`max_by` and the
rest of Array's Enumerable-style surface entirely -- a gap `docs/syntax.md`
already named as "the existing asymmetry" before this pass closed it.
`module Enumerable` now also has `to_a` (materializes the receiver via
`self.each(...)`) plus `sort`, `sort_by`, `min`, `max`, `min_by`, `max_by`,
`reject`, `find`, `each_with_index`, `sum`, `take`, `drop`, `flat_map`,
`partition`, `group_by`, `zip`, `each_slice`, `each_cons`, and `tally`,
each delegating to the existing Array-typed `array_*`/`enumerable_*`
function on that materialized copy rather than re-deriving index-based
logic generically (see `docs/design.md`'s Enumerable section for the
full reasoning). Verified against both `Range` and a custom `each`-only
class, plus the pre-existing `legacy_0362`/`0363`/`0364` Enumerable
fixtures. `docs/syntax.md` and `docs/design.md` updated to match.

Step 6 (removing compatibility wrappers) turned out to be far narrower
than its "one deliberate cleanup release" framing implied, once checked
against how receiver dispatch actually works: `vm.c`'s `DIAMOND_OP_INVOKE`
resolves `values.map(cb)`/`hash.fetch(...)`/etc. by looking up a
same-named top-level prelude function (`find_top_level_function`) at
runtime and forwarding to it -- roughly 38 of the `array_*`/`hash_*`/
`enumerable_*` functions across this doc's candidate lists *are* that
lookup's real target, not a redundant layer in front of one. Deleting any
of them breaks the receiver method itself for every `Array`/`Hash` in the
language; the doc's own "candidates" framing assumed they'd become
genuinely removable once receiver syntax existed, which isn't how the
mechanism works. The one function that actually fit the "redundant
wrapper" description was `array_join` (`values.join(separator)`, forwarding
to the already fully-native, non-table-dispatched `.join()`, per
`src/vm.c`'s `array_join_helper` comment) -- it has been deleted from
`lib/core.di`. `tests/cases/array_join_method.di` (which exercised both
`.join()` and the free-function form) and `tests/cases/legacy_0440`
through `legacy_0443` (which existed solely to test the free-function
form) were updated to `values.join(...)` receiver syntax, preserving their
coverage (including `legacy_0443`'s `nil`/`Bool` element stringification
case) with matching `.expected` output confirmed against the built binary.
`docs/syntax.md` and this doc's own "Array candidates" section updated to
match; the rest of the `array_*`/`hash_*`/`enumerable_*` free functions
stay exactly as they are -- true removal would need a native-dispatch
rework (hardcoding these directly in `vm.c` instead of runtime name
lookup), which is out of scope for a modernization pass and not attempted
here.

## High-Value Receiver Migrations

### Array candidates

These functions have an Array receiver as their first argument and are strong
candidates for methods:

- `array_first`, `array_first_or`, `array_last`, `array_last_or`
- `array_empty`, `array_include`, `array_each`, `array_map`
- `array_map_int`, `array_map_string`, `array_map_typed`
- `array_reverse`, `array_concat`, `array_compact`, `array_uniq`
- `array_flatten`, `array_delete_at`, `array_sum`
- `array_reject`, `array_find`, `array_each_with_index`
- `array_min_by`, `array_max_by`, `array_take`, `array_drop`
- `array_flat_map`, `array_partition`, `array_group_by`, `array_zip`
- `array_each_slice`, `array_each_cons`, `array_tally`

The intended end state is familiar receiver syntax such as:

```diamond
values.map(callback)
values.flatten()
values.each_slice(2)
values.group_by(callback)
```

`array_join` was removed (see Status above): `Array#join` is already native
and the free function was only a compatibility wrapper, unlike the rest of
this list, whose free functions remain vm.c's actual dispatch targets.
`array_sort` in `lib/core/numeric.di` is also a special case: it is typed
specifically for
`Array[Int]` and should either become an Array method with a deliberate typed
contract or be replaced by the existing native sorting surface, not blindly
renamed.

### Hash candidates

These functions have a Hash receiver and should become Hash methods:

- `hash_fetch`
- `hash_empty`
- `hash_each`
- `hash_keys`
- `hash_values`
- `hash_include_key`
- `hash_map_values`
- `hash_merge`

Likely target forms:

```diamond
options.fetch("port", 8080)
options.keys()
options.values()
options.merge(overrides)
```

`hash_find` appears in the broader code inventory and should be checked against
native Hash behavior before adding another method; do not create two subtly
different lookup contracts.

### Enumerable candidates

These are protocol operations and belong on `Enumerable`, or on the concrete
receiver if the runtime already provides a native fast path:

- `enumerable_select`, `enumerable_count`, `enumerable_any`, `enumerable_all`
- `enumerable_map`, `enumerable_reduce`
- `enumerable_sort`, `enumerable_sort_by`
- `enumerable_min`, `enumerable_max`

The current code already uses receiver methods such as `map`, `select`,
`sort_by`, `group_by`, `each_with_index`, `each_slice`, `each_cons`, and
`flat_map` in places. Those call sites are migration exemplars. Before moving
an `enumerable_*` helper, confirm whether the operation is meant to work on
Array, Hash, Range, or an arbitrary object implementing `each`.

## Names That Should Stay Prefixed

These prefixes communicate ownership rather than old syntax:

- `emit_*`, `patch_*`, `allocate_*`, and compiler/VM helpers: internal C or
  compiler implementation procedures, not Diamond object methods.
- `render_*` on visitors and statement nodes: renderer protocol hooks; moving
  these to generic methods would blur dialect boundaries.
- `parse_*` and `compile_*` in parser/compiler code: phase-specific internal
  operations.
- `http_*` and `rack_*`: package-level public functions with deliberately
  server/framework-oriented names.
- `integer_times`, `integer_upto`, `integer_downto`: these are compatibility
  forwarding helpers for native integer dispatch. They could eventually be
  ordinary Integer methods, but the native dispatch contract should be changed
  and tested first.
- Application helpers such as `author_by_id`, `book_by_id`,
  `author_delete_handler`, and `book_write_handler`: these are readable in a
  small application and should become model/repository methods only when the
  ActiveRecord layer owns them.

## Fuller Syntax Opportunities

### Compound assignment

Many loops still use the older spelling:

```diamond
index = index + 1
index = index - 1
total = total + value
```

Use the supported forms where the target is a local or instance variable:

```diamond
index += 1
index -= 1
total += value
```

Indexed compound assignment is not supported, so keep explicit read/modify/
write code for forms such as `values[index] = values[index] + 1`.

### Truthiness and `unless`

Patterns like these can be shortened when the semantics are unchanged:

```diamond
if item != nil
  result.push(item)
end

if array_include(result, item) == false
  result.push(item)
end
```

Prefer receiver methods and truthiness where appropriate:

```diamond
unless item == nil
  result.push(item)
end

unless result.include?(item)
  result.push(item)
end
```

Do not blindly rewrite `value != nil` to `if value`: `false` and `nil` are
both falsy, while the comparison only excludes `nil`.

### Existing receiver syntax

Prefer the fuller method surface already used by the codebase:

- `values.map(callback)` over `array_map(values, callback)`
- `values.select(callback)` over `enumerable_select(values, callback)`
- `values.sort_by(callback)` over `enumerable_sort_by(values, callback)`
- `values.each_with_index(callback)` over `array_each_with_index(values, callback)`
- `values.group_by(callback)` over `array_group_by(values, callback)`
- `values.join(separator)` over `array_join(values, separator)`
- `hash.keys()`/`hash.values()` where the native methods provide the needed
  contract

### Explicit query and persistence syntax

The Arel and `active_record` code is already aligned with the newer
explicit style: intermediate statements make line-oriented chaining clear,
repositories receive tables/mappers/keys explicitly, and associations receive
foreign keys explicitly. Keep that style. Do not introduce DSL magic, schema
inspection, or `method_missing` merely to make the code look more Rails-like.

## Recommended Order

1. Add or confirm receiver methods for one Array family at a time, preserving
   the free functions as compatibility wrappers.
2. Migrate internal `lib/core*.di` call sites to receiver syntax and add focused
   behavior tests.
3. Done for Array and Hash (see Status above). Repeat for Enumerable after
   resolving cross-receiver contracts.
4. Modernize mechanical local counter updates to compound assignment.
5. Apply `unless` and truthiness rewrites only where nil-versus-false semantics
   are explicit.
6. Remove compatibility wrappers only in a deliberate language/library
   cleanup release -- done for `array_join`, the one genuinely redundant
   wrapper found; see Status above for why the rest of this doc's
   `array_*`/`hash_*`/`enumerable_*` candidates aren't actually removable
   without a native-dispatch rework.

The key constraint is source-order compilation: method additions and call-site
migration should land in dependency order, with the full native and REPL suites
run after each slice.
