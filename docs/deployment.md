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

You can also invoke the Diamond binary from the application directory:

```sh
cd ../skindicate.dia
../diamond/build/diamond build app.di
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
  `clang`, a full path, ...). Defaults to the compiler recorded in the
  installed AOT kit, or to whatever `$(CC)` resolves to for an ordinary
  `make` invocation in a checkout.
- A compile error (a real syntax/type/name error in `SOURCE` or anything it
  `require`s) prints the same diagnostic `diamond program.di` would and
  exits 65 -- the C compiler and `make` are never invoked in this case.
- A `make`/link failure (a missing toolchain or system library) exits 74.
- On success, prints `diamond: built 'OUTPUT'` and exits 0.
- An installed `diamond` (see "Installed AOT kit" below) links with a single
  compiler invocation against its prebuilt kit and needs neither this
  checkout nor `make`. Without a kit it uses the checkout, as follows.
- The compiler finds its own build checkout from the Diamond executable,
  while `SOURCE` and relative `OUTPUT` paths resolve from the caller's
  working directory. The checkout still supplies the C sources and
  Makefile used for linking.
- The first build compiles the runtime into `build/aot-COMPILER/` and
  archives it as `libdiamond-aot.a`. Later builds reuse that archive and
  compile only the generated program data. Header/source changes and
  changes to compiler flags rebuild affected runtime objects. The cache stamp
  includes a SHA-256 fingerprint of the runtime sources, headers, Makefile,
  and embedded prelude, so replacing a checkout with files that have older
  timestamps still invalidates the archive safely. `make clean` removes the
  default cache. `--cc` uses a separate cache per compiler.
  Set `AOT_CACHE_ROOT` to an absolute path to keep the archive outside an
  ephemeral checkout; remove that directory explicitly when no longer
  needed. Container builds should key this path to their image/toolchain.

## Installed AOT kit

`make aot-kit` collects everything `diamond build` needs to link into
`build/aot-kit/`: the runtime archive `libdiamond-aot.a` (compiled at `-O2`),
`libreginold.a`, and one-argument-per-line files recording the compiler,
compile flags, link flags, and Diamond version. `make install` copies the
built tools to `PREFIX/bin` and the kit to `PREFIX/lib/diamond/aot`:

```sh
make release facet lsp dap
make install PREFIX="$HOME/.local"
```

An installed `diamond build` finds the kit at `<bin>/../lib/diamond/aot`
(`DIAMOND_AOT_KIT` overrides the location), refuses a kit recorded for a
different Diamond version, and runs one `cc` command: the generated program
data plus the two archives and the recorded link flags. It still needs a C23
compiler with `#embed` support (GCC 15+ or Clang 19+) and the development
libraries named in the link flags, but not the Diamond sources, headers, or
`make`. The kit is specific to the platform and library versions it was built
against.

## JIT in standalone programs

Built executables use the same opt-in JIT settings as the interpreter:

```sh
DIAMOND_JIT=1 ./app
DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_TRACE_JIT=1 ./app
```

The default compilation threshold is 50 calls per eligible function. The
JIT runs only on glibc x86-64; on other platforms the executable stays on
the interpreter path. `DIAMOND_TRACE_JIT=1` prints compiled-function and
bailout counts when the program returns normally; a server terminated by a
signal may not emit that summary. The JIT remains off when `DIAMOND_JIT` is
unset, including for standalone binaries.

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
- **The Diamond build checkout is still needed at build time.** `diamond
  build` invokes its checkout's `make aot-build` target to link the binary;
  there is no separate installed runtime library yet. The finished binary
  has no dependency on the checkout at run time and can run from any
  working directory, including on a machine with no Diamond checkout.

See [docs/roadmap.md](roadmap.md) for what's explicitly deferred here
(cross-compilation, a real install step, static linking).

## Building for a different target distro

Because `diamond build` links against whatever OpenSSL/SQLite3/`libpq`/MariaDB
client/glibc versions the *build machine* has ("Not a fully static/hermetic
binary" above), a binary built on one distro isn't guaranteed to run on
another -- most concretely, glibc is backward- but not forward-compatible, so
a binary built on a newer glibc can reference symbol versions an older-glibc
target doesn't have.

Build on a machine with the target's distro and architecture. For Skindicate,
the production droplet has one CPU core and 1 GB of RAM, so **never compile
on the droplet**. Its [deployment runbook](https://github.com/matrix9180/skindicate.dia/blob/main/DEPLOYMENT.md)
builds on a local Ubuntu 26.04 VM with `build_vm.sh`, then uploads the finished
binary with `deploy_prebuilt.sh`. The upload script updates tracked static
assets and restarts the service; it does not build on the droplet.

A container is another way to match the target distro for local development.
`docker/Dockerfile.ubuntu-build` is an example:
an `ubuntu:26.04` base image with the dev packages in
[CONTRIBUTING.md](../CONTRIBUTING.md#build-and-test). It has no source baked in;
source comes from a bind mount at container-run time, so it reflects whatever is on disk
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

`skindicate.dia/build_ubuntu.sh` implements this container build for local
use. Production uses its VM build script, which also records the source
revision required by `deploy_prebuilt.sh`.

The example keeps AOT runtime objects in `skindicate.dia/dist/aot-cache/`,
outside the disposable source copy. Diamond fingerprints the runtime inputs,
so the cache remains safe when a new source copy has older timestamps. The
Diamond CLI's `make release` step still rebuilds on each container run, while
the AOT runtime compilation is reused. Delete `dist/aot-cache/` when that
cache is no longer wanted.

**One more thing a cross-build must override:** the Makefile's own `release`
target defaults `CFLAGS_RELEASE` to `-march=native` (right for the common
case -- no distro packaging exists yet, so whoever builds Diamond is
building it for the machine they're going to run it on). A container only
isolates the OS/libraries, not the CPU, so building inside one still runs on
the *host's* actual CPU -- inheriting that default would bake the build
machine's exact CPU features into a binary meant for a different one.
`build_vm.sh` and `build_ubuntu.sh` pass `make CFLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v3"
release` instead: `x86-64-v3` is a named, standardized ISA tier (AVX2/BMI2/
FMA/LZCNT/MOVBE), not one CPU's exact feature set -- supported by any real
x86_64 machine from roughly 2013 (Intel Haswell) or 2015 (AMD Excavator)
onward, recovering the same speedup `-march=native` would give a local
build (confirmed directly: `bench/int_arithmetic.di`, ~3.9s/iter at the
portable `x86-64` baseline vs ~0.5s/iter at both `native` and `x86-64-v3` --
multi-precision arithmetic leans heavily on BMI2/ADX) without tying the
binary to whichever machine happened to compile it. Any cross-build script
needs the same explicit override -- verify the actual deploy target's CPU
flags first (`ssh target grep flags /proc/cpuinfo`, checking for `avx2
bmi2 fma`), don't just assume.

`x86-64-v3` isn't an arbitrary choice of override for this one script --
it's Diamond's stated practical minimum supported configuration (see the
Makefile's own `CFLAGS_RELEASE` comment and the README's system
dependencies section). A deploy target older than that is unsupported, not
just slower.

**Caveat:** matching the distro and release does not guarantee identical
package versions. A local VM or container can drift from the target's
`apt upgrade` history. Inspect the produced binary with `file` and `ldd`,
then smoke-test it on the target after upload.
