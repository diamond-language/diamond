# Diamond

[![Version](https://img.shields.io/github/v/tag/diamond-language/diamond?label=version&color=blue)](https://github.com/diamond-language/diamond/tags)
[![Build status](https://github.com/diamond-language/diamond/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/diamond-language/diamond/actions/workflows/ci.yml)

Diamond is an object-oriented language with gradual, checked static typing,
compiled to a register-bytecode VM written in C23 (with an optional x86-64
JIT). It builds to one standalone executable, and runs in your browser at
[getdiamond.codes](https://getdiamond.codes).

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

## Familiar syntax

Blocks, `end`-delimited bodies, classes/modules/mixins, `rescue`/`ensure`,
Enumerable-style methods, string interpolation, and symbols will look
familiar if you have written scripting languages. The language is its own: it
is not a compatibility project for any other, and its standard library and
package ecosystem are its own.

## Where it actually differs

- **Every call needs parentheses.** `puts("x")`, not `puts "x"`; `Foo.new()`,
  not `Foo.new`. There is no bare-call form.
- **Types are optional but real.** Annotations, union types with nil
  narrowing, generics, and structural interfaces (no `implements` -- any
  class with a matching method satisfies one) are all checked at compile
  time, not just documentation.
- **Compiled, not just interpreted.** Source goes through a real Pratt
  compiler to register bytecode, run on a VM with inline caches, object
  shapes, quickening, and an opt-in x86-64 JIT. `diamond build` produces a
  standalone executable, no separate interpreter or `.d6` file needed at run
  time; with a target kit it cross-compiles, and with a musl kit it links fully
  statically.
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

New to Diamond? The [getting-started guide](docs/getting-started.md) goes from
installing it to a tested project built into a standalone executable. Each
[release](https://github.com/diamond-language/diamond/releases) has `.deb`
(Ubuntu 26.04) and `.rpm` (Fedora 44) packages for x86-64.

`.d6` is the default and preferred extension for Diamond source files; compiled bytecode
caches use `.d6c`. Backward compatibility with `.di` source files is retained
for existing projects and published cuts. Use `.d6` for new files to avoid
colliding with D interface files and to get automatic editor recognition.
When both `helper.d6` and `helper.di` exist, `require "helper"` loads
`helper.d6`; see the [import precedence example](docs/core-syntax.md#source-files-and-imports).

```sh
make
./build/diamond -e '20 + 22'
./build/diamond program.d6
./build/diamond              # REPL, when stdin is a terminal
```

Build a standalone binary (from the repo root; see
[docs/deployment.md](docs/deployment.md) for cross-compiling and static
builds):

```sh
./build/diamond build app.d6 -o app
./app
```

Try it without installing anything: the REPL at [getdiamond.codes](https://getdiamond.codes)
runs the interpreter as WebAssembly (`wasm/build.sh`, see
[docs/internal/wasm.md](docs/internal/wasm.md)).

See [CONTRIBUTING.md](CONTRIBUTING.md) for required system dependencies
(per-platform install commands), build/test targets, and the loopback-network
and sanitizer/ptrace notes relevant to running in a container or agent
sandbox.

## Repository layout

- `src/`: loader, lexer, compiler, bytecode, VM, GC, and native primitives
- `lib/core.d6`: embedded Diamond prelude
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
  (the type system), `kvstore` (TCP and IO.poll), `ledger`
  (the object model), `markdown` (strings and Regexp), `models`
  (metaprogramming), `parallel`
  (threads, channels, supervisors), `task_board` (a Mjolnir-backed terminal board),
  `taskrun` (processes), and `vault`
  (crypto and encoding); `make test-examples` runs the standalone tours, and
  `make test-task-board` checks the task board
- `fuzz/`: compiler, bytecode-execution and differential (interpreter vs JIT)
  fuzz targets
- `wasm/`: WebAssembly build script, REPL driver and browser module
- `bench/`: performance and GC evidence

## Learn more

- [Getting started](docs/getting-started.md) -- install, the REPL, a first
  project with `facet new`, tests, dependencies and `diamond build`, then a
  reading order for the rest of `docs/`
- [CHANGELOG.md](CHANGELOG.md) -- completed capability milestones
- [docs/roadmap.md](docs/roadmap.md) -- candidate work, explicitly
  deferred work, and known limitations
- [Language reference](docs/syntax.md) -- [core syntax](docs/core-syntax.md),
  [callables](docs/callables.md),
  [classes and modules](docs/classes-and-modules.md),
  [types and errors](docs/types-and-errors.md),
  [collections](docs/collections.md)
- [Object model](docs/object-model.md), [Fibers](docs/fibers.md),
  [Threads](docs/threads.md), [Cancellation](docs/cancellation.md),
  [Time and calendar](docs/time.md)
- [I/O and native services](docs/io.md) -- [local I/O](docs/local-io.md),
  [networking](docs/networking.md), [databases](docs/databases.md),
  [processes](docs/processes.md)
- [Packages](docs/packages.md), [Language Server](docs/lsp.md),
  [Debugging](docs/debugging.md), [REPL](docs/repl.md)
- [Deployment](docs/deployment.md), [Sandbox](docs/sandbox.md),
  [Bytecode caching](docs/caching.md), [Portability](docs/portability.md)

Implementation notes and maintainer tooling (VM/GC internals, fuzzing) live
under [`docs/internal/`](docs/internal/README.md) -- not needed to write or
run Diamond programs, but useful background if you're contributing.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build/test workflow, code style,
and commit and documentation conventions.

## License

[MIT](LICENSE)
