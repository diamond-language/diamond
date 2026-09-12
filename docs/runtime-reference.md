# Runtime features and debugging

[Language reference](syntax.md) · Previous: [Collections and Enumerable](collections.md)

## Fibers

```ruby
f = Fiber.new(callable)
f.resume(0)      # runs/resumes; returns the yielded or completed value
f.status()       # "runnable" / "suspended" / "completed" / "failed"
f.alive?()
```

Inside a callable body that does not declare an `&block` parameter,
`yield(value)` suspends and sends `value` out
to whoever resumes; the next `.resume(v)` delivers `v` back in as
`yield`'s own expression result. `Fiber.new`'s argument must be a
zero-argument callable (captures are fine — only nested `def`s produce a
referenceable one; see [Functions, closures, and calls](callables.md)).
`Fiber.yield(value)` is the explicit equivalent and remains unambiguous inside
a callable that also declares `&block`; `Fiber.yield()` sends `nil`.

## I/O

```ruby
puts("hello, #{name}")     # print(value) has no trailing newline
line = gets()               # one line from stdin, nil at EOF

f = File.open("data.txt", "w")
f.write("some text")
f.close()

server = TCPServer.listen(8080)
conn = server.accept()      # blocks until a client connects
conn.gets()
conn.write("response\n")
conn.close()

client = TCPSocket.connect("example.com", 8080)
```

A connected socket (from `.connect` or `.accept()`) is a `File` under the
hood, so `.read()`/`.read(n)`/`.gets()`/`.write(value)`/`.close()` work
identically on both.

