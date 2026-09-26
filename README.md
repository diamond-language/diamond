# Diamond

[![Version](https://img.shields.io/github/v/tag/diamond-language/diamond?label=version)](https://github.com/diamond-language/diamond/tags)
[![Build status](https://github.com/diamond-language/diamond/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/diamond-language/diamond/actions/workflows/ci.yml)

Diamond is a Ruby-inspired language with gradual, checked static typing,
compiled to a custom register-bytecode VM written in C23.

Learn more at [dilang.tech](https://dilang.tech).

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

## Familiar if you know Ruby

Syntax, blocks, `end`-delimited bodies, classes/modules/mixins,
`rescue`/`ensure`, Enumerable-style methods, string interpolation, symbols,
and the REPL will all feel immediately at home. Diamond borrows Ruby's
object-model ideas but is not a compatibility project -- matching Ruby's edge
cases, standard library, or gem ecosystem is explicitly not a goal.

## Where it actually differs

- **Every call needs parentheses.** `puts("x")`, not `puts "x"`; `Foo.new()`,
  not `Foo.new`. There is no bare-call form.
- **Types are optional but real.** Annotations, union types with nil
  narrowing, generics, and structural interfaces (no `implements` -- any
  class with a matching method satisfies one) are all checked at compile
  time, not just documentation.
- **Compiled, not just interpreted.** Source goes through a real Pratt
  compiler to register bytecode, run on a VM with inline caches, object
  shapes, and opt-in quickening -- closer to Crystal or a JIT'd language than
  a tree-walker. `diamond build` also produces a standalone native
  executable, no separate interpreter or `.di` file needed at run time.
- **Concurrency doesn't share mutable state.** Threads use isolated heaps;
  values are copied across boundaries instead of shared. A `Channel` gives
  threads an ongoing mailbox, and a `Supervisor` restarts a crashed worker
  automatically (Erlang's `one_for_one`, not exception handling).
- **A sandbox mode is built into the language itself**
  (`diamond --sandbox`), not bolted on: denies filesystem, network, and
  subprocess access for running untrusted code.
- **No general `eval`.** `ClassName.compile_method` compiles a capture-free
  method body from a source string; there's no arbitrary code evaluation in
  the calling scope.

## Build and run

```sh
make
./build/diamond -e '20 + 22'
./build/diamond program.di
./build/diamond              # REPL, when stdin is a terminal
```

Build a standalone binary (from the repo root; see
[docs/deployment.md](docs/deployment.md) for what it does and doesn't do):

```sh
./build/diamond build app.di -o app
./app
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for required system dependencies
(per-platform install commands), build/test targets, and the loopback-network
and sanitizer/ptrace notes relevant to running in a container or agent
sandbox.

## Repository layout

- `src/`: loader, lexer, compiler, bytecode, VM, GC, and native primitives
- `lib/core.di`: embedded Diamond prelude
- `selfhost/`: Diamond lexer/compiler and bootstrap programs
- `tests/`: executable cases and native/differential harnesses
- `lsp/`: language server; `dap/`: Debug Adapter Protocol server
- `editors/vscode/`: editor extension
- `packages/`: independently consumable Diamond libraries (HTTP, Rack-style
  middleware, Arel-style SQL builder, ActiveRecord-style ORM, GraphQL, ...)
- `examples/`, `applications/`: runnable sample apps, from a two-table CRUD
  app and a WebSocket chat room to a standalone log-summary CLI and a
  from-scratch GPT-style transformer, plus smaller tours of one area each:
  `agenda` (Time and calendars), `calc` (parsing and pattern matching), `generators` (fibers), `grades`
  (the type system), `ledger`
  (the object model), `markdown` (strings and Regexp), `parallel`
  (threads, channels, supervisors), `taskrun` (processes), and `vault`
  (crypto and encoding); `make test-examples` runs their tests
- `fuzz/`: compiler and bytecode-execution fuzz targets
- `bench/`: performance and GC evidence

## Learn more

- [CHANGELOG.md](CHANGELOG.md) -- completed capability milestones
- [docs/roadmap.md](docs/roadmap.md) -- candidate work, explicitly
  deferred work, and known limitations
- [Language reference](docs/syntax.md) -- [core syntax](docs/core-syntax.md),
  [callables](docs/callables.md),
  [classes and modules](docs/classes-and-modules.md),
  [types and errors](docs/types-and-errors.md),
  [collections](docs/collections.md)
- [Object model](docs/object-model.md), [Fibers](docs/fibers.md),
  [Threads](docs/threads.md), [Time and calendar](docs/time.md)
- [I/O and native services](docs/io.md) -- [local I/O](docs/local-io.md),
  [networking](docs/networking.md), [databases](docs/databases.md),
  [processes](docs/processes.md)
- [Packages](docs/packages.md), [Language Server](docs/lsp.md),
  [Debugging](docs/debugging.md), [Portability](docs/portability.md)

Implementation notes and maintainer tooling (VM/GC internals, fuzzing) live
under [`docs/internal/`](docs/internal/README.md) -- not needed to write or
run Diamond programs, but useful background if you're contributing.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build/test workflow, code style,
and commit and documentation conventions.

## License

[MIT](LICENSE)
