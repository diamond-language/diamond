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

## Building for a different target distro, via container

Because `diamond build` links against whatever OpenSSL/SQLite3/`libpq`/MariaDB
client/glibc versions the *build machine* has ("Not a fully static/hermetic
binary" above), a binary built on one distro isn't guaranteed to run on
another -- most concretely, glibc is backward- but not forward-compatible, so
a binary built on a newer glibc can reference symbol versions an older-glibc
target doesn't have.

Rather than chase static linking (a separately-tracked, much bigger effort --
see `docs/roadmap.md`), the practical fix is to build *inside* a container
running the same distro/version as the deploy target, so the binary links
against *its* library versions directly, regardless of what the build host
itself has installed. `docker/Dockerfile.ubuntu-build` is a worked example:
an `ubuntu:26.04` base image with the same dev packages this README's own
"On Ubuntu" section lists, with no source baked in (source comes from a bind
mount at container-run time, not a clone, so it reflects whatever's on disk
locally -- committed or not).

The container must never build against a live checkout in place via a
read-write bind mount: some build artifacts (e.g. this repo's own
`reginold/libreginold.a`) live as plain tracked files rather than under
`BUILD_DIR`, so a native build and a container build sharing one directory
risks `make`'s mtime-based staleness tracking silently reusing a
wrong-platform object file left over from the other one. The safe shape is:

1. Bind-mount the source **read-only** into the container.
2. `cp -r` it into the container's own writable layer at container start (an
   ephemeral copy, gone when the container exits).
3. Build entirely inside that copy, using the container's own freshly-built
   `diamond` (never the host's).
4. Copy only the finished binary back out, via a second, **read-write** bind
   mount pointing at a dedicated output directory -- the only thing that
   crosses the container boundary in either direction besides the initial
   read-only source copy.

`skindicate.dia/build_ubuntu.sh` and `skindicate.dia/deploy_prebuilt.sh` are a
complete worked example of this for a Diamond application: the former runs
the container build described above and leaves the result at
`skindicate.dia/dist/app`; the latter ships that binary to a real deploy
target and restarts the service running it.

**Caveat:** matching the target's distro and major version is "very likely
works," not a guaranteed identical package snapshot -- the container image's
installed library versions are whatever that base image shipped with at
build time, which can drift from a real target's own `apt upgrade` history
since it was provisioned. Verify with `ldd`/`file` against the produced
binary and a real smoke test against the actual target, the same as any
other deploy.
