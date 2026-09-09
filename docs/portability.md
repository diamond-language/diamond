# Portability

This document records which OS/libc/kernel/toolchain assumptions Diamond's
native implementation actually depends on, where each one lives in the code,
and what's been verified versus merely assumed. It exists because "should
build on Linux" is not a claim worth making without checking what "Linux"
is doing the load-bearing work -- glibc and musl, in particular, disagree on
enough POSIX-adjacent surface area to matter here.

## Validated platforms

- **Fedora and Ubuntu 26.04, GCC and Clang, x86_64 (glibc)** -- CI runs the
  full test suite (`make test-all`) across this 2x2 matrix on every push
  (`.github/workflows/ci.yml`, `test-all` job).
- **Alpine (musl), GCC, x86_64** -- CI runs `make test` plus a curated set
  of focused targets (`test-musl` job, same workflow) on every push. 1283
  of 1285 `tests/cases/*.di` corpus cases pass; the two known failures
  (`bcrypt.di`, `active_record_secure_password.di`) are excluded from the
  CI run itself rather than left to fail it -- both are the same real,
  documented libc limitation (BCrypt, below), not a bug. Narrower than
  `test-all`: GCC only (Clang-on-musl has not been checked), and no
  sanitizer/tsan builds or package-specific tests (database, HTTP,
  GraphQL, ...), since those have not actually been validated against
  musl either -- see the `test-musl` job's own comment for why extending
  its scope to match `test-all` isn't done casually. See "musl-specific
  findings" below for exactly what building there required.
- **FreeBSD 15.1, Clang, x86_64** -- CI runs `make test` plus the same
  curated focused-target set as musl (`test-freebsd` job, same workflow),
  via a real QEMU-booted VM (`vmactions/freebsd-vm`), on every push. Four
  known failures excluded the same way musl's two are: `bcrypt.di`/
  `active_record_secure_password.di` (BCrypt.hash needs libxcrypt's
  `crypt_gensalt_rn`, unavailable with no `<crypt.h>` at all on FreeBSD --
  see "FreeBSD/OpenBSD" below) and `math_exp_log(_tanh).di` (exp/log
  differ from glibc's own libm in the last representable bit -- expected
  cross-library variance, not a bug). Narrower than `test-all` for the
  same reason musl's own job is: no sanitizer/tsan builds or
  package-specific tests validated against FreeBSD either. GCC-on-FreeBSD
  has not been checked (CC=clang here, FreeBSD's own base compiler, to
  avoid an extra ports build).

Not yet validated: a non-x86_64 architecture, and OpenBSD (blocked on a
real gap, not just an unwritten CI job -- see below). macOS/Darwin's
build now compiles clean on real hardware, but has no CI job yet -- the
test harness's own coreutils dependencies aren't fixed (see below), so a
real `test-macos` job would need those first. Portability claims should
not extend past what's actually been checked -- see docs/roadmap.md's
"Explicitly deferred". See
"macOS/Darwin" and "FreeBSD/OpenBSD" below for what's been found so far.

## Toolchain assumptions

- **C23**, via `-std=c23`: `nullptr`, `constexpr`, `#embed`, `typeof`,
  `thread_local` as a keyword. Requires GCC 15+ or Clang 19+. `#embed`
  specifically is load-bearing (`src/prelude.c`, `src/compiled_prelude_data.c`)
  -- there's no fallback path for a pre-C23 compiler.
