# Deployment

`diamond build` compiles a `.d6` program into a standalone native executable:
no `diamond` interpreter, no `.d6` source file, and no recompilation step at
run time. It exists for shipping a finished Diamond program to a machine
that doesn't have (and shouldn't need) a Diamond checkout, alongside the
ordinary `diamond program.d6` interpreter path.

```sh
./build/diamond build app.d6 -o app
./app arg1 arg2
```

You can also invoke the Diamond binary from the application directory:

```sh
cd ../skindicate.dia
../diamond/build/diamond build app.d6
```

## CLI contract

```text
diamond build SOURCE [-o OUTPUT] [--cc=COMPILER] [--target=TRIPLE] [--lto]
```

- `SOURCE` is compiled exactly the way `diamond program.d6` compiles it --
  identical `require` resolution, identical embedded-prelude-template fast
  path -- so anything that runs correctly under the interpreter compiles
  identically under `build`.
- `-o OUTPUT` names the produced binary. Defaults to `SOURCE`'s own basename
  with its final extension stripped (`app.d6` -> `app`, and legacy
  `app.di` -> `app`). A basename without an extension is unchanged.
- `--cc=COMPILER` overrides the C compiler used to link the binary (`gcc`,
  `clang`, a full path, ...). Defaults to the compiler recorded in the
  installed AOT kit, or to whatever `$(CC)` resolves to for an ordinary
  `make` invocation in a checkout.
- `--target=TRIPLE` links for another platform from a cross kit built with
  `make aot-kit-cross` (see "Cross-compilation" below). It never falls back to the
  host toolchain: a missing or mismatched kit exits 74.
- `--lto` links the kit's LTO runtime (see "Optional LTO runtime" below) instead of the
  default one. It needs an installed kit built with `AOT_LTO=1`; without one it exits 74
  and says how to build it. It does not combine with the checkout's `make` path.
- A compile error (a real syntax/type/name error in `SOURCE` or anything it
  `require`s) prints the same diagnostic `diamond program.d6` would and
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

### Optional LTO runtime

The default runtime is compiled at `-O2` without link-time optimization, so `diamond build`
links in a fraction of a second. `make aot-kit AOT_LTO=1` (or `make install AOT_LTO=1`) adds a
second archive, `libdiamond-aot-lto.a`, holding the same sources at `-O3` as fat LTO objects,
plus `compile-lto.args`; `diamond build --lto` links against that, so the compiler optimizes
the runtime and the program together. Measured on five `bench/` programs against the default:
`-O2` with LTO is about 2% slower and `-O3` with LTO about 3% faster (instructions and cycles
both), and every `--lto` build recompiles the runtime in that link, about 38 s instead of
0.2 s. That is why it is opt-in at both ends. The compiler needs LTO support (GCC's works with
plain `ar`; Clang needs a linker plugin or lld).

## JIT in standalone programs

Built executables use the same opt-in JIT settings as the interpreter:

```sh
DIAMOND_JIT=1 ./app
DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_TRACE_JIT=1 ./app
```

The default compilation threshold is 50 calls per eligible function; a function
containing a loop is compiled by its first call, since a loop in a function called
once would otherwise run interpreted to completion. The
JIT runs only on glibc x86-64; on other platforms the executable stays on
the interpreter path. `DIAMOND_TRACE_JIT=1` prints compiled-function and
bailout counts when the program returns normally; a server terminated by a
signal may not emit that summary. The JIT remains off when `DIAMOND_JIT` is
unset, including for standalone binaries.

At the language level, the produced binary behaves exactly like
`diamond program.d6`: it sets `ARGV` from its own command-line arguments and
prints its top-level result the same way (`diamond_value_print`, the same
function `diamond program.d6` itself uses -- e.g. a program whose last
expression is a bare string prints that string, with no explicit `puts`
needed).

## What this is not

- **Not a fully static/hermetic binary by default.** (A static musl kit
  produces one; see "Static musl binaries" below.) The produced executable still
  dynamically links whatever `diamond` itself links against on this
  platform -- OpenSSL, SQLite3, `libpq`, the MariaDB client library, zlib,
  `libcrypt`, and their own transitive dependencies (confirmed with `ldd`
  against a real build). "No separate interpreter, source file, or
  recompilation step" is the actual guarantee; the binary is exactly as
  portable across machines as `diamond` itself is, no more and no less.
- **Cross-compilation needs a prepared kit.** Without `--target` the binary targets
  the machine `diamond build` ran on; see "Cross-compilation" below.
- **The Diamond build checkout is still needed at build time.** `diamond
  build` invokes its checkout's `make aot-build` target to link the binary;
  there is no separate installed runtime library yet. The finished binary
  has no dependency on the checkout at run time and can run from any
  working directory, including on a machine with no Diamond checkout.

