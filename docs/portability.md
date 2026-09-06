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

Not yet validated: a non-x86_64 architecture, and any BSD or Darwin libc.
Portability claims should not extend past what's actually been checked --
see docs/roadmap.md's "Explicitly deferred".

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
   functions). Fibers (`src/vm.c`, see docs/concurrency-internals.md) are
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