`File` also has a small family of path utilities needing no open handle:
`File.join("a", "b")` (`=> "a/b"`), `.dirname(path)`, `.basename(path,
suffix = nil)`, `.extname(path)`, `.absolute?(path)`, and
`.expand_path(path, base = nil)` (resolves and lexically normalizes
`path` against `base`, or the current working directory when `base` is
omitted) are all pure string manipulation, needing no filesystem access.
`.directory?(path)` is the one exception — real `stat()`-backed I/O, `true`
only if `path` exists and is a directory; any `stat()` failure (missing
path, permission denied, ...) reads as an ordinary `false` rather than
raising, matching Ruby's `File.directory?` so a recursive directory walk
can use it in a plain condition with nothing to rescue. See [Local
I/O](local-io.md#file-paths-filejoindirnamebasenameextnameabsoluteexpandpath)
for the full rules.

`ARGV` and `ENV` are plain global values, not calls — `ARGV` is an
`Array` of `String`s, the script's own trailing command-line arguments
(`diamond script.di one two` → `ARGV == ["one", "two"]`; `[]` for `-e`/a
file run with no trailing args, or from the REPL). `ENV` is a `Hash` of
`String` to `String`, a snapshot of the process environment taken at
startup — `ENV["PATH"]`, `ENV["HOME"]`, etc.; a missing key is `nil`,
same as any other `Hash`. Mutating the `ENV` `Hash` only changes that
in-memory snapshot, not the real environment (no `setenv` round-trip) —
read-only in effect, even though nothing stops the write syntax itself.
Like every other built-in name, a local variable or user-defined
function named `ARGV`/`ENV` shadows it.

`exit(code = 0)` immediately terminates the whole process with the given
status (0–255; anything else raises `ArgumentError`, a non-`Int` raises
`TypeError`). This is a *hard* exit, not a raised/rescuable control-flow
value like Ruby's plain `exit` — no `ensure` block anywhere on the call
stack runs, and every other `Thread.new`-spawned OS thread stops too,
since they all share this one process. A validation failure (bad code)
is an ordinary catchable exception; only a valid code actually exits:

```ruby
begin
  exit(-1)
rescue error: ArgumentError
  puts("bad exit code: #{error.message()}")
end
exit(1)   # this one actually terminates the process
```

Like every other built-in name, a local variable or user-defined
function named `exit` shadows it.

## Debugging

```ruby
def compute(x)
  y = x * 2
  debugger()   # breakpoint() is the same thing, either name works
  y + 1
end
compute(5)
```

```
--- paused at compute:3:3 ---
locals:
  x = 5
  y = 10
(press Enter to continue)
```

`debugger()`/`breakpoint()` pauses execution, prints where it was called
from and every currently-live local (name and value — parameters count
as locals too), then waits for one line of input on stdin before
resuming normally. **Read-only inspection, not a live REPL**: there is no
way to evaluate a new expression or reassign a local from the pause — it
prints what's already there and continues, deliberately scoped short of
a Ruby `binding.pry`/`debug`-style interactive session. Stdin at EOF
(closed, redirected from `/dev/null` — the ordinary case under a non-
interactive script or test run) continues immediately rather than
hanging, so it's always safe to leave a `debugger()` call in code that
might run non-interactively.

Like `puts`/`gets`/`Time`/every other built-in name, a local variable or
a user-defined function named `debugger`/`breakpoint` shadows it —
`def debugger(); ...; end` makes `debugger()` call that instead, never
the built-in.

An editor's own gutter breakpoints reuse this exact same pause, inserted
at compile time rather than written into the source — see
[Debugging](debugging.md) for the DAP integration (`dap/`,
`editors/vscode`'s "Debugging" section), its limitations, and the
`DIAMOND_DEBUG_FD`/`DIAMOND_DEBUG_BREAKPOINTS` env-var contract behind it.

## Regexp

```ruby
re = Regexp.new("(\\d+)-(\\d+)")
m = re.match("id:42-99")   # => ["42-99", "42", "99"]
m[0]                        # full match
m[1]                        # first capture group

re.match?("id:42-99")       # => true, no captures allocated
re.match("no digits")       # => nil, no match

Regexp.new("foo", 1)        # 1 = case-insensitive (see options below)
```

Backed by `reginold`, a companion regex engine vendored in-repo under
`reginold/` and built as a static archive — compiled under Ruby regex
syntax. `Regexp.new(pattern, options = 0)` —
the options argument is a plain `Int` bitmask: `1` = ignore case, `2` = `.`
matches newline, `4` = extended (whitespace and `#` comments ignored in
the pattern). Diamond has no bitwise-OR operator, so combine flags by
adding them (they're disjoint bits — addition and OR coincide): `3` for
case-insensitive *and* dot-matches-newline together.

`.match(string)` returns an `Array` — index `0` is the full match, indices
`1..` are capture groups in order, `nil` at any index for an unmatched
optional group (`(a)|(b)` matched against `"b"` gives
`["b", nil, "b"]`) — or `nil` if the pattern didn't match at all.
`.match?(string)` is the same search without allocating capture data, for
a plain yes/no check. An invalid pattern raises a rescuable `RegexpError`
at `Regexp.new` time.

This is deliberately a small first cut: no `/pattern/` literal syntax yet
(`/` already means division; telling a leading regex apart from division
needs the same kind of disambiguation Symbol's `:` got, not yet done for
`/`), no `"x".match(re)`/`=~` String integration, and no richer
`MatchData` object (`pre_match`, named captures) — the plain-`Array`
result covers the common case. `String#split`/`#sub`/`#gsub`/`#scan` do
accept a `Regexp` (`"a,b,c".split(re)`, `"abc123".sub(re, "X")`,
`"abc123".gsub(re, "X")`, `"abc123".scan(re)`), each with backreference
and capture-group handling of its own — see the
[Collections guide](collections.md) for those; `Regexp.new` +
`.match`/`.match?` plus that String quartet is the whole surface for now.

## No AST

The compiler is a Pratt parser that emits register bytecode directly;
there is no retained AST. `diamond_compile` actually runs this parser
twice per compile, not once: a throwaway first "discovery" pass walks the
whole source (tolerating a forward reference to a not-yet-declared
class/module/interface just long enough to keep going) purely to
register every declaration regardless of textual order, then a real
second pass emits the bytecode that actually runs, with every
declaration already known from the start -- see `docs/roadmap.md`'s
"Compiler representation" section. This is what lets a class construct
or call a singleton method on another class declared later in the same
file (`SomeClass.new(...)`, `OtherClass.someMethod(...)`), and lets a
type annotation name a class declared later too. A superclass still has
to be declared first (`class B < A` needs `A`'s complete, already-*fully-
compiled* field table, not just its name), and there's still no retained
AST for either pass to share — each is a full, independent walk of the
token stream. Pass `--dump-bytecode` on the CLI to see how any construct
in this document actually lowers (that dump reflects only the second,
real pass's output).
