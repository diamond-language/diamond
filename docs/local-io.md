# Local I/O

[I/O and native services](io.md) · Next: [Networking and signals](networking.md)

This guide covers standard streams, files, and path manipulation. See the
[I/O index](io.md) for networking, databases, time, and processes.

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

`puts` (`newline=1`) flushes stdout after writing; `print` (`newline=0`)
does not. stdout is fully buffered, not line-buffered, once it isn't a
terminal — redirected to a file, a pipe, whatever a test harness or
`> log` capture uses — so without this, a `puts("ready")` written right
before a program blocks in a native call (`.accept()`, `IO.poll`, a
`UDPSocket#receive` loop) could sit in the buffer indefinitely: nothing
forces a flush until the buffer fills or the process exits, and a
process blocked waiting for someone to *see* its own "ready" line is
exactly the case that never reaches either. Found this the hard way
while building the signals test below (see its own section) — a
"server prints ready, test harness polls the captured output for that
line" pattern already used throughout `tests/run.sh` and the
`packages/*` test scripts, which happened to keep working anyway
wherever the thing being waited on (`bind`, mostly) completes fast
enough that the *lack* of an early flush didn't matter, until it did.
`print` stays fully buffered — matching ordinary line-buffered-on-a-
terminal behavior unconditionally (rather than only when `isatty()`)
only for the newline-terminated case, so building up a line
incrementally via repeated `print` calls doesn't pay a flush cost per
fragment.

A write failure — most commonly `EPIPE` from a reader that closed its end
of a pipe — raises a rescuable `IOError` (`"write error: %s"` with
`strerror(errno)`), the same convention `File#write` already uses,
instead of being silently dropped or killing the process outright.
`diamond_vm_init` ignores `SIGPIPE` process-wide (not just for the TLS
write path elsewhere in this document), so a write past a closed reader
always surfaces as a plain, catchable error:

```ruby
begin
  loop do
    puts("line")
  end
rescue e: IOError
  # the reader went away
end
```

## stdin: `gets()`

```ruby
name = gets()
puts("hi #{name}")
```

`gets()` takes no arguments and reads one line from stdin, returning it
as a `String` with the trailing line ending stripped — both `\n` and
`\r\n` are handled, and stripping happens on the accumulated line rather
than per underlying read, so it's correct regardless of where an
internal buffer boundary happens to fall relative to the ending. Returns
`nil` only when zero bytes were read before EOF; a final line with no
trailing newline still returns its content, matching Ruby's `gets`.
Compiles to a single `DIAMOND_OP_GETS dest` instruction. Recognized with
the same shadowing precedent as `print`/`puts`.

Line length is not capped — reading grows a buffer across as many
underlying `fgets` calls as a line needs, the same growable-buffer
pattern (`StringBuilder`) already used for value formatting elsewhere
in `src/vm.c`.

## Files: `File.open`/`.read`/`.gets`/`.write`/`.close`

```ruby
f = File.open("data.txt", "w")
f.write("hello, ")
f.write("world")
f.close()

g = File.open("data.txt", "r")
g.read()   # => "hello, world"
g.close()
```

`File.open(path, mode)` opens a file via the C `fopen(path, mode)`
convention directly — `mode` is passed through unvalidated (`"r"`,
`"w"`, `"a"`, `"r+"`, and so on all work exactly as they would in C; an
invalid mode fails the same way a missing path does). A failed open
raises a rescuable `IOError` with `strerror(errno)` in the message, the
same phrasing the CLI's own `require`/file-loading errors already use
(`cannot open '<path>': <reason>`).

A `File` value is a new GC-managed heap object kind
(`DIAMOND_OBJECT_FILE`), a thin wrapper around a `FILE *` — the same
shape as `Fiber`'s `DiamondFiberHandle` around a `DiamondFiber *`.
Unlike `Fiber`, nothing inside a `DiamondFileHandle` references another
Diamond value, so `mark_object` needs no dedicated branch for it; sweeping
an unreached handle whose stream is still open calls `fclose` on it as a
safety net, the same role sweep-time cleanup plays for an unclosed
`Fiber`'s native stack.

