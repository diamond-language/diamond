# Diamond

Diamond is a Ruby-inspired programming language with gradual typing and a
custom register-bytecode virtual machine written in C23.

It is a coherent, executable language rather than a compatibility project:
Ruby supplies familiar syntax and object-model ideas, but matching Ruby's edge
cases, standard library, or ecosystem is explicitly not a goal. CI validates
Fedora and Ubuntu 26.04 (glibc) across GCC and Clang, plus Alpine (musl),
FreeBSD, and macOS/Darwin each on one compiler -- three libcs and three
kernels, not just five distros. Debian itself is not separately tested;
Ubuntu tracks ahead of it, so Debian compatibility is assumed, not
confirmed. OpenBSD and non-x86_64 architectures remain unvalidated -- see
[docs/portability.md](docs/portability.md) for the full matrix and known
limitations.

```ruby
class Counter
  include Comparable

  def initialize(value: Int)
    @value = value
  end

  def <=>(other: Counter) = @value <=> other.value()
  def value() = @value
end

values = [Counter.new(3), Counter.new(1), Counter.new(2)]
values.sort_by() do |counter|
  counter.value()
end.map() do |counter|
  counter.value()
end
# => [1, 2, 3]
```

## Status

Diamond executes complete programs through:

```text
source -> require expansion -> lexer -> Pratt compiler
       -> register bytecode -> VM -> managed heap
```

The implementation includes:

- integers with transparent arbitrary-precision overflow, floats, booleans,
  `nil`, strings, symbols, arrays, hashes, ranges, regexps, and instances;
- expression-valued control flow, `case`/`when`, ternaries, loops, destructuring,
  compound assignment, postfix conditions, and short-circuit operators;
- functions, closures, trailing `do |...| ... end` blocks, recursion, defaults,
  keyword arguments, generics, and typed callable contracts;
- classes, inheritance, modules, namespaces, interfaces, visibility, generated
  attributes, singleton methods, operator methods, method aliases, and runtime
  method replacement;
- optional parameter and return annotations, unions, nil narrowing, generic
  collection contracts, and structural interface checks;
- rescuable runtime failures, typed rescue clauses, `ensure`, `else`, `retry`,
  causal exception chains, and captured backtraces;
- cooperative fibers and real OS threads with isolated heaps, plus a
  bounded thread-safe `Channel` mailbox for ongoing inter-thread messaging;
- a Diamond-written core library with Enumerable, Comparable, JSON, formatting,
  collection helpers, and a small Minitest-style test library;
- files, blocking and nonblocking TCP, UDP, TLS, signals, SQLite3, calendar time,
  subprocesses, and stdin/stdout primitives;
- a generational, stop-the-world mark/sweep collector with stress-GC modes;
- source-mapped diagnostics, stack traces, bytecode disassembly, inline caches,
  runtime object shapes, and opt-in opcode quickening;
- `diamond build`, producing a standalone native executable with no separate
  interpreter, source file, or recompilation step at run time (see
  [docs/deployment.md](docs/deployment.md)).

The repository also contains:

- `facet`, a git-based package manager;
- a Language Server with diagnostics, completion, hover, definitions, document
  symbols, and workspace symbols;
- a Debug Adapter Protocol server (`dap/`) for compile-time-breakpoint
  step debugging -- editor gutter breakpoints, real stack traces, and
  locals at the paused frame, with no stepping yet (see
  [docs/debugging.md](docs/debugging.md));
- a VS Code extension with syntax highlighting, an LSP client, and a DAP
  debugger integration;
- a Diamond implementation of the lexer and compiler that differentially
  matches the native compiler and passes self-compile/self-run bootstrap checks;
- HTTP, Rack-style middleware, Gremlin server, and Arel-style SQL-builder
  packages;
- compiler and bytecode-execution fuzz targets plus performance/GC benchmarks.

The end-to-end corpus contains more than 1,000 Diamond programs, alongside
native VM tests, lexer and parser differential suites, sanitizer builds,
package tests, LSP protocol tests, REPL tests, and fuzz smoke tests.

See [CHANGELOG.md](CHANGELOG.md) for completed capability milestones and
[docs/roadmap.md](docs/roadmap.md) for future directions.

## Build and run

Required system dependencies:

- GCC 15+ or Clang 19+ (see below) with C23 support;
- OpenSSL development headers and libraries;
- SQLite3, PostgreSQL (`libpq`), and MariaDB/MySQL client development
  headers and libraries;
