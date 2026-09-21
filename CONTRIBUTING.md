# Contributing

Diamond is a personal research language (see [README.md](README.md)). This
file documents how work actually gets done in this repository -- for
future-me as much as anyone else -- not a generic open-source intake process.

## Before making a change

Read [the roadmap](docs/roadmap.md) for candidate work and explicitly deferred
directions. For behavior, use the relevant topic guide; the roadmap is not a
feature reference.

For anything beyond a small fix, skim
[docs/internal/design.md](docs/internal/design.md) and the relevant topic
guide under `docs/` before touching `src/` or `lib/core.di` -- README.md's
own "Learn more" section links the full set.

## Build and test

Required system dependencies:

- GCC 15+ or Clang 19+ with C23 support;
- OpenSSL development headers and libraries;
- SQLite3, PostgreSQL (`libpq`), and MariaDB/MySQL client development
  headers and libraries;
- zlib and libcrypt (`crypt(3)`) development headers and libraries;
- POSIX threads and `ucontext`, provided by the target Linux environment;
- On x86_64: a CPU supporting the `x86-64-v3` microarchitecture level
  (AVX2, BMI2, FMA -- roughly Intel Haswell/2013 or AMD Excavator/2015
  onward). `make`/`make release` default to `-march=native`; Diamond's
  arbitrary-precision integers (`src/bignum.c`) lean on BMI2/ADX heavily
  enough that going without them is roughly a 7x slowdown, not a
  rounding error.

```sh
# Fedora
sudo dnf install gcc openssl-devel sqlite-devel libpq-devel \
  mariadb-connector-c-devel libxcrypt-devel zlib-devel

# Ubuntu (confirmed against a real Ubuntu 26.04 install; Debian is assumed
# compatible from the same package names, not separately tested)
sudo apt install gcc libssl-dev libsqlite3-dev libpq-dev libmariadb-dev \
  libcrypt-dev zlib1g-dev

# Alpine (musl; also needs libucontext for Fiber support -- see
# docs/portability.md for what else differs on musl)
apk add gcc musl-dev openssl-dev sqlite-dev libpq-dev mariadb-connector-c-dev \
  zlib-dev libucontext libucontext-dev

# FreeBSD (Clang is the base cc; GNU Make is gmake, not make)
pkg install gmake sqlite3 openssl postgresql16-client mariadb-connector-c

# macOS (Clang only -- there is no system GCC)
brew install openssl@3 sqlite postgresql@16 mariadb-connector-c
```

Clang works as a drop-in `$(CC)` substitute on Fedora and Ubuntu
(`make CC=clang debug`) and is required separately for the fuzz targets
(`make fuzz`), which always build with Clang regardless of `$(CC)`
(`-fsanitize=fuzzer` is Clang/LLVM-only). Alpine has only been validated
with GCC; FreeBSD and macOS have only been validated with Clang -- see
docs/portability.md for exactly what's been checked on each platform.

```sh
make            # debug build (default)
make release    # -O3, no assertions
make sanitize   # ASan/UBSan
make tsan       # ThreadSanitizer
make lsp        # build/diamond-lsp
```

Test targets, from fastest/narrowest to slowest/broadest:

```sh
make test           # debug build + native/case suite -- run this before every commit
make test-lsp        # tests/lsp_test.sh, if lsp/ changed
make test-repl        # tests/repl_test.sh, if src/repl.c changed
make test-<package>   # e.g. make test-rack-package -- if one package changed
make test-all          # debug, release, sanitizers, packages, LSP, REPL, fuzz smoke,
                        # self-host bootstrap -- slow; CI runs this, not every local commit
make test-self-host    # ~1400-case lexer/parser differential corpus -- periodic, not per-push
```

At minimum, `make test` must pass before a commit. If the change touches
`lsp/`, also run `make test-lsp`.

`make test` and `make test-all` require local loopback networking. The
native suite starts real TCP, UDP, and TLS listeners, and several package
suites start HTTP and WebSocket servers, all bound only to
`127.0.0.1`/`localhost`; they do not need public Internet access. Run these
targets outside any OS/container or agent sandbox that denies socket
creation, binding, or loopback connections. An error such as
`TCPServer.listen ... Operation not permitted` indicates outer sandbox
policy, not a Diamond test failure -- independent of Diamond's own
`--sandbox` mode, which the suite exercises separately.

`make sanitize`/`test-sanitize` run with LeakSanitizer enabled
(`ASAN_OPTIONS=detect_leaks=1`) by default. Under ptrace-restricted
containers, including GitHub's own CI runners, set
`ASAN_OPTIONS=detect_leaks=0` before invoking `make test-sanitize` (an
already-set `ASAN_OPTIONS` always wins over the Makefile's own default).

Self-hosting is in minimal-compat maintenance mode (see docs/roadmap.md):
`test-all` only confirms the self-hosted frontend still parses and runs
itself, not full parity. `make test-self-host` (~1400 cases) is periodic,
not per-push -- it's dominated by the self-hosted parser re-parsing
`lib/core.di` through the interpreter on every case.

## Code style

There is no `.clang-format` (the existing style is dense and specific --
see any file under `src/` or `lsp/`) and no linter gate; match the
surrounding file by eye. In brief: minimal whitespace (`if(x){y;}`, not
`if (x) { y; }`), `nullptr`/`constexpr`/C23 features used freely, comments
that explain *why* (a constraint, a prior incident, a non-obvious
invariant) rather than *what* the code already says. A comment that's
gone stale relative to the code it describes is treated as a real defect,
not cosmetic -- see how often existing comments cite a specific empirical
result, a specific prior bug, or a specific alternative considered and
rejected.

Diamond-language style (`lib/`, `packages/`, `selfhost/`) follows
[docs/internal/di-modernization-audit.md](docs/internal/di-modernization-audit.md)'s
findings: prefer receiver syntax (`values.map(f)`) over the equivalent
free function (`array_map(values, f)`) at any call site being touched
anyway, compound assignment over `x = x + 1`, and `unless`/truthiness
over `if x != nil` only where the nil-vs-false distinction is genuinely
irrelevant.

## Commits

Imperative, present-tense summary line (`Add X`, `Fix Y`, not `Added`/
`Fixed`); a package-scoped change is often prefixed (`packages/rack: add
ContentNegotiation`). Explain *why* in the body when it isn't obvious from
the summary -- `git log` is read far more often than it's written. Keep a
commit to one logical change; split unrelated work into separate commits
even when it landed in the same session.

## Documentation and the changelog

Three different places, three different jobs -- don't blend them:

- **`docs/*.md`** -- current, accurate behavior. Update the relevant guide
  in the same commit as the behavior change it describes.
- **`CHANGELOG.md`** -- a concise, user-visible capability milestone under
  `## Unreleased`, not every implementation step. See its own "Maintenance
  policy" section.
- **`docs/roadmap.md`** -- forward-looking only. When roadmap work lands:
  document the behavior (above), record the changelog milestone (above),
  then remove the completed item from the roadmap itself -- see its own
  "Completion policy" section. A roadmap entry that stays stale after the
  work is done is worse than no entry.

## Scope discipline

This repo's own history is unusually candid about cutting scope
deliberately (see docs/roadmap.md's "Explicitly deferred" section, and
how often a commit message says what it *didn't* do and why). Prefer that
same discipline: a fix doesn't need an adjacent refactor, a new API
doesn't need speculative flexibility nobody has asked for yet, and a
measured claim ("compile is 90-96% of wall time, measured on...") beats
an unmeasured one every time.