- **`-fPIE` is explicit** (`Makefile`'s `CFLAGS_COMMON`), not left to the
  compiler's own default. Fedora/Ubuntu's gcc pair `-fPIE`-at-compile-time
  and `-pie`-at-link-time consistently either way, so this was invisible
  there. Alpine's musl-targeting gcc does not: it links `-pie` by default
  without defaulting to `-fPIE` at compile time, so any object compiled
  without it fails to link at all (`relocation R_X86_64_32 against
  '.rodata' can not be used when making a PIE object`). Forcing `-fPIE`
  unconditionally makes every object agree regardless of a given
  toolchain's own default pairing.
- **`reginold/libreginold.a` needs `-fPIC`** (`reginold/Makefile`) --
  found on Ubuntu, unrelated to this pass; already fixed, kept here since
  it's the same category of "PIC/PIE default varies by toolchain" issue.

## Feature-test macros (glibc vs. musl)

Both libcs gate POSIX-beyond-C23 declarations behind feature-test macros
(`_DEFAULT_SOURCE`, `_POSIX_C_SOURCE`, `_XOPEN_SOURCE`, `_GNU_SOURCE`), but
their *defaults* differ in exactly the case that matters here:

- glibc, with no macro defined at all, still exposes a workable default
  set regardless of `-std=cNN`.
- musl's `<features.h>` only falls back to a working default
  (`_BSD_SOURCE` + `_XOPEN_SOURCE=700`) when `__STRICT_ANSI__` is *not*
  defined. GCC/Clang define `__STRICT_ANSI__` for any strict standard mode
  (`-std=c23`, not `-std=gnu23`) -- which this project uses -- so a
  translation unit with no explicit feature-test macro gets none of musl's
  fallback, and any POSIX declaration (`ucontext_t`, for one) is simply
  absent.

The practical effect: every `.c` file that reaches `vm.h` (directly or
transitively, since `vm.h` itself does `#include <ucontext.h>`) needs its
own `#define _DEFAULT_SOURCE` (or an equivalent macro) *before its own
first `#include`* -- not merely before its own `#include "vm.h"` if some
earlier header already reaches a libc header first, and not centralized
in `vm.h` itself. Both restrictions come from the same mechanism: a
feature-test macro's effect on a given header (`<signal.h>`'s own
`#if defined(_BSD_SOURCE) || ...`) is decided the first time *that header*
is processed in the translation unit, gated by its own separate include
guard -- defining the macro after that point, even from inside a
supposedly-shared header, has no effect. This project's own established
pattern (`src/main.c`, `src/vm.c`, `src/repl.c`, `src/run_source.c`,
`src/compiler.c`, `src/loader.c` each already defined their own macro,
before this pass, for their own reasons) turned out to be the *only*
correct place to put this, not a stylistic inconsistency to clean up.
Files fixed in this pass for the same reason: `src/bignum.c`,
`src/disassemble.c`, `src/value.c`, `src/compiled_prelude.c`,
`tools/gen_compiled_prelude.c`, `lsp/completion.c`, `lsp/definition.c`,
`lsp/diagnostics.c`, `lsp/document_symbol.c`, `lsp/references.c`,
`lsp/receiver.c`, `tests/api_invalidation.c`, `tests/fiber_states.c`,
`tests/incremental_compile_test.c`, `fuzz/compile_fuzzer.c`.

## musl-specific findings

Building and testing on Alpine surfaced four real, distinct issues beyond
the feature-test-macro gap above:

1. **`ucontext_t`/`swapcontext`/`getcontext`/`makecontext` are declared by
   musl's headers but not implemented in Alpine's shipped `libc.so`** --
   confirmed directly (`nm -D` on `libc.so` shows none of the three
   functions). Fibers (`src/vm.c`, see docs/internal/concurrency-internals.md) are
   built entirely on this POSIX API, and `run_chunk`'s own yield-opcode
   handler references it unconditionally, so the whole binary fails to
   *link*, not merely to run fibers, once the feature-test-macro gap above
   is fixed. Resolved by linking `libucontext` (`apk add libucontext
   libucontext-dev`, then `-lucontext`), a small compatibility library
   providing all four under their standard names as weak symbols -- no
   source change needed once linked, and confirmed to actually *work*
   (not just link): a real yield/resume fiber program runs correctly
   under it. This is a real, external build-time dependency for musl
   specifically, not something to route around in Diamond's own code.