- zlib and libcrypt (`crypt(3)`) development headers and libraries;
- POSIX threads and `ucontext`, provided by the target Linux environment.

On Fedora:

```sh
sudo dnf install gcc openssl-devel sqlite-devel libpq-devel \
  mariadb-connector-c-devel libxcrypt-devel zlib-devel
```

On Ubuntu (confirmed against a real Ubuntu 26.04 install; Debian itself is
assumed compatible from the same package names, but not separately tested --
Ubuntu runs ahead of Debian, and nothing here has been checked against a
real Debian install):

```sh
sudo apt install gcc libssl-dev libsqlite3-dev libpq-dev libmariadb-dev \
  libcrypt-dev zlib1g-dev
```

On Alpine (musl; also needs `libucontext` for Fiber support -- see
[docs/portability.md](docs/portability.md) for what else differs on musl):

```sh
apk add gcc musl-dev openssl-dev sqlite-dev libpq-dev mariadb-connector-c-dev \
  zlib-dev libucontext libucontext-dev
```

On FreeBSD (Clang is the base `cc`; GNU Make is `gmake`, not `make`):

```sh
pkg install gmake sqlite3 openssl postgresql16-client mariadb-connector-c
```

On macOS (Clang only -- there is no system GCC):

```sh
brew install openssl@3 sqlite postgresql@16 mariadb-connector-c
```

Clang works as a drop-in `$(CC)` substitute on Fedora and Ubuntu
(`make CC=clang debug`, tested end-to-end on both alongside GCC) and is
required separately for the fuzz targets (`make fuzz`), which always build
with Clang regardless of `$(CC)` (`-fsanitize=fuzzer` is Clang/LLVM-only).
Alpine has only been validated with GCC; FreeBSD and macOS have only been
validated with Clang -- see docs/portability.md for exactly what's been
checked on each platform, including per-platform `CPPFLAGS_EXTRA`/
`LDFLAGS_EXTRA`/`LDLIBS_DL`/`LDLIBS_CRYPT` overrides FreeBSD/macOS need for
non-default header/library locations.

Build and run:

```sh
make
./build/diamond -e '20 + 22'
./build/diamond program.di
./build/diamond              # interactive REPL when stdin is a terminal
```