See [docs/roadmap.md](roadmap.md) for what's explicitly deferred here
(static linking).

## Cross-compilation

`diamond build app.d6 --target=aarch64-linux-gnu` produces a binary for another
platform. The compiled program is portable between targets with the same word size
and byte order (the program image records both and a mismatched image is refused when
it loads); what changes is the runtime, which must be compiled and linked for the
target together with the target's own OpenSSL, SQLite3, `libpq`, MariaDB client, zlib
and `libcrypt`. A *cross kit* holds that runtime, built once per target:

```sh
make release
make aot-kit-cross TARGET_TRIPLE=aarch64-linux-gnu SYSROOT=/opt/sysroots/aarch64 \
     CROSS_FLAGS_EXTRA='--gcc-install-dir=/opt/sysroots/aarch64/usr/lib/gcc/aarch64-redhat-linux/16'
./build/diamond build app.d6 --target=aarch64-linux-gnu
```

- A cross build needs a C23 compiler that targets the triple (clang 19+ with
  `--target`, default `CROSS_CC=clang`) and a **sysroot**: the target's C library,
  headers, and the libraries above. A distribution's own aarch64 packages extracted
  into a directory work; `CROSS_FLAGS_EXTRA` passes anything else the driver needs
  (`--gcc-install-dir`, `-fuse-ld=lld`).
- The kit lands in `build/aot-targets/TRIPLE`; `make install` copies every such kit
  to `PREFIX/lib/diamond/aot/targets/TRIPLE`, which an installed `diamond` searches
  first. The kit records the sysroot path, so the sysroot must still exist where
  `diamond build --target` links.
- The prelude snapshot is generated by the host build and reginold is rebuilt from a
  scratch copy for the target, so the host build is left alone.
- The JIT is x86-64/glibc only, so other targets run on the interpreter.
- Test a target with `tests/cross_build_test.sh` (`make test-cross-build`); set
  `DIAMOND_CROSS_SYSROOT`, `DIAMOND_CROSS_TARGET`, `DIAMOND_CROSS_FLAGS_EXTRA` and
  optionally `DIAMOND_CROSS_RUN` (an emulator such as `qemu-aarch64-static -L SYSROOT`)
  to build and run a real cross binary.

Unless the kit is static (below), the built binary links the target's shared libraries
dynamically, so it is as portable across machines as that sysroot's libraries are.

### Static musl binaries

A musl target plus `STATIC=1` produces a fully static executable with no dependency on
the target machine's libc, OpenSSL, SQLite, `libpq` or zlib. It runs on any Linux of that
architecture, including a `FROM scratch` container:

```sh
make aot-kit-cross TARGET_TRIPLE=x86_64-alpine-linux-musl SYSROOT=/opt/sysroots/alpine \
     NO_MYSQL=1 STATIC=1 \
     CROSS_FLAGS_EXTRA='--gcc-install-dir=/opt/sysroots/alpine/usr/lib/gcc/x86_64-alpine-linux-musl/15.2.0 -rtlib=libgcc --unwindlib=libgcc'
./build/diamond build app.d6 --target=x86_64-alpine-linux-musl
```

- The sysroot needs static libraries: `libc.a`, `libssl.a`/`libcrypto.a`, `libsqlite3.a`,
  `libz.a`, `libpq.a` with `libpgcommon_shlib.a`/`libpgport_shlib.a`, `libucontext.a`
  (musl has no `ucontext`), and `libgcc.a`. An Alpine root with `musl-dev`,
  `openssl-libs-static`, `sqlite-static`, `zlib-static`, `libpq-dev`, `libucontext-dev`,
  `libgcc-static`, `linux-headers` and `gcc` provides all of them. `STATIC_LDLIBS`
  overrides the extra archives for a sysroot that names them differently.
- **MySQL is left out** (`NO_MYSQL=1`): MariaDB Connector/C has no static build in Alpine.
  `MySQL.open` raises a `MySQLError` ("MySQL support is not compiled into this build")
  and `-lmariadb` is not linked. `NO_POSTGRES`, `NO_SQLITE`, `NO_ZLIB`, `NO_TLS` and `NO_CRYPTO` do
  the same for those libraries, and `MINIMAL=1` sets all six; they work for ordinary builds too.
- The binary reads the host's CA certificates for TLS (`/etc/ssl`), so a minimal image
  still needs a certificate bundle for outbound HTTPS.
- Static glibc is not offered: its name-service lookups load shared libraries at run
  time. Use musl for static output.

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
build (confirmed directly: `bench/int_arithmetic.d6`, ~3.9s/iter at the
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
