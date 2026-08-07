# I/O

Diamond has no general I/O yet — no file access, no stdin, no sockets.
This document covers what exists today (stdout only) and will grow as
later slices land (see `docs/roadmap.md`).

## stdout: `print`/`puts`

```ruby
print("hello, ")
puts("world")
```

`print(value)` writes `value`'s stringified form to stdout with no
trailing newline; `puts(value)` writes it with one. Both take exactly one
argument and both return `nil`.

Stringification reuses the exact mechanism string interpolation (`#{}`)
and the `DIAMOND_OP_TO_STRING` opcode already use, via a shared
`stringify_value` helper in `src/vm.c`: a `String` value passes through
unchanged; an instance with a `to_s()` method has it called (and its
return value must be a `String`, or a rescuable `TypeError`); everything
else falls back to the same formatter used by string interpolation
(`nil`, `true`/`false`, integers, and `[...]`/`{...}` for arrays/hashes).
A `to_s` with the wrong arity raises a rescuable `ArgumentError`, exactly
as calling it any other way would.

`print`/`puts` are recognized in the compiler's `parse_name`, the same
place `Fiber.new` and `redefine_method` are — gated on the identifier not
already being a local or a user-defined function, so `def print(x) ...
end` shadows the builtin entirely (no reserved keyword). They compile to
a single `DIAMOND_OP_PRINT dest, source, newline` instruction; `newline`
is a compile-time-constant byte (`0` for `print`, `1` for `puts`), not a
runtime value.

## What's deliberately out of scope for this slice

- **stdin**: no way to read input yet.
- **Files**: no way to open, read, or write a file yet.
- **Sockets**: needed before the Rack-style web server idea on the
  roadmap is possible at all.
- **Multiple arguments**: `puts(a, b)` (Ruby-style, one line per
  argument) is not supported — exactly one argument, matching the
  narrowest useful slice.
- **Error handling for write failures**: a failed `fwrite`/`fputc` to
  stdout (e.g. a broken pipe) is not currently surfaced as a rescuable
  exception; this mirrors most languages' baseline `print`, but is a
  known simplification, not a deliberate design stance.

Each of these is a plausible next slice, sized independently rather than
attempted together.
