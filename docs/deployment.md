# Deployment

`diamond build` compiles a `.di` program into a standalone native executable:
no `diamond` interpreter, no `.di` source file, and no recompilation step at
run time. It exists for shipping a finished Diamond program to a machine
that doesn't have (and shouldn't need) a Diamond checkout, alongside the
ordinary `diamond program.di` interpreter path.

```sh
./build/diamond build app.di -o app
./app arg1 arg2
```

## CLI contract

```text
diamond build SOURCE [-o OUTPUT] [--cc=COMPILER]
```

- `SOURCE` is compiled exactly the way `diamond program.di` compiles it --
  identical `require` resolution, identical embedded-prelude-template fast
  path -- so anything that runs correctly under the interpreter compiles
  identically under `build`.
- `-o OUTPUT` names the produced binary. Defaults to `SOURCE`'s own basename
  with a trailing `.di` extension stripped (`app.di` -> `app`; no extension
  at all if `SOURCE` has none).
- `--cc=COMPILER` overrides the C compiler used to link the binary (`gcc`,
  `clang`, a full path, ...). Defaults to whatever `$(CC)` resolves to for
  an ordinary `make` invocation in this checkout.
- A compile error (a real syntax/type/name error in `SOURCE` or anything it
  `require`s) prints the same diagnostic `diamond program.di` would and
  exits 65 -- the C compiler and `make` are never invoked in this case.
- A `make`/link failure (a missing toolchain or system library) exits 74.
- On success, prints `diamond: built 'OUTPUT'` and exits 0.

At the language level, the produced binary behaves exactly like
`diamond program.di`: it sets `ARGV` from its own command-line arguments and
prints its top-level result the same way (`diamond_value_print`, the same
function `diamond program.di` itself uses -- e.g. a program whose last
expression is a bare string prints that string, with no explicit `puts`
needed).

## What this is not

- **Not a fully static/hermetic binary.** The produced executable still
  dynamically links whatever `diamond` itself links against on this
  platform -- OpenSSL, SQLite3, `libpq`, the MariaDB client library, zlib,
  `libcrypt`, and their own transitive dependencies (confirmed with `ldd`
  against a real build). "No separate interpreter, source file, or
  recompilation step" is the actual guarantee; the binary is exactly as
  portable across machines as `diamond` itself is, no more and no less.
- **Not cross-compilation.** The binary targets the machine `diamond build`
  ran on.
- **Must be run *from* the repository checkout, not run *against* one
  afterward.** `diamond build` invokes `make aot-build` to link the binary,
  the same assumption `make dap`/`make lsp` already make about their own
  build targets -- there is no separate "install Diamond system-wide" story
  yet for it to build against instead. This is a **build-time** constraint
  only: the finished binary has no dependency on the checkout at *run*
  time. It can be copied anywhere, including a machine with no Diamond
  checkout on it at all, and run from any working directory (confirmed
  directly: copying a built binary outside the repo, then running it from
  an unrelated directory, produces identical output to running it from
  inside the checkout).

See [docs/roadmap.md](roadmap.md) for what's explicitly deferred here
(cross-compilation, a real install step, static linking).
