# D6 1.0.0-rc5

The fifth release candidate for D6 1.0 fills in the argument shapes everyday
collection and String code reaches for, and makes JIT-compiled code
substantially faster.

## Everyday conveniences

Calls that used to stop with an arity or undefined-method error now work:

<!-- doc-check: skip -->
```d6
[nil, 1].any?()                          # => true, no block needed
[1, 2, 3, 4, 5].each_slice(2) do |pair| puts(pair) end
[1, 2, 3].sum(0.0)                       # => 6.0
[5, 3, 9, 1].min(2)                      # => [1, 3]
["apple", "avocado", "kiwi"].uniq() do |s| s[0] end
[1, [2, [3]]].flatten(1)                 # => [1, 2, [3]]
counts.merge(more) do |key, old, new| old + new end
"report.csv".chomp(".csv")               # => "report"
3.14159.floor(2)                         # => 3.14
```

Also new: `take_while`, `drop_while`, `slice_when` and `intersect?` on Array,
`zip` over several arrays, Hash `values_at`, `fetch_values`, `first`,
`delete_if` and `keep_if`, Hash `each_with_object` blocks that take
`(key, value, memo)`, `Int#step(limit, by)`, and String `squeeze(chars)`,
`index(needle, start)`, `insert` and `prepend`. The
[collections guide](collections.md#prelude-methods) lists them all.

## A faster JIT

With the JIT on (`DIAMOND_JIT=1`):

- Compiled code writes values as whole 8-byte words and copies them in 8-byte
  halves, so the loads that read them back no longer wait for the stores to
  retire, and a comparison followed by a branch on its result branches
  directly. fib takes half the cycles it did in rc4, mandelbrot a third less,
  poly a quarter less.
- Array `select`, `each_with_index` and `reduce` and Int `times` run their
  blocks as native loops, as `each` and `map` already did, and `map` and
  `select` push into their results without calling C per element.

On the local D6-versus-Ruby benchmark pairs, D6 with its JIT is now at least
even with Ruby's YJIT on all 21. These are workstation measurements; the
interpreter, the default, is unchanged by this work.

## Other changes

- A thread that fails and is never joined prints its uncaught exception and
  backtrace on stderr instead of losing it.
- Session cookies are marked `Secure` under `DIAMOND_ENV=production` (cookies
  cut 0.2.4, published 2026-11-12).
- The language is written **D6** throughout the guides, CLI help and release
  titles.

See the [changelog](../CHANGELOG.md) for the complete list. Release
preparation does not update deployed services; the
[production record](../applications/registry/deploy/PRODUCTION.md) records
actual deployments.