Build a standalone binary (from the repo root; see
[docs/deployment.md](docs/deployment.md) for what it does and doesn't do):

```sh
./build/diamond build app.di -o app
./app
```

Useful build and test targets:

```sh
make debug
make release
make sanitize
make tsan
make test
make test-all
make test-self-host          # full self-hosted lexer/parser differential corpus
make fuzz                    # uses clang/libFuzzer
make clean
```

`make test-all` intentionally runs the broad validation matrix sequentially:
debug, release, sanitizers, native VM/fiber tests, packages, LSP, REPL, fuzz
smoke tests, and a self-hosting bootstrap smoke check. It is thorough and
correspondingly slow; CI is the normal place to run it after a focused local
test.

`make sanitize`/`test-sanitize` run with LeakSanitizer enabled
(`ASAN_OPTIONS=detect_leaks=1`) by default -- real, working leak detection on
a normal Linux machine. Under ptrace-restricted containers, including
GitHub's own CI runners, set `ASAN_OPTIONS=detect_leaks=0` in the
environment before invoking `make test-sanitize` (an already-set
`ASAN_OPTIONS` always wins over the Makefile's own default, never the other
way around; see ci.yml). Even with leak detection disabled, `test-sanitize`
has been observed to fail intermittently on GitHub's runners with no
diagnostic output and no local reproduction on a real (non-container) Linux
box -- consistent with the runners' own virtualization rather than a real
bug, which is why CI retries this one step once before failing the job.

Self-hosting is in minimal-compat maintenance mode (see docs/roadmap.md):
`test-all` only confirms the self-hosted frontend still parses and runs
itself, not full parity. The exhaustive lexer/parser differential corpus
(~1400 cases) is `make test-self-host`, run periodically/manually rather
than on every push -- it's dominated by the self-hosted parser re-parsing
`lib/core.di` through the interpreter on every case, an inherent cost of
self-hosting rather than a test-harness inefficiency.

Inspect bytecode while still executing a program:

```sh
./build/diamond --dump-bytecode -e '20 + 22'
./build/diamond --dump-bytecode program.di
```

Collect before every eligible allocation:

```sh
DIAMOND_STRESS_GC=1 ./build/diamond program.di
```

## Language tour

Only `false` and `nil` are falsey. Control-flow constructs return values:

```ruby
label = if score >= 90
  "excellent"
elsif score >= 70
  "good"
else
  "retry"
end

fallback = configured ? value : "default"
```

Functions return their final expression unless they return explicitly:

```ruby
def factorial(n: Int) -> Int
  return 1 if n <= 1
  n * factorial(n - 1)
end
```

Classes have stable fields, inheritance, `self`, and lexically anchored
`super` calls:

```ruby
class Point
  attr_reader x: Int, y: Int

  def initialize(x: Int, y: Int)
    @x = x
    @y = y
  end

  def +(other: Point) -> Point
    Point.new(@x + other.x(), @y + other.y())
  end
end
```

Modules provide reusable behavior, while interfaces are structural: there is
no `implements` declaration, so any class that happens to define a matching
method satisfies the interface, and using the interface as a type is what
actually gets checked, at compile time, against whatever value flows there:

```ruby
interface Named
  def name() -> String
end

module Printable
  def print_name()
    puts(self.name())
  end
end

class User
  include Printable
  def name() -> String = "Ada"
end

def greet(entity: Named)
  puts("Hello, " + entity.name())
end

greet(User.new())
# => Hello, Ada
```

Annotations are optional. Dynamic code and checked code share one object model:

```ruby
def find_name(id: Int) -> String | Nil
  if id == 42
    "diamond"
  else
    nil
  end
end

name = find_name(42)
puts(name.upcase()) if name != nil
```

Generic functions infer type variables from values and callback signatures:

```ruby
def first[T](values: Array[T]) -> T
  values[0]
end

first([1, 2, 3])
```

Arrays, hashes, ranges, and classes implementing `each` can use Enumerable
operations. Native collections use VM forwarding to the same Diamond-written
functions used by `include Enumerable`:

```ruby
evens = (1..10).select() do |n|
  n % 2 == 0
end

groups = ["ant", "bear", "cat"].group_by() do |word|
  word.length()
end
```

Exceptions use one unwind path for explicit raises and VM failures:

```ruby
begin
  risky_operation()
rescue error: IOError | SQLite3Error
  puts(error.message())
ensure
  cleanup()
end
```

`require` combines Diamond files into one compilation:

```ruby
require "models/user"
require "support/formatting.di"
```

Paths are relative to the requiring file, `.di` is inferred, canonical files
load once, and cycles are rejected. `require` never falls back to anything
else — an installed package (a "cut") is reached explicitly instead, with
`require_cut "name"`, resolved from `cuts/<name>/lib/<name>.di` against the
process's own working directory. The two never compete: a relative file and
a same-named cut can coexist without either shadowing the other. Diagnostics
and runtime traces retain the original imported file, line, and column.

## Standard and native facilities

Representative APIs include:

```ruby
JSON.parse(JSON.stringify({"answer": 42}))

db = SQLite3.open(":memory:")
db.execute("CREATE TABLE items (name TEXT, qty INTEGER)")
db.execute("INSERT INTO items VALUES (?, ?)", ["pens", 3])
rows = db.query("SELECT * FROM items WHERE qty >= ?", [1])
db.close()

response = Process.run(["printf", "hello"])
meeting = Time.parse("2026-08-30T12:30:00-07:00")
reminder = meeting.days_ago(1)
```

See [docs/io.md](docs/io.md) for files, sockets, polling, signals, TLS, SQLite,
and Process. See the [Time and calendar guide](docs/time.md) for construction,
fixed offsets, ISO-8601, arithmetic, DST-aware calendar helpers, and deliberate
timezone limits. See [docs/syntax.md](docs/syntax.md) for the complete syntax
and core-library surface.

## Tooling

### REPL

Running `diamond` with terminal stdin starts a session-accumulating REPL.
Multi-line definitions prompt until their closing `end`; locals, functions, and
classes remain available to later evaluations. See [docs/repl.md](docs/repl.md).

### Packages

`facet` installs dependencies pinned to git refs and writes a lockfile. Diamond
does not currently have a hosted registry or semantic-version solver. See
[docs/packages.md](docs/packages.md).

### Language Server and VS Code

Build the server with `make lsp`. It communicates over stdio using LSP/JSON-RPC
and supports full-document synchronization, diagnostics, completion, hover,
definitions, document symbols, and workspace symbols. Receiver-aware method
resolution remains limited. See [docs/lsp.md](docs/lsp.md) and
[editors/vscode/README.md](editors/vscode/README.md).

### Self-hosted compiler

`selfhost/lexer.di` and `selfhost/parser.di` implement the frontend in Diamond.
Differential suites compare their tokens, diagnostics, generated bytecode, and
runtime behavior with the native C compiler. Bootstrap checks cover compiling
and running the compiler through itself. The native compiler remains the main
CLI frontend. See [docs/roadmap.md](docs/roadmap.md) for remaining directions.

## Runtime and performance controls

The VM has polymorphic method and field caches, monomorphic call-site rewrites,
runtime object shapes, and opt-in integer opcode quickening. These environment
variables expose the main research controls:

```text
DIAMOND_QUICKEN=1
DIAMOND_QUICKEN_THRESHOLD=N
DIAMOND_IC_MONO_THRESHOLD=N
DIAMOND_TRACE_IC=1
DIAMOND_TRACE_IC_SITES=1
DIAMOND_TRACE_IC_REWRITES=1
DIAMOND_TRACE_FIELDS=1
DIAMOND_TRACE_SHAPES=1
DIAMOND_TRACE_OPCODES=1
DIAMOND_TRACE_GC=1
```

The benchmark directories document measured results rather than relying only
on wall-clock microbenchmarks:

- `bench/gremlin_http`: threaded HTTP throughput;
- `bench/burn_in`: long-running server behavior with a guarded memory ceiling;
- `bench/gc_churn`: collector pause cost versus live-set size and churn.

## Architecture

Diamond deliberately compiles directly to bytecode without retaining an AST.
The implementation is organized as follows:

- `src/`: loader, lexer, compiler, bytecode, VM, GC, and native primitives;
- `lib/core.di`: embedded Diamond prelude;
- `selfhost/`: Diamond lexer/compiler and bootstrap programs;
- `tests/`: executable cases and native/differential harnesses;
- `lsp/`: language server;
- `editors/vscode/`: editor extension;
- `packages/`: independently consumable Diamond libraries;
- `fuzz/`: compiler and bytecode-execution fuzz targets;
- `bench/`: performance and GC evidence.

Language and runtime guides:

- [Language reference](docs/syntax.md)
  ([core syntax](docs/core-syntax.md), [callables](docs/callables.md),
  [classes and modules](docs/classes-and-modules.md),
  [types and errors](docs/types-and-errors.md),
  [collections](docs/collections.md))
- [Object model](docs/object-model.md)
- [Fibers](docs/fibers.md)
- [Threads](docs/threads.md)
- [Time and calendar](docs/time.md)
- [I/O and native services](docs/io.md)
  ([local I/O](docs/local-io.md), [networking](docs/networking.md),
  [databases](docs/databases.md), [processes](docs/processes.md))
- [Packages](docs/packages.md)
- [Language Server](docs/lsp.md)
- [Debugging](docs/debugging.md)
- [Portability](docs/portability.md)
- [Roadmap](docs/roadmap.md)

Implementation notes and maintainer tooling (VM/GC internals, fuzzing) live
under [`docs/internal/`](docs/internal/README.md) -- not needed to write or
run Diamond programs, but useful background if you're contributing.

## Current limitations

- The language, bytecode, and embedding APIs are intentionally unstable.
- The native compiler uses a declaration-discovery pass followed by real
  bytecode generation; forward and mutually recursive top-level calls resolve.
- Bytecode offsets, program tables, call depth, lexical captures, and other VM
  resources have fixed implementation limits.
- The collector is generational mark/sweep, but collection is still
  stop-the-world within a VM. Threads avoid a shared-heap pause by using
  isolated heaps and copying values across thread boundaries.
- Threads use isolated heaps. Values are copied across thread boundaries rather
  than sharing mutable objects.
- There is no general `eval`: `ClassName.compile_method` compiles a method
  body from a source string, but only into a capture-free method installed
  via `define_method`, not arbitrary code in the calling scope.
- The LSP recompiles complete documents and cannot generally resolve a method
  name through an arbitrary receiver type.
- `facet` has no hosted registry, version solver, or multi-version dependency
  model.
- OpenBSD is blocked on a real gap (no `<ucontext.h>` at all, a Fiber-
  implementation limitation, not a CI-tooling one); non-x86_64 architectures
  are unvalidated; Debian is assumed, not separately tested, compatible with
  the Ubuntu package names above.
- Calendar time supports UTC, the process-local zone, and fixed offsets, but
  not named IANA timezone selection.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build/test workflow, code style,
and commit and documentation conventions.

## License

[MIT](LICENSE)
