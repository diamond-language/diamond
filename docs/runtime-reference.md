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
warn("disk almost full")    # stderr, with a newline
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
I/O](local-io.md#file-paths-filejoindirnamebasenameextnameabsoluteexpand_path)
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
A constant or user-defined function named `ARGV`/`ENV` shadows it.
Constant definitions belong at top level or inside a class or module.

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
the pattern) — combine flags with `|` (or plain addition, since they're
disjoint bits and the two coincide): `1 | 2` for case-insensitive *and*
dot-matches-newline together. See [Classes and modules](classes-and-modules.md)
for `|`/`&`/`^`/`>>`'s own Int-only bitwise semantics.

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

## JSON

```ruby
text = JSON.stringify({"name": "Ada", "scores": [98, 91.5], "admin": false, "note": nil})
# => {"name":"Ada","scores":[98,91.5],"admin":false,"note":null}

data = JSON.parse(text)
data["scores"][1]   # => 91.5
```

`JSON.stringify(value)` accepts `nil`, `Bool`, `Int` (including big
integers), finite `Float`, `String`, and any `Array` or `Hash` built from
those. Hash keys that aren't Strings are written as their string form
(`{1: 2}` becomes `{"1":2}`); Hash entries keep their insertion order.
Strings are escaped as JSON requires (quotes, backslashes, and control
characters) and otherwise written byte for byte. Anything else -- a
`Symbol`, an instance, `NaN`, `Infinity` -- raises a rescuable `JSONError`.
Output is compact, with no whitespace. Nesting is limited to 91 levels, the same
bound `JSON.parse` uses; a deeper document, or an Array/Hash that contains
itself, raises `SystemStackError` rather than recursing without end.

`JSON.parse(text)` (also `text.parse_json()`) returns the value a JSON
document describes: objects become Hashes with String keys, arrays become
Arrays, `null` becomes `nil`, numbers without a fraction or exponent become
`Int` (promoting past 64 bits as needed), other numbers become `Float`, and
`\uXXXX` escapes are decoded to UTF-8. Invalid input raises `JSONError`
(for example "unexpected end of input").

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

### Targeted bytecode dumps

`diamond --dump-bytecode=user FILE [ARGS...]` (or `-e CODE [ARGS...]`)
prints the application's bytecode and its `require`/`require_cut` imports,
excluding the bundled core/JSON preamble. Instruction offsets, registers,
function IDs, and jump targets retain their values in the complete program.
The program still runs afterward, with its usual output and exit status.

Plain `--dump-bytecode` keeps the complete dump; `--dump-bytecode=all` is an
explicit equivalent. User-only dumps recompile instead of reading or updating
the `.dic` cache so the preamble boundary is known even for cached programs.
This is a preamble filter, not a single-file or single-function selector.

## Diagnostic environment variables

These switches report what the interpreter did; none change what a program
means. A flag takes effect when it is set to any value, including an empty one.
A numeric one takes a positive integer and is ignored otherwise. Every report
goes to standard error, and the counter reports print once, after the program
has finished without a runtime error; a server stopped by a signal never
prints them. They apply to the `diamond` command.

Documented with their features instead: `DIAMOND_JIT`, `DIAMOND_JIT_THRESHOLD`
and `DIAMOND_TRACE_JIT` ([deployment](deployment.md)); `DIAMOND_NO_CACHE` and
`DIAMOND_TRACE_CACHE` ([caching](caching.md)); `DIAMOND_SANDBOX`,
`DIAMOND_SANDBOX_ALLOW` and the three `DIAMOND_MAX_*` budgets
([sandbox](sandbox.md)); `DIAMOND_DEBUG_FD`, `DIAMOND_DEBUG_BREAKPOINTS` and
`DIAMOND_DEBUG_BREAKPOINT_OFFSETS` ([debugging](debugging.md)); `DIAMOND_FORCE_REPL`
([repl](repl.md)); `DIAMOND_AOT_KIT` ([deployment](deployment.md)).

