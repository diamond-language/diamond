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

`array_join` is lower priority because `Array#join` is already native and the
free function is only a compatibility wrapper. `array_sort` in
`lib/core/numeric.di` is also a special case: it is typed specifically for
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

The Arel and `diamond-active_record` code is already aligned with the newer
explicit style: intermediate statements make line-oriented chaining clear,
repositories receive tables/mappers/keys explicitly, and associations receive
foreign keys explicitly. Keep that style. Do not introduce DSL magic, schema
inspection, or `method_missing` merely to make the code look more Rails-like.

## Recommended Order

1. Add or confirm receiver methods for one Array family at a time, preserving
   the free functions as compatibility wrappers.
2. Migrate internal `lib/core*.di` call sites to receiver syntax and add focused
   behavior tests.
3. Repeat for Hash, then Enumerable after resolving cross-receiver contracts.
4. Modernize mechanical local counter updates to compound assignment.
5. Apply `unless` and truthiness rewrites only where nil-versus-false semantics
   are explicit.
6. Remove compatibility wrappers only in a deliberate language/library
   cleanup release.

The key constraint is source-order compilation: method additions and call-site
migration should land in dependency order, with the full native and REPL suites
run after each slice.