`File.open` is recognized in the compiler the same way `Fiber.new` is
(shadowable by a local or a top-level function of the same name),
compiling to a single
`DIAMOND_OP_FILE_OPEN dest, path, mode` instruction. `.read()`/`.gets()`/
`.write(value)`/`.close()` are native `DIAMOND_OP_INVOKE` dispatch on a
`DIAMOND_OBJECT_FILE` receiver, the same mechanism `Fiber`'s `.resume`/
`.status`/`.alive?` use:

- `.read()` reads all remaining bytes from the current position to EOF
  as one `String`; `.read(n)` reads up to `n` bytes and stops instead —
  needed for reading a fixed-size chunk (e.g. an HTTP request body of
  known `Content-Length`, as the `packages/http` package does) without
  also blocking on or consuming whatever the other side sends next on a
  still-open connection.
- `.gets()` reads one line, sharing the exact `read_line` helper stdin's
  global `gets()` uses (same EOF/`nil`, CRLF-stripping, and no-line-
  length-cap behavior).
- `.write(value)` stringifies `value` via `stringify_value` (same as
  `print`) and writes it.
- `.close()` is idempotent — closing an already-closed handle is a no-op,
  not an error.

Any operation other than `.close()` on an already-closed handle, or a
genuine read/write failure (checked via `ferror`, not just a short
return value), raises a rescuable `IOError`.

## File paths: `File.join`/`.dirname`/`.basename`/`.extname`/`.absolute?`/`.expand_path`

```ruby
File.join("a", "b", "c")             # => "a/b/c"
File.join("a/", "/b/", "c")          # => "a/b/c" -- redundant separators collapsed
File.dirname("/a/b/c")               # => "/a/b"
File.basename("/a/b/c.rb")           # => "c.rb"
File.basename("/a/b/c.rb", ".rb")    # => "c"
File.extname("archive.tar.gz")       # => ".gz"
File.absolute?("/a/b")               # => true
File.absolute?("a/b")                # => false
File.expand_path("../b", "/a/x")     # => "/b"
File.expand_path("relative/path")    # => cwd + "/relative/path"
```

Pure String manipulation — none of these touch the filesystem or
require `path` to actually exist, except `.expand_path` calling
`getcwd()` when resolving a relative `path` with no `base` (or a
relative `base`) given. All six are recognized in the compiler the same
way `File.open`/`Fiber.new` are (shadowable by a local or a top-level
function of the same name); `.join`'s variable-length argument list
compiles to `DIAMOND_OP_FILE_JOIN dest, base, count` (the same
contiguous-register-run shape `Thread.new`'s own argument list uses),
and the other five share one `DIAMOND_OP_FILE_PATH dest, arg1, arg2,
selector` instruction (the `DIAMOND_OP_MATH_UNARY`/`_BINARY` "one opcode
+ a selector byte" shape, reused here for `.dirname`/`.extname`, taking
one argument, and `.basename`/`.expand_path`, taking an optional
second).

- `.join(*parts)` skips empty-string parts entirely and collapses a
  redundant `/` at each seam — not bug-for-bug identical to Ruby's own
  `File.join` (which treats a leading `""` part as still contributing a
  separator), a deliberately simpler and more predictable rule instead.
- `.dirname(path)` / `.basename(path, suffix = nil)` / `.extname(path)`
  follow Ruby's own rules: no separator at all → `"."` for `dirname`;
  a path made entirely of separators (`"/"`) stays `"/"`, never reduced
  to `""`; a dotfile's own leading dot(s) never start an `extname`
  (`".bashrc"` → `""`). `suffix` is stripped from `basename` only on an
  exact literal match (no Ruby-style `".*"` wildcard support).
- `.absolute?(path)` is `path` starting with `/` — no drive-letter or
  UNC handling, since this VM only targets POSIX platforms.
- `.expand_path(path, base = nil)` resolves `path` to an absolute,
  lexically-normalized path: an absolute `path` is normalized as-is
  (`base` is then irrelevant); otherwise `path` is joined onto `base`
  (itself resolved against the current working directory first if
  `base` is relative) or directly onto the current working directory
  when `base` is nil. Normalization then walks the combined path
  dropping empty and `"."` segments and popping the previous real
  segment on `".."` — kept literally only when there is nothing left to
  pop, so this can never climb above the root, matching Ruby.