### Where the time goes

- `DIAMOND_TRACE_STARTUP` -- `startup: load Xs, compile Xs (N bytes: P prelude + U
  user), run Xs, total Xs`. The prelude figure is 0 when the prelude comes
  precompiled instead of being compiled with the program.
- `DIAMOND_TRACE_COMPILE` -- `compile: discovery Xs, real Xs, total Xs, source N
  bytes`, once per compile: the throwaway declaration-discovery pass, then the pass
  that emits bytecode (see "No AST" above).
- `DIAMOND_TRACE_GC` -- `GC: M major (Xs), m minor (Xs)`: collection counts and
  total pause time.
- `DIAMOND_REPEAT=N` -- run the compiled program `N` times in one VM, stopping at the
  first failure. Later runs start with warm caches, which is what a steady-state
  measurement wants; the program's own output repeats every time.
- `DIAMOND_TRACE_OPCODES` -- one `opcode[N]: count` line per opcode that ran, where
  `N` is the opcode's ordinal in `DiamondOpCode` (`src/vm.h`).
  `--dump-bytecode` prints the names.

### Inline caches and shapes

Method calls and field reads are cached per call site. A site that keeps hitting
one class is rewritten to dispatch directly.

- `DIAMOND_TRACE_IC` -- `inline caches: H hits, M misses`.
- `DIAMOND_TRACE_IC_SITES` -- one `inline cache site[i]: H hits, M misses, C classes`
  line for every site that was ever used.
- `DIAMOND_TRACE_IC_FAST` -- `monomorphic dispatches: N`, the calls that took the
  single-class fast path.
- `DIAMOND_TRACE_IC_PROBES` -- `method cache probes: N`.
- `DIAMOND_TRACE_IC_REWRITES` -- `direct dispatch rewrites: N`, the call sites
  rewritten to direct dispatch.
- `DIAMOND_TRACE_IC_POLICY` -- the two thresholds in effect, `quicken threshold` and
  `monomorphic threshold`.
- `DIAMOND_IC_MONO_THRESHOLD=N` -- how many cache hits a site needs before it counts
  as monomorphic and may be rewritten. Default 1.
- `DIAMOND_TRACE_FIELDS` -- `field caches: H hits, M misses`.
- `DIAMOND_TRACE_SHAPES` -- `shape transitions: N`, the times an instance's field
  layout changed.
- `DIAMOND_INVALIDATE_IC_EACH_RUN` -- with `DIAMOND_REPEAT`, clear the method caches
  before every run after the first, so each run measures a cold cache.
- `DIAMOND_TRACE_IC_EACH_RUN` -- with `DIAMOND_REPEAT`, a `run N: inline caches: H
  hits, M misses, rewrites: R` line after every run instead of only a final total.

### Quickening

- `DIAMOND_QUICKEN` -- rewrite a generic `+`, `-`, `*`, `/`, `==`, `!=`, `<`, `<=`, `>` or
  `>=` site to an `Int`-specialized opcode once it has seen `Int` operands. A site whose
  operands later stop being `Int` falls back to the generic opcode, which is how
  mixed `Int`/`Float` arithmetic keeps working after a rewrite. Off by default.
- `DIAMOND_QUICKEN_THRESHOLD=N` -- `Int` observations, counted across the whole VM,
  needed before a rewrite. Default 1.
- `DIAMOND_TRACE_QUICKEN` -- `quickened sites: N, deoptimized sites: M`.

### Stress and verification

- `DIAMOND_STRESS_GC` / `DIAMOND_STRESS_MINOR_GC` -- force a major or a minor
  collection before every allocation that could trigger one. Programs run far
  slower; this exists to expose a value that is not rooted when the collector runs
  (see [GC design](internal/gc-generational-design.md)).
- `DIAMOND_DEBUG_VERIFY` -- the bytecode verifier (run on every `ProgramBuilder#run`
  program and on the embedded prelude) disassembles what it checks and normally
  discards the text. With this set the disassembly goes to standard error.
