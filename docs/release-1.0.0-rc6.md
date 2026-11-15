# d6 1.0.0-rc6

The sixth release candidate for d6 1.0 makes a few everyday expressions mean
what they mean in Ruby, closes the last gaps in signal handling for servers,
and gives every published cut a documentation page.

## Ruby's answers

<!-- doc-check: skip -->
```d6
-2.5.floor()                 # => -3 (the minus belongs to the literal)
-2.abs()                     # => 2
4.35.floor(2)                # => 4.35 (was 4.34)
0.07.ceil(2)                 # => 0.07 (was 0.08)
"a\n\n".chomp("")            # => "a"
Hash.new(0).merge({"a": 1})  # missing keys still read as 0
counts = Hash.new(0)         # new: a Hash with a default
squares = Array.new(3) do |i| i * i end   # => [0, 1, 4]
```

`-2 ** 2` is still -4, and `- 2.abs()` (with a space) and `-x.abs()` still
negate the whole call. A struct's generated `==` and `to_s` now cover fields a
subclass or the struct's own body adds.

## Signals reach blocked servers

A trapped signal (`Signal.trap`) that arrives just before `IO.poll`,
`TCPServer#accept`, `UDPSocket#receive` or a blocking socket `read` starts
waiting now runs its handler during the wait. Before, JIT-compiled code could
miss it and wait out the whole timeout, or forever, so a server could ignore
SIGTERM. A signal that lands during a blocking socket `read` no longer raises
"Interrupted system call".

## Documentation for every cut

Each public cut has a page on the docs site, under
[All cuts](https://rolld6.codes/docs/cuts/index.html): its README plus an API
reference built from `##` doc comments in its source
([how to write them](packages.md#documenting-a-cut)). All 20 cuts get a patch
release carrying those comments; no cut's code changed.

## Other changes

- The site moved to [rolld6.codes](https://rolld6.codes/) and the registry to
  [cuts.rolld6.codes](https://cuts.rolld6.codes), which facet now uses by default; the former
  addresses still answer.
- The brand is written **d6** (lowercase) again, and the logo is the six face
  of a die.
- In the REPL, a blank or comment-only line no longer prints `nil`.

See the [changelog](../CHANGELOG.md) for the complete list. Release
preparation does not update deployed services; the
[production record](../applications/registry/deploy/PRODUCTION.md) records
actual deployments.