2. **`crypt_gensalt_rn`/`CRYPT_GENSALT_OUTPUT_SIZE` don't exist on musl at
   all** -- these are libxcrypt-specific convenience functions (Fedora and
   Ubuntu both link against libxcrypt, not a bare glibc `crypt`) for
   generating a bcrypt salt string. `BCrypt.hash` (`src/vm.c`,
   `bcrypt_hash_helper`) used them directly. Worse than a missing
   symbol, though: **musl's own `crypt_r` doesn't implement the bcrypt
   algorithm (`$2b$`) at all**, confirmed directly (`crypt_r("x",
   "$2b$04$...", &data)` returns `"*"`, libcrypt's own
   unsupported-algorithm signal, while the identical call with `$6$`
   (SHA-512) succeeds) -- so a portable salt generator alone would not
   have been enough. `bcrypt_hash_helper` now checks for
   `CRYPT_GENSALT_IMPLEMENTS_AUTO_ENTROPY` (a capability macro libxcrypt's
   own `crypt.h` defines, and the same one this function's own comment
   already used for a different purpose) at compile time, and returns a
   clear runtime error ("BCrypt.hash is not supported on this platform's
   crypt() implementation") when it's absent, rather than failing to
   compile. `BCrypt.verify` needed no equivalent change: it was already
   written to treat any `crypt_r` failure as an ordinary non-match rather
   than an exception, so it degrades safely (always reports "no match")
   without modification -- confirmed correct, not just convenient, since
   verifying against an unsupported hash format is exactly the case that
   existing behavior was already meant to cover. A fully portable BCrypt
   would need to bundle its own algorithm implementation instead of
   delegating to the system `crypt(3)`; not attempted here (see
   docs/roadmap.md).
3. **Reassigning the global `stdout`** (`src/repl.c`, output capture
   during REPL candidate evaluation) doesn't even compile on musl:
   `stdout` is a plain, reassignable `FILE *` global on glibc, but POSIX
   only guarantees it names *some* `FILE *` expression, and musl's own
   `<stdio.h>` defines it as a non-assignable macro. Fixed by redirecting
   the underlying file descriptor with `dup2(fileno(capture),
   STDOUT_FILENO)` instead of touching the `stdout` variable at all --
   every write through the untouched `stdout` `FILE*` (including inside
   the user's own running code) still targets fd 1, which now happens to
   point at the capture file; restored the same way afterward. Portable
   by construction, not glibc-specific behavior relied on elsewhere.
4. **Alpine's base image ships no `tzdata`, no GNU `sed` (BusyBox's own
   `sed` chokes on this project's own `{...}` block syntax with
   "unterminated {"), and no `bash`/`coreutils`/`procps` by default** --
   all external environment-setup gaps, not code bugs, exactly like the
   Fedora-vs-Ubuntu MariaDB/PostgreSQL header-path differences already
   handled in `Makefile`'s own `CPPFLAGS`. Installed via `apk add tzdata
   sed bash coreutils procps` for this validation; would need the same
   treatment in any CI job targeting musl. Also worth noting for whoever
   next validates this on a *container* specifically (not just a musl
   host): a container with no real init process (PID 1 that reaps
   orphans) can make an unrelated, environment-only "zombie processes
   left behind" test failure look like a Diamond bug -- confirmed by
   re-running the identical test under `podman run --init`, which passed
   cleanly. `docker run --init` / `podman run --init` (or an
   equivalent minimal reaper) is a real prerequisite for that specific
   test, unrelated to musl itself.

## macOS/Darwin: build-level prerequisites confirmed, test harness still open

The build itself is now confirmed against a real GitHub-hosted
`macos-latest` runner (arm64, Apple Silicon, Xcode 26.6, Darwin 25) --
`src/vm.c` compiles cleanly with the real project headers. The test
harness's own GNU-coreutils dependencies (below) are not yet
addressed, so there's still no real `test-macos` CI job -- only a
build-level one would be honest right now.

- **Two `Makefile` fixes**, independent of whether a Darwin CI job ever
  lands: `CFLAGS_RELEASE`'s `-march=x86-64` is an x86-only flag gcc/clang
  reject outright on arm64 -- what every current GitHub-hosted `macos-*`
  runner is -- now guarded by `uname -m` rather than assumed (this also
  matters on an arm64 Linux box, not just macOS). `LDLIBS`'s `-lcrypt`
  fails to link on macOS (`crypt()` lives in libSystem directly, no
  matching `.dylib`) -- confirmed directly; **`-ldl` was wrongly assumed
  to fail the same way and doesn't** (it links fine on this real runner --
  an earlier version of this doc asserted otherwise from general
  knowledge, not a real check, and was wrong). Both are their own
  overridable `LDLIBS_DL`/`LDLIBS_CRYPT` variables regardless, alongside
  `CPPFLAGS_EXTRA`/`LDFLAGS_EXTRA` override points for Homebrew's
  keg-only OpenSSL/libpq/MariaDB Connector paths (`/opt/homebrew` on
  Apple Silicon).
- **`<ucontext.h>` is not actually absent** (unlike OpenBSD) -- Apple's
  own SDK header gates `getcontext`/`makecontext`/`swapcontext`/
  `ucontext_t` behind an explicit `#error` unless `_XOPEN_SOURCE` is
  defined ("The deprecated ucontext routines require _XOPEN_SOURCE to be
  defined"), confirmed directly. Fixed the same way this codebase already
  handles `_DEFAULT_SOURCE`/`_GNU_SOURCE` (`Feature-test macros` above):
  added to the same 22 files. Still compiles with deprecation warnings
  (not fatal, no `-Werror` covering them) -- Apple discourages these
  routines but hasn't removed them.
- **Defining `_XOPEN_SOURCE` alone breaks FreeBSD**, confirmed the hard
  way: it landed on `main` once, broke `test-freebsd` immediately
  (`MAP_ANONYMOUS`/`SO_REUSEPORT`/`_SC_NPROCESSORS_ONLN`/`crypt_r`/
  `struct crypt_data` all became undeclared -- FreeBSD's `<sys/cdefs.h>`
  disables `__BSD_VISIBLE` by default once `_XOPEN_SOURCE` is defined),
  and was reverted the same day. `__BSD_VISIBLE=1` restores it on
  FreeBSD; it means nothing to Apple's headers, which needed their own
  equivalent, `_DARWIN_C_SOURCE`, to restore the identical set of hidden
  declarations there (plus `timegm`/`NI_MAXHOST`/`NI_MAXSERV`). All three
  macros -- `_XOPEN_SOURCE 700`, `__BSD_VISIBLE 1`, `_DARWIN_C_SOURCE` --
  now sit alongside `_DEFAULT_SOURCE` in the same 22 files; each is a
  no-op where its own libc doesn't recognize the name (glibc/musl ignore
  the BSD/Darwin ones the same way FreeBSD/macOS ignore `_DEFAULT_SOURCE`
  itself).
- **`struct crypt_data`/`crypt_r` don't exist on macOS at all** -- neither
  declared in any header, confirmed directly ("incomplete type 'struct
  crypt_data'"). No portable feature test predicts this the way
  `__has_include(<crypt.h>)` did for the top-of-file include: FreeBSD
  also lacks `<crypt.h>` but *does* declare both (in `<unistd.h>`), so
  that same check would have wrongly disabled this on FreeBSD too.
  `bcrypt_verify_helper`'s use of them is now gated on `__APPLE__`
  specifically -- the one deliberate exception in this codebase to the
  "no OS-name branching" pattern documented below, because no feature
  test exists for this particular gap. Degrades the same already-
  documented way an unsupported algorithm does on musl: `BCrypt.verify`
  never raises, so an unresolvable digest is just reported as no match.
  (`BCrypt.hash` was already correctly excluded from ever reaching this
  code on macOS -- no `<crypt.h>` there either means
  `CRYPT_GENSALT_IMPLEMENTS_AUTO_ENTROPY` is never defined, same as
  FreeBSD/musl.)
- **Plain `crypt()` on macOS silently mishandles an unrecognized bcrypt
  salt** rather than returning `NULL` the honest way FreeBSD/OpenBSD/musl
  all do -- confirmed directly: `crypt("password", "$2b$04$...")`
  returned a 13-character traditional-DES-style string instead of an
  error. Harmless here (the length mismatch against a real 60-character
  bcrypt digest already makes `bcrypt_verify_helper`'s own comparison
  fail safely), but worth knowing if anything ever calls raw `crypt()` on
  macOS expecting a clean failure signal instead of a wrong-algorithm
  result.
- **The test harness (mostly `tests/run.sh`) leans on GNU-coreutils
  behavior far more than the build does** -- not yet addressed, still
  what's blocking a real `test-macos` job:
  - `timeout` -- 28 call sites across process/signal/socket tests, several
    relying specifically on GNU/uutils relay-then-child-status semantics
    (see the comment above the `signal_pid` wait in the Signal.trap test).
    Confirmed directly: macOS ships no `timeout` at all in its base
    install, and no `gtimeout` either unless `coreutils` is installed via
    Homebrew (also not preinstalled on the runner image checked).
  - `nproc` (6 uses) -- confirmed absent from macOS's base install.
    `sysctl -n hw.ncpu` works, but so does `getconf _NPROCESSORS_ONLN` --
    POSIX-portable, confirmed working identically on this macOS runner
    *and* every Linux/BSD already checked in this document, so it's a
    better replacement than per-OS `sysctl`/`nproc` branching would be.
  - `/proc/cpuinfo` (physical-core counting, same Thread real-parallelism
    check already guarded for FreeBSD/BSD above) -- no `/proc` on macOS
    either, same fix already applies.
  - `$EPOCHREALTIME` (that same test's own timing) -- bash 5+ only.
    Confirmed directly: macOS's `/bin/bash` is the pre-GPLv3 3.2 Apple
    still ships, **and it's what `which bash` resolves too** -- no
    Homebrew bash is preinstalled or put ahead on `PATH` by the runner
    image, unlike FreeBSD/OpenBSD where installing one via `pkg`/`pkg_add`
    was enough on its own. A real fix here needs the same explicit
    symlink-into-a-prepended-`PATH`-directory technique already used for
    FreeBSD's `gsed`, not just a `brew install bash`.
  - `bc` -- confirmed present (`/usr/bin/bc`) and functional; the earlier
    version of this doc's uncertainty here was unfounded.
  - `sed` -- confirmed BSD sed (rejects `--version`), same GNU-extension
    risk already documented for FreeBSD/musl; not yet checked against
    `tests/collection_relay_contracts.sh`'s specific `:label`/`n`/`p`/`b`
    loop the way FreeBSD's failure was, but likely the identical issue.
    No `gsed` preinstalled either.
  - Homebrew package names for a future CI job's own dependencies,
    confirmed present on the runner image already: `openssl@3`, `sqlite`.
    `postgresql@16` and `mariadb-connector-c` install cleanly via `brew
    install` (used to verify the `vm.c` compile above) -- their headers
    land under `$(brew --prefix postgresql@16)/include` and both
    `$(brew --prefix mariadb-connector-c)/include` and that same prefix's
    `include/mariadb` subdirectory (the mysql.h split already familiar
    from FreeBSD's own port).

  Unlike the musl job's own narrow carve-out (two BCrypt test cases,
  `test-musl`'s own comment), excluding everything `timeout` touches here
  would gut most of the process/signal/socket coverage -- there's no small
  subset of `make test` that dodges this cleanly. A real `test-macos` job
  needs the coreutils gaps above fixed in `tests/run.sh` first, not just
  Homebrew packages installed, to be worth more than the build-only
  compile check already confirmed.

## FreeBSD/OpenBSD: confirmed against real VMs

Unlike the macOS section above, this was checked against real, current
kernels -- FreeBSD 15.1-RELEASE and OpenBSD 7.9, both amd64, each booted
for real via `vmactions/freebsd-vm`/`vmactions/openbsd-vm` (QEMU, not
emulation of the userland alone). FreeBSD now has a real, passing CI job
(`test-freebsd`, see "Validated platforms" above); OpenBSD does not, for
the real reason below, not lack of effort.

What it took to get FreeBSD's job from a first real build attempt to
fully green, beyond the prerequisite findings below (each its own commit
on the way there, `git log -- .github/workflows/ci.yml src/vm.c
tests/run.sh`):

- `gmake`, not `make`: FreeBSD's base `make` is BSD make (pmake), which
  doesn't understand this Makefile's GNU-only syntax (`ifeq`,
  `$(wildcard ...)`, `$(shell ...)`) at all.
- `mysql.h`/`libmariadb.so` both live under FreeBSD's mariadb-connector-c
  port's own `mariadb/` subdirectory (`/usr/local/include/mariadb/`,
  `/usr/local/lib/mariadb/`), on top of the plain `/usr/local` prefix
  everything else needs -- the same split Debian/Ubuntu's `libmariadb-dev`
  needs on Linux (`Makefile`'s own `CPPFLAGS` comment), just a different
  extra path.
- `CPPFLAGS_EXTRA`/`LDFLAGS_EXTRA` (added for macOS, see above) needed one
  real `Makefile` fix to actually work everywhere: most link recipes
  (`gen_compiled_prelude` on) use `$(LDLIBS)` alone, never `$(LDFLAGS)` --
  only the final `diamond` binary and the API test binaries link with
  `$(LDFLAGS)`. `LDFLAGS_EXTRA`'s `-L` paths are now prepended into
  `LDLIBS` itself instead, reaching every recipe uniformly.
- `tests/collection_relay_contracts.sh` uses a GNU sed extension (a
  `:label`/`n`/`p`/`b` loop) FreeBSD's base BSD sed rejects outright --
  same category as musl/Alpine's own BusyBox sed finding above. FreeBSD's
  GNU sed port installs as `gsed`, not a `sed` override, and
  `/usr/local/bin` sits *after* `/usr/bin` in FreeBSD's default `PATH`,
  so (unlike Alpine's `apk` package) installing it doesn't shadow the
  base `sed` on its own; the CI job symlinks it into a directory it
  prepends to `PATH` explicitly instead.
- **Two real runtime bugs, not tooling gaps, found this way** (both fixed
  in `src/vm.c`, both were invisible on Linux/musl):
  1. `tcp_listen_helper`/`udp_socket_helper`'s wildcard bind
     (`getaddrinfo(nullptr, port, {AF_UNSPEC, AI_PASSIVE})`) never set
     `IPV6_V6ONLY`. Linux's default `net.ipv6.bindv6only=0` makes an
     IPv6-wildcard bind already dual-stack, so an IPv4 client connecting
     to `127.0.0.1` reaches it with no code needed. FreeBSD (and OpenBSD)
     default `net.inet6.ip6.v6only` to `1` -- the identical bind only
     accepted IPv6 there, so an IPv4 loopback connect got plain
     `ECONNREFUSED`, nothing about the failure pointing at IPv6 at all.
     Fixed by disabling `IPV6_V6ONLY` explicitly on the v6 candidate,
     matching Linux's default rather than silently depending on it.
  2. `UDPSocket#receive(0)`: FreeBSD's `recvfrom` leaves the sender
     address entirely unpopulated (`ss_family` stays `AF_UNSPEC`) on a
     genuinely zero-length request, later failing `getnameinfo` with
     `EAI_FAMILY` ("Address family not recognized"). Linux populates it
     regardless of the requested length. Fixed by always requesting at
     least 1 byte from the kernel (satisfies FreeBSD's requirement to
     resolve the address) while still reporting zero bytes of data
     whenever the caller's own request was `receive(0)` -- `recvfrom`
     always consumes/discards the full datagram regardless of buffer
     size, so this doesn't change `receive(0)`'s documented "consumes
     without copying" contract.
- **Two real, expected (not fixed) platform differences**, excluded from
  the CI run the same way musl's own two BCrypt cases are (`test-freebsd`
  job's own comment):
  - `bcrypt.di`/`active_record_secure_password.di`: `BCrypt.hash` needs
    libxcrypt's `crypt_gensalt_rn`/`CRYPT_GENSALT_IMPLEMENTS_AUTO_ENTROPY`
    to generate a fresh salt, both declared only in libxcrypt's own
    `<crypt.h>` -- which doesn't exist on FreeBSD at all (see the
    `__has_include` guard below), so `bcrypt_hash_helper` always takes
    its "not supported on this platform" branch there. A different
    missing piece than musl's own gap (musl has `<crypt.h>` but not
    bcrypt algorithm support in `crypt_r` itself), same observable
    result. `BCrypt.verify` is unaffected -- no salt generation, just
    `crypt_r` against an existing digest, confirmed genuinely correct on
    FreeBSD (see below).
  - `math_exp_log(_tanh).di`: `exp(1.0)`/`exp(log(5.0))` differ from
    glibc's own libm in the last representable bit
    (`2.7182818284590455` vs `2.718281828459045`, `5.0` vs
    `4.999999999999999`). Not a bug in either library -- IEEE 754 only
    mandates correctly-rounded results for `+`/`-`/`*`/`/`/`sqrt`, not
    transcendental functions, so two conforming libm implementations
    disagreeing in the last ULP is expected.

The rest of this section is the original diagnostic probe's own
findings -- confirmed directly, not assumed, before any of the above was
attempted, and the prerequisites everything above sits on top of.

- **`ucontext_t`/`getcontext`/`makecontext`/`swapcontext`** (Fiber support,
  same dependency as the musl section above): work natively on FreeBSD, no
  compatibility library needed. **`<ucontext.h>` does not exist at all on
  OpenBSD** -- `fatal error: 'ucontext.h' file not found`, not merely an
  unimplemented symbol the way musl's case was. This is a real gap in
  Diamond's own Fiber implementation (`src/vm.c` depends on POSIX
  `ucontext` unconditionally), not a build/CI-tooling problem: "Known
  limitations" below already notes there's no fallback fiber
  implementation (e.g. hand-rolled `setjmp`/`longjmp` + manual stack
  switch) planned. OpenBSD Fiber/Thread support is out of reach without
  that separate engineering effort -- a CI job alone can't close this gap.
- **`crypt(3)` bcrypt support**: native and correct on both -- confirmed by
  hashing the same password with the same salt on each and getting the
  identical, valid `$2b$` hash back. (The probe's first run produced a
  false NULL on both platforms from a malformed 21-character test salt,
  not a real platform gap -- bcrypt salts are fixed at 22; re-run with a
  known-valid one confirmed this cleanly.)
- **`-ldl`**: links fine on FreeBSD. **Fails on OpenBSD** ("unable to find
  library -ldl") -- same category as macOS's finding above (`dlopen` lives
  in libc directly, no separate `.dylib`/`.so` to link against); the same
  `LDLIBS_DL=` override already added for macOS covers this.
- **`timeout`**: present in *both* base installs (unlike macOS, which has
  none). FreeBSD's accepts GNU-style long options
  (`--foreground`/`--kill-after`/`--preserve-status`/`--signal`, though not
  `--version`) -- and its actual relay/exit-status semantics against every
  real call site in `tests/run.sh` are now confirmed correct outright: the
  `test-freebsd` job runs all of them successfully (see the same
  GNU-vs-uutils distinction docs/portability.md's macOS section flags, for
  what to check if this ever needs revisiting). OpenBSD's is a minimal
  native dialect (`-fp`/`-k time`/`-s signal`, no long options at all) --
  meaningfully different, not just missing, and untested against real call
  sites given OpenBSD's own blocker above.
- **`nproc`**: present in FreeBSD's base (`/bin/nproc`) -- no fix needed
  there. Absent on OpenBSD, same as macOS; `sysctl -n hw.ncpu` works on
  both as the portable fallback.
- **`bc`**: present and functional on both.
- **`bash` 5+** (for `$EPOCHREALTIME`): not in either base install, but
  installs cleanly via `pkg`/`pkg_add` and resolves first on `PATH` ahead
  of any system shell once installed -- confirmed directly by `which
  bash` after install on both, unlike macOS's still-unconfirmed PATH
  ordering.
- **Toolchain**: both ship Clang as the base `cc` (FreeBSD 19.1.7, OpenBSD
  19.1.7); GCC is available on both via the package manager (FreeBSD ports
  as plain `gcc`, OpenBSD as `egcc` -- `gcc` there is reserved for an
  ancient bundled version). FreeBSD 15's package-managed base system
  (`pkgbase`) already has OpenSSL and SQLite dev packages installed by
  default in the image used; PostgreSQL/MariaDB Connector package names
  were not checked (grep only searched already-installed packages, not
  the full repository).

**Net assessment**: FreeBSD's `test-freebsd` job is real and green (see
"Validated platforms" above) -- every prerequisite checked out clean, and
the handful of real gaps it surfaced (two runtime bugs, two expected
platform differences) are now fixed or excluded, same as musl's own job.
OpenBSD is blocked on the missing `ucontext.h`, a language-runtime gap,
not a portability/tooling one; revisit only alongside (or after) a
non-`ucontext` Fiber implementation, not as a CI task on its own.

## Known limitations (not fixed, by design)

- **`BCrypt.hash`/`BCrypt.verify`** depend on the system `crypt(3)`'s own
  bcrypt support. Confirmed present via libxcrypt (Fedora, Ubuntu).
  Confirmed absent on musl (Alpine) -- `.hash` fails with a clear runtime
  error; `.verify` always reports no match. Unverified on other libcs
  (a classic BSD `crypt()` typically lacks bcrypt too; OpenBSD's own
  `crypt()` has it natively, since that's where bcrypt originated).
- **Fiber support requires a working POSIX `ucontext.h`** implementation,
  native or via a compatibility library (`libucontext` on musl). No
  fallback fiber implementation (e.g. a hand-rolled `setjmp`/`longjmp` +
  manual stack switch) exists or is currently planned.

## What hasn't been found (checked, not just assumed)

- No `#ifdef __linux__`/`__APPLE__`/`__FreeBSD__`/`_WIN32`-style OS
  branching exists anywhere in Diamond's own source (`src/`, `lsp/`,
  `tools/`, `tests/`, `fuzz/`) -- the vendored `reginold/vendor/onigmo/`
  regex engine has its own, unrelated to this.
- No Linux-only APIs (`epoll`, `eventfd`, `inotify`, `sendfile`, `prctl`,
  anything under `<linux/...>`) -- socket/pipe multiplexing uses plain
  POSIX `poll(2)` throughout (`src/vm.c`).
- No other glibc-only extensions beyond the crypt.h ones above
  (`memmem`, `asprintf`, `qsort_r`, `strverscmp`, `get_current_dir_name`,
  `program_invocation_name` and similar were all checked directly via
  `grep` -- none are used).
- `thread_local`/`mmap(MAP_ANONYMOUS)`/`mprotect`/`sigaction`/
  `getaddrinfo` are all used, all standard C23/POSIX, and all present
  and behave identically on both validated libcs.
