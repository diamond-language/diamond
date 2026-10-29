# diamond_run_source's own auto-dispatch (src/run_source.c) caches
# compiled bytecode next to a script's real on-disk path by default (see
# docs/caching.md) -- every test-* target here eventually runs the real
# `diamond` binary against tests/cases/*.d6 (directly from a shell
# script, not just through build/run_cases's own diamond_run_source_
# with_template, which never touches the cache at all regardless). A
# .d6c file left behind in tests/cases/ from one test run could silently
# serve stale bytecode to a *later* run whose compiler changed in some
# way the cache's own build fingerprint doesn't happen to catch --
# exactly the kind of regression this whole test suite exists to catch,
# so every recipe below always compiles fresh. Harmless for non-test
# targets (build/link steps never execute the resulting `diamond` binary
# at all).
export DIAMOND_NO_CACHE := 1

CC := gcc
REGINOLD_DIR := reginold
REGINOLD_LIB := $(REGINOLD_DIR)/libreginold.a
# -I/usr/include/mysql(/mysql): mariadb_config --cflags's own include path
# for MariaDB Connector/C (libmysqlclient-API-compatible) on Fedora, where
# libmariadb-devel installs mysql.h under /usr/include/mysql. Debian/Ubuntu's
# libmariadb-dev instead installs it under /usr/include/mariadb (confirmed
# against a real Ubuntu 26.04 container) -- hence the third -I below.
# -I/usr/include/postgresql: libpq-fe.h's location is itself distro-
# dependent, confirmed the hard way deploying to a real Ubuntu box after
# every prior build/test of this project happened on Fedora -- Fedora's
# libpq-devel installs it directly under /usr/include (so this extra -I
# was previously believed unnecessary, per this comment's own prior
# wording), but Debian/Ubuntu's libpq-dev installs it under
# /usr/include/postgresql instead. Harmless to add unconditionally on
# distros where it's not needed -- gcc/clang silently ignore a nonexistent
# -I path -- so there's no reason to special-case any of these per platform.
# -Ilsp: lsp/repl_completion.c includes lsp/completion.h/json.h directly for
# Tab-completion (see REPL_COMPLETION_SOURCES below) -- global rather
# than scoped to just that one file's own compile step, since no
# src/*.h/lsp/*.h basename collision exists to make that a risk.
# CPPFLAGS_EXTRA: empty by default, appended below -- same pattern as
# LDLIBS_EXTRA's own comment further down. Its one real use today: macOS's
# Homebrew keeps OpenSSL/libpq/MariaDB Connector keg-only (not symlinked
# into a default search path the way Fedora/Ubuntu's package managers
# install headers), so those need their Homebrew prefixes passed in
# explicitly rather than hardcoded here alongside the Linux-distro paths
# below -- Homebrew's own prefix differs by CPU architecture (/opt/homebrew
# on Apple Silicon, /usr/local on Intel), so it can't be a fixed path either.
CPPFLAGS_EXTRA :=
# Optional native dependencies. Each NO_<FEATURE>=1 builds without that library: the
# feature's entry point raises an error naming it, its -l flag is dropped, and its header
# is not needed. MINIMAL=1 turns all of them on (the WebAssembly build, a static musl build
# whose sysroot lacks a client library). See docs/internal/wasm.md and CONTRIBUTING.md.
#   NO_MYSQL     MariaDB/MySQL client     NO_POSTGRES  libpq
#   NO_SQLITE    SQLite                   NO_ZLIB      Gzip.compress / Gzip.decompress
#   NO_TLS       OpenSSL's TLS: TLSSocket and TLSServer raise
#   NO_CRYPTO    OpenSSL's libcrypto: Digest, HMAC, SecureRandom and Base64 use portable
#                code (src/portable_crypto.c); Cipher (AES-GCM) is unavailable
MINIMAL ?=
NO_MYSQL ?= $(MINIMAL)
NO_POSTGRES ?= $(MINIMAL)
NO_SQLITE ?= $(MINIMAL)
NO_ZLIB ?= $(MINIMAL)
NO_TLS ?= $(MINIMAL)
NO_CRYPTO ?= $(MINIMAL)
OPTIONAL_DEFINES := $(if $(NO_MYSQL),-DDIAMOND_NO_MYSQL) $(if $(NO_POSTGRES),-DDIAMOND_NO_POSTGRES) \
	$(if $(NO_SQLITE),-DDIAMOND_NO_SQLITE) $(if $(NO_ZLIB),-DDIAMOND_NO_ZLIB) \
	$(if $(NO_TLS),-DDIAMOND_NO_TLS) $(if $(NO_CRYPTO),-DDIAMOND_NO_CRYPTO)
# libssl needs libcrypto, and libcrypto alone serves the digest/cipher calls.
OPTIONAL_LIBS := $(if $(NO_SQLITE),,-lsqlite3) $(if $(NO_POSTGRES),,-lpq) \
	$(if $(NO_MYSQL),,-lmariadb)
OPENSSL_LIBS := $(if $(NO_TLS),,-lssl) $(if $(and $(NO_TLS),$(NO_CRYPTO)),,-lcrypto)
CPPFLAGS := -Isrc -Ilsp -I$(REGINOLD_DIR) -I/usr/include/mysql -I/usr/include/mysql/mysql \
	-I/usr/include/mariadb -I/usr/include/postgresql $(CPPFLAGS_EXTRA) \
	$(OPTIONAL_DEFINES)
# -fPIE: explicit, not left to the compiler's own default. Fedora/Ubuntu's
# gcc default to a consistent compile/link PIE pairing either way, so this
# was invisible there, but Alpine's musl-targeting gcc defaults to `-pie`
# at link time without defaulting `-fPIE` at compile time -- an object
# compiled under that mismatched default fails to link at all ("relocation
# R_X86_64_32 against `.rodata' can not be used when making a PIE object").
# Forcing -fPIE here makes every object agree regardless of a given
# toolchain's own default pairing; gcc's own driver adds the matching
# `-pie` at link time once it sees PIE-compiled objects, no separate
# LDFLAGS needed. See docs/roadmap.md's "Portability".
CFLAGS_COMMON := -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Werror=implicit-function-declaration -fPIE
# CC=clang needs debug's own optimization level bumped from -O0 to -O1,
# GCC doesn't -- see CFLAGS_SANITIZE's own -O1-vs-O0 comment below for the
# same underlying cause (run_chunk's giant per-opcode-case local set not
# getting stack-slot-coalesced at -O0). Measured via -fstack-usage: GCC's
# plain -O0 run_chunk frame is 60,176 bytes, comfortably clear of the
# depth(5000)/DIAMOND_MAX_CALL_DEPTH=95 guard the comment below describes
# tripping under GCC's *ASan-instrumented* -O0 build (105,680 bytes/frame)
# -- but Clang's plain, non-sanitized -O0 frame for the same function is
# 143,064 bytes, worse than that ASan case, and segfaults past the OS
# stack limit around 58 native frames, well before the depth-95 guard can
# trip. Clang's -O1 measures 23,144 bytes/frame (smaller than even GCC's
# -O0), so only Clang's `debug` build trades away full -O0 variable
# visibility under a debugger; GCC's is unaffected.
#
# Both CFLAGS_DEBUG and CFLAGS_RELEASE below add -march=native on x86_64:
# with no distro packaging story yet, whoever builds this builds it for
# themselves, on the machine that's going to run it -- so there's no
# "built on a faster machine, copied to a different one" case to protect
# by default, for a debug build any more than a release one. That
# distinction matters because -native is not just "some missed
# vectorization on newer CPUs" the way a bytecode VM's non-numeric hot
# path might suggest -- measured directly (bench/int_arithmetic.d6, this
# repo's own bignum.c, which a plain `-march=x86-64` baseline denies
# BMI2/ADX): ~3.9s/iter generic vs ~0.5s/iter native, a ~7x difference,
# not a rounding error. Multi-precision arithmetic leans on BMI2
# (mulx)/ADX (adcx/adox) far more than "AVX/FMA" alone suggests.
#
# Practical minimum supported configuration: x86-64-v3 (AVX2/BMI2/FMA/
# LZCNT/MOVBE -- roughly 2013 Intel Haswell or 2015 AMD Excavator
# onward). Below that, the ~7x arithmetic falloff above isn't worth
# specifically supporting -- but this is a stated policy, not enforced
# in code: no compile-time or runtime check exists for it, and
# -march=native already clears this floor on any real CPU still in
# service, so there's nothing to enforce in the common case. Building
# for genuinely older hardware by overriding CFLAGS_DEBUG/CFLAGS_RELEASE
# yourself is possible; it's just unsupported.
#
# Anything that deliberately builds on one machine to run the binary
# on a *different* one -- skindicate.dia's build_vm.sh is the
# production example --
# must override CFLAGS_RELEASE itself rather than rely on the -native
# default, e.g. `make CFLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v3"
# release`: x86-64-v3 is a named, standardized ISA tier, not one
# specific CPU's exact feature set -- it recovers the same ~7x win as
# native for this workload (confirmed directly) without native's "tied
# to whichever machine happened to compile it" risk of an
# illegal-instruction crash on a different CPU.
#
# -march=native is itself an x86-only flag -- both gcc and clang reject it
# outright targeting arm64 ("unsupported argument"), which every current
# GitHub-hosted `macos-*` runner is by default (Apple Silicon). Guarded by
# UNAME_M rather than assumed, so this stays correct on an arm64 Linux box
# too, not just macOS. (aarch64's own equivalent, -mcpu=native, is a
# separate, unmeasured decision -- not made here.)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
ifeq ($(findstring clang,$(CC)),clang)
CFLAGS_DEBUG := -O1 -g3 -DDIAMOND_DEBUG -march=native
else
CFLAGS_DEBUG := -O0 -g3 -DDIAMOND_DEBUG -march=native
endif
CFLAGS_RELEASE := -O3 -DNDEBUG -march=native
else
ifeq ($(findstring clang,$(CC)),clang)
CFLAGS_DEBUG := -O1 -g3 -DDIAMOND_DEBUG
else
CFLAGS_DEBUG := -O0 -g3 -DDIAMOND_DEBUG
endif
CFLAGS_RELEASE := -O3 -DNDEBUG
endif
# -O1, not CFLAGS_DEBUG's -O0: run_chunk (src/vm_run_chunk.c) is one ~8,000-line
# function whose giant opcode switch declares its own locals (registers,
# per-opcode buffers, DiamondTypeBinding[8] arrays for generic-call
# opcodes, TLS setup buffers, etc.) in dozens of mutually-exclusive case
# blocks. -O0 disables stack-slot coalescing across non-overlapping
# lexical scopes, so every one of those locals gets its own permanent
# slot in one shared frame regardless of which case actually runs --
# confirmed via -fstack-usage at 105,680 bytes/frame, enough that
# depth(5000) (the DIAMOND_MAX_CALL_DEPTH regression test) hit a real
# ASan stack-overflow at ~71 recursive frames, well before
# DIAMOND_MAX_CALL_DEPTH=95's own guard (src/vm_run_chunk.c) could trip. -O1
# restores stack-slot coalescing (measured 68,624 bytes/frame, ~35%
# smaller) while keeping ASan/UBSan instrumentation and frame pointers
# (-fno-omit-frame-pointer) fully intact for readable backtraces; some
# locals may show "optimized out" under gdb, an accepted tradeoff scoped
# to this diagnostic build only -- GCC's `debug` stays -O0 for full
# variable visibility (Clang's own `debug` build needs the same -O1
# bump for a different reason -- see CFLAGS_DEBUG above).
CFLAGS_SANITIZE := -O1 -g3 -DDIAMOND_DEBUG -fsanitize=address,undefined \
	-fno-omit-frame-pointer
LDFLAGS_SANITIZE := -fsanitize=address,undefined
# ThreadSanitizer can't combine with ASan+UBSan above (mutually exclusive
# instrumentation), so this is its own build variant rather than an
# addition to CFLAGS_SANITIZE -- see docs/threads.md and tests/tsan_test.sh.
CFLAGS_TSAN := $(CFLAGS_DEBUG) -fsanitize=thread
LDFLAGS_TSAN := -fsanitize=thread
# LDLIBS_EXTRA: empty by default, appended into LDLIBS below so every
# target that already links against $(LDLIBS) (there's no separate list
# to keep in sync) picks up a platform-specific addition without any
# other change. Its one real use today: `make LDLIBS_EXTRA=-lucontext`
# on musl (Alpine), whose shipped libc.so declares ucontext_t/
# swapcontext/getcontext/makecontext in its headers but doesn't
# implement them at all -- libucontext (`apk add libucontext-dev`)
# provides all four under their standard names. Never needed on
# glibc (Fedora/Ubuntu), which implements them natively -- see
# docs/portability.md.
LDLIBS_EXTRA :=
# LDLIBS_DL/LDLIBS_CRYPT: default to Linux's real separate libraries, but
# each is its own overridable variable (not folded into the fixed list
# below) because macOS's libSystem provides both dlopen/dlsym and crypt()
# directly with no matching libdl.dylib/libcrypt.dylib to link against --
# `-ldl`/`-lcrypt` fail there with "library not found", unlike a merely
# redundant -I path, so these need to be droppable (`make LDLIBS_DL=
# LDLIBS_CRYPT=`), not just appended to.
LDLIBS_DL := -ldl
LDLIBS_CRYPT := -lcrypt
# LDFLAGS_EXTRA: same _EXTRA pattern as CPPFLAGS_EXTRA above, for the
# matching -L search path macOS's keg-only Homebrew and FreeBSD's ports
# OpenSSL/libpq/MariaDB Connector both need alongside CPPFLAGS_EXTRA's -I
# one (confirmed necessary on FreeBSD directly: linking failed with
# "unable to find library -lsqlite3/-lpq/-lmariadb", all three ports-
# installed under /usr/local/lib, not a default linker search path).
# Prepended into LDLIBS itself, not left as a separate $(LDFLAGS) most
# recipes below don't even reference -- only the final `diamond` binary
# and the API test binaries link with $(LDFLAGS); every tool/test target
# (gen_compiled_prelude and everything past it) links with $(LDLIBS)
# alone, so that's the one variable guaranteed to reach all of them.
# -L flags ahead of the -l flags that need them is all linker ordering
# requires; where they physically appear in $(LDLIBS) doesn't matter.
LDFLAGS_EXTRA :=
LDFLAGS := $(LDFLAGS_EXTRA)
LDLIBS := $(LDFLAGS_EXTRA) -lm $(REGINOLD_DIR)/libreginold.a $(OPTIONAL_LIBS) $(LDLIBS_DL) -lpthread $(OPENSSL_LIBS) $(LDLIBS_CRYPT) $(if $(NO_ZLIB),,-lz) $(LDLIBS_EXTRA)

# libFuzzer is a Clang/LLVM feature (-fsanitize=fuzzer isn't recognized by
# GCC at all) -- the fuzz binary is the one build variant in this Makefile
# that can't use $(CC), and needs its own object files entirely (GCC-built
# .o files carry no fuzzer/ASan/UBSan instrumentation to link against).
CC_FUZZ := clang
CFLAGS_FUZZ := -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Werror=implicit-function-declaration \
	-O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer

BUILD_DIR := build
TARGET := $(BUILD_DIR)/diamond
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
# The REPL's Tab-completion (lsp/repl_completion.c, called from src/repl.c) reuses lsp/completion.c's
# completion_compute_with_resolver directly (in-process, no LSP
# transport) rather than re-implementing candidate-list logic -- these
# are its own transitive dependencies (compile_buffer.c for the shared
# "build the buffer diamond_compile expects" step, analysis_cache.c for
# reusing completed tooling analysis, receiver.c for
# receiver.method resolution, json.c for the CompletionItem-shaped
# result), deliberately not the full $(LSP_SOURCES) below: no
# document.c (the REPL has no open-document table -- see
# completion_compute_with_resolver's own doc comment on passing
# resolver=nullptr), no rpc.c/main.c (no JSON-RPC transport). A
# separate lsp-%.o naming/object prefix, not the plain src/%.o pattern
# rule below, purely to keep these visually and mechanically distinct
# in $(BUILD_DIR) (a flat directory) from src/'s own objects -- no
# actual basename collision exists today, but this stays true even if
# one is ever introduced.
REPL_COMPLETION_SOURCES := lsp/completion.c lsp/compile_buffer.c lsp/analysis_cache.c \
	lsp/receiver.c lsp/json.c lsp/repl_completion.c
REPL_COMPLETION_OBJECTS := $(REPL_COMPLETION_SOURCES:lsp/%.c=$(BUILD_DIR)/lsp-%.o)
DEPS := $(OBJECTS:.o=.d) $(REPL_COMPLETION_OBJECTS:.o=.d)

.PHONY: all debug sanitize tsan release test test-examples test-release test-sanitize test-tsan test-api test-semver test-inline-cache-slots test-incremental-compile test-compiled-prelude test-aot-cache test-fibers test-fiber-run test-fiber-context test-vm-context test-yield test-continuation test-multi-yield test-scheduler test-scheduler-run-all test-fiber-gc-roots test-fiber-guards test-nested-yield-guard test-stack-overflow test-all test-builds test-integration test-facet test-facet-new facet test-database-config-package test-http-package test-gremlin-package test-websocket-package test-redis-package test-rack-package test-cookies-package test-multipart-package test-network-safety-package test-div-package test-dials-package test-graphql-package test-graphsql-package test-mjolnir-package test-logger-package test-minitest-package test-log-viewer-package test-active-karma-package test-active-auth-package test-active-social-package test-active-tagging-package test-active-discussion-package test-jobs-package test-registry-package test-registry-http test-deploy-registry test-monitor test-example-apps test-docs test-sanitize-script test-registry-inventory test-registry-nginx test-registry-seed test-database-dialects test-pheint-application test-lexer-diff test-parser-diff test-self-host test-self-host-smoke lsp test-lsp test-receiver dap test-dap aot-build aot-kit aot-kit-cross install test-aot-kit test-cross-build test-minimal test-wasm test-repl test-repl-completion fuzz test-fuzz test-stress-threads test-stress-threads-tsan test-cache clean

all: debug

debug: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_DEBUG)
debug: $(TARGET) $(BUILD_DIR)/run_cases

# sanitize/tsan/release each need a `clean-variant` (below) before they build --
# switching CFLAGS between variants (debug/sanitize/tsan/release) on the
# same build/%.o paths means a stale object from a *different* variant
# could otherwise get silently relinked instead of rebuilt. Listing
# `clean` as an ordinary prerequisite alongside $(TARGET)/run_cases (as
# these three used to) does NOT guarantee that ordering under `-j`:
# make's parallel scheduler is free to run sibling prerequisites of the
# same target concurrently whenever nothing in the dependency graph
# orders one before another, so removing the objects could -- and,
# confirmed directly under `-j$(nproc)`, did -- run concurrently with
# gcc already compiling into that same directory (surfacing as spurious
# "No such file or directory" on files gcc had only just finished
# writing). A synchronous `$(MAKE) clean-variant` in the recipe body, followed
# by a second $(MAKE) for the actual build, forces `clean` to fully
# finish first while still letting that second invocation parallelize
# its own compile steps under -j exactly as before (a recursive
# $(MAKE) shares the parent's jobserver, same as $(REGINOLD_LIB)'s own
# recipe above already relies on).
sanitize:
	$(MAKE) clean-variant
	$(MAKE) $(TARGET) $(BUILD_DIR)/run_cases \
		CFLAGS="$(CFLAGS_COMMON) $(CFLAGS_SANITIZE)" LDFLAGS="$(LDFLAGS_SANITIZE)"

tsan:
	$(MAKE) clean-variant
	$(MAKE) $(TARGET) $(BUILD_DIR)/run_cases \
		CFLAGS="$(CFLAGS_COMMON) $(CFLAGS_TSAN)" LDFLAGS="$(LDFLAGS_TSAN)"

# The release build links with -flto: run_chunk lives in its own translation unit (src/vm_run_chunk.c,
# docs/internal/vm-source-layout.md), and without cross-unit inlining and register allocation the
# interpreter executes about 3% more instructions (1.5% more cycles) than it did in one file.
LTO_FLAGS ?= -flto

release:
	$(MAKE) clean-variant
	$(MAKE) $(TARGET) $(BUILD_DIR)/run_cases \
		CFLAGS="$(CFLAGS_COMMON) $(CFLAGS_RELEASE) $(LTO_FLAGS)" \
		LDFLAGS="$(LDFLAGS) $(CFLAGS_RELEASE) $(LTO_FLAGS)"

# What a variant switch must discard: the objects and binaries built with $(CFLAGS), which the
# variants change. Not the whole $(BUILD_DIR): the tool objects in $(DBG_OBJ_DIR) and the fuzzer
# objects in $(FUZZ_OBJ_DIR) are built with fixed flags whatever the variant, so they stay valid
# (a clean instrumented compile of vm.c is minutes).
.PHONY: clean-variant
clean-variant:
	rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.d $(TARGET) $(BUILD_DIR)/run_cases

$(REGINOLD_LIB):
	$(MAKE) -C $(REGINOLD_DIR) libreginold.a

$(TARGET): $(OBJECTS) $(REPL_COMPLETION_OBJECTS) $(REGINOLD_LIB)
	$(CC) $(OBJECTS) $(REPL_COMPLETION_OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

# The bytecode cache (docs/caching.md) must not reuse a .d6c compiled by a
# different compiler, even when the file format is unchanged. A checksum of
# the compiler's sources and prelude goes into the cache fingerprint, and
# compiled_prelude.o rebuilds whenever any of them change. cksum is POSIX,
# so this works on every platform CI covers.
PRELUDE_SOURCES := lib/core.d6 $(wildcard lib/core/*.d6)
DIAMOND_BUILD_ID := $(shell cat src/*.c src/*.h $(PRELUDE_SOURCES) | cksum | cut -d' ' -f1)
$(BUILD_DIR)/compiled_prelude.o: CPPFLAGS += -DDIAMOND_BUILD_ID=$(DIAMOND_BUILD_ID)u
$(BUILD_DIR)/compiled_prelude.o: $(SOURCES) $(wildcard src/*.h) $(PRELUDE_SOURCES)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/lsp-%.o: lsp/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

test: debug
	bash tests/merge_when_green_test.sh
	bash tests/run.sh

test-release: release
	bash tests/run.sh

# The test run, its one retry for harness timing flakes, and why a sanitizer finding is never retried are in tools/test_sanitize.sh.
test-sanitize: sanitize
	bash tools/test_sanitize.sh

# The retry logic against fake test runners (no sanitizer build needed).
test-sanitize-script:
	bash tests/test_sanitize_test.sh

test-tsan: tsan
	bash tests/tsan_test.sh

# src/repl.c excluded for the same reason src/main.c already is: nothing
# under $(API_SOURCES)'s own consumers (api_invalidation, fiber_states,
# fiber_run, facet, the fuzz targets, run_cases) calls diamond_repl_run
# -- confirmed directly, not assumed. Excluding it here also means none
# of them need repl.c's own new lsp/ completion dependency
# (REPL_COMPLETION_SOURCES below) pulled in just to satisfy a linker
# that would otherwise see repl.c's unused-by-them references to it.
API_SOURCES := $(filter-out src/main.c src/repl.c,$(SOURCES))

# Build-time embedded prelude snapshot (docs/roadmap.md's "Make programs
# start faster"): src/compiled_prelude_data.c's own #embed points at
# this generated .bin, not a checked-in source file, so (unlike
# src/prelude.c's lib/*.d6 #embeds) nothing here is tracked by a `.c`
# file's own mtime -- every target below that compiles
# src/compiled_prelude_data.c as one of $(API_SOURCES) needs
# $(PRELUDE_BIN) listed as an explicit (order-only) prerequisite of its
# own, or a clean build would try to #embed a file that doesn't exist
# yet. GEN_PRELUDE_SOURCES excludes compiled_prelude_data.c itself from
# the generator's own link: gen_compiled_prelude doesn't need the
# embedded blob (it's what *produces* it), and linking it back in would
# make $(PRELUDE_BIN) depend on its own prior output. src/run_source.c
# is excluded too -- its own diamond_run_source now calls
# diamond_compiled_prelude_data/_size (src/compiled_prelude_data.c), so
# linking it here would reintroduce the same cycle one level removed;
# nothing the generator needs (diamond_compile,
# diamond_program_write_compiled, diamond_prelude_*) lives in
# run_source.c anyway.
PRELUDE_BIN := $(BUILD_DIR)/compiled_prelude.bin
GEN_PRELUDE_SOURCES := $(filter-out src/compiled_prelude_data.c src/run_source.c,$(API_SOURCES))

# Every tool and test binary below (gen_compiled_prelude, facet, the LSP and DAP servers, the
# API and fiber tests, ...) links the same sources compiled with the same debug flags. They used
# to recompile all of src/*.c, vm.c above all, in each one's own command; now the sources are
# compiled once into $(DBG_OBJ_DIR) and each binary only compiles its own file and links.
# Independent of which variant ($(TARGET)'s own flags) is active, because these binaries always
# used the debug flags. No -DDIAMOND_BUILD_ID, as before: only $(TARGET)'s own
# compiled_prelude.o carries it.
DBG_OBJ_DIR := $(BUILD_DIR)/dbg
API_DBG_OBJECTS := $(API_SOURCES:src/%.c=$(DBG_OBJ_DIR)/%.o)
GEN_DBG_OBJECTS := $(GEN_PRELUDE_SOURCES:src/%.c=$(DBG_OBJ_DIR)/%.o)

$(DBG_OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) -MMD -MP -c $< -o $@

# Same reason as $(BUILD_DIR)/compiled_prelude_data.o's own line below.
$(DBG_OBJ_DIR)/compiled_prelude_data.o: $(PRELUDE_BIN)

# The prelude sources too (lib/core.d6 and lib/core/*.d6): src/prelude.c
# #embeds them, and this generator compiles the sources directly, so no .d
# file records that dependency. Without it, editing the prelude left
# compiled_prelude.bin -- the prelude bytecode every diamond binary
# actually runs -- stale.
$(BUILD_DIR)/gen_compiled_prelude: tools/gen_compiled_prelude.c $(GEN_DBG_OBJECTS) $(PRELUDE_SOURCES) $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(GEN_DBG_OBJECTS) $< $(LDLIBS) -o $@

$(PRELUDE_BIN): $(BUILD_DIR)/gen_compiled_prelude
	$(BUILD_DIR)/gen_compiled_prelude $@

# Normal (not order-only) prerequisite: -MMD -MP's own generated .d file
# for this object (once it exists) already tracks $(PRELUDE_BIN) as a
# dependency via its #embed, exactly like every other src/%.o already
# tracks its own #include'd headers -- this line only matters for the
# very first build, before that .d file exists yet.
$(BUILD_DIR)/compiled_prelude_data.o: $(PRELUDE_BIN)

# Built with whichever variant (debug/sanitize/release) is currently
# active, same as $(TARGET) itself -- tests/run.sh's own file-based case
# loop runs whatever `make test`/`test-release`/`test-sanitize` just
# built, and needs run_cases to match (see docs/roadmap.md for why this
# exists: running every tests/cases/*.d6 case in this one process
# instead of tests/run.sh spawning a fresh `diamond` per case). Placed
# here, after $(PRELUDE_BIN)'s own definition above, not up by
# $(TARGET)'s own rule where it used to sit -- a prerequisite list
# (everything right of the `:`, order-only included) is macro-expanded
# the moment make *parses* this line, not lazily at build time the way
# a recipe body's own $(...) references are. `$(PRELUDE_BIN)` used up
# there, before its `:=` definition existed yet, silently expanded to
# nothing -- a real, confirmed bug: it left this order-only prerequisite
# empty, so nothing stopped make from starting this recipe before
# $(PRELUDE_BIN) was ready under `-j` (surfacing as a spurious
# "compiled_prelude.bin: No such file or directory" on an otherwise
# clean build -- reproduced directly under `-j$(nproc)`, gone once this
# rule moved below the real definition).
# Links the same objects $(TARGET) does (minus main.o and repl.o, as $(API_SOURCES) excludes
# them), built with the same variant flags, instead of recompiling every source in this command:
# under the sanitizer variant that was a second multi-minute compile of vm.c.
API_OBJECTS := $(filter-out $(BUILD_DIR)/main.o $(BUILD_DIR)/repl.o,$(OBJECTS))

$(BUILD_DIR)/run_cases: tests/run_cases.c $(API_OBJECTS) $(PRELUDE_SOURCES) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(API_OBJECTS) $< $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/compiled_prelude_test: tests/compiled_prelude_test.c $(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) $< $(LDLIBS) -o $@

test-compiled-prelude: $(BUILD_DIR)/compiled_prelude_test
	$(BUILD_DIR)/compiled_prelude_test

$(BUILD_DIR)/api_invalidation: tests/api_invalidation.c $(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) $< $(LDLIBS) -o $@

# buffer_growth_test includes jit.c and loader.c to reach their private buffers.
$(BUILD_DIR)/buffer_growth_test: tests/buffer_growth_test.c src/jit.c src/loader.c \
		$(filter-out $(DBG_OBJ_DIR)/jit.o $(DBG_OBJ_DIR)/loader.o,$(API_DBG_OBJECTS)) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
		$(filter-out $(DBG_OBJ_DIR)/jit.o $(DBG_OBJ_DIR)/loader.o,$(API_DBG_OBJECTS)) $< $(LDLIBS) -o $@

$(BUILD_DIR)/aot_write_test: tests/aot_write_test.c src/main.c $(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) $< $(LDLIBS) -o $@

test-api: $(BUILD_DIR)/api_invalidation $(BUILD_DIR)/buffer_growth_test $(BUILD_DIR)/aot_write_test
	$(BUILD_DIR)/api_invalidation
	$(BUILD_DIR)/buffer_growth_test
	$(BUILD_DIR)/aot_write_test

# tools/semver.c is deliberately standalone -- no Diamond compiler/VM
# dependency at all (see its own header comment), so this doesn't link
# $(API_SOURCES)/$(REGINOLD_LIB) the way every other tools/tests target
# above does. -Itools (not folded into the global CPPFLAGS) is scoped to
# just this one rule since nothing else needs tools/semver.h.
$(BUILD_DIR)/semver_test: tests/semver_test.c tools/semver.c tools/semver.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS_COMMON) $(CFLAGS_DEBUG) tools/semver.c $< -o $@

test-semver: $(BUILD_DIR)/semver_test
	$(BUILD_DIR)/semver_test

$(BUILD_DIR)/inline_cache_slots: tests/inline_cache_slots.c src/vm_internal.h src/vm.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $< -o $@

test-inline-cache-slots: $(BUILD_DIR)/inline_cache_slots
	$(BUILD_DIR)/inline_cache_slots

$(BUILD_DIR)/incremental_compile_test: tests/incremental_compile_test.c $(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) $< $(LDLIBS) -o $@

test-incremental-compile: $(BUILD_DIR)/incremental_compile_test
	$(BUILD_DIR)/incremental_compile_test

$(BUILD_DIR)/fiber_states: tests/fiber_states.c $(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) $< $(LDLIBS) -o $@

test-fibers: $(BUILD_DIR)/fiber_states
	$(BUILD_DIR)/fiber_states

$(BUILD_DIR)/repl_completion_test: tests/repl_completion_test.c src/repl.c \
		$(API_DBG_OBJECTS) $(REPL_COMPLETION_SOURCES) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) src/repl.c $(API_DBG_OBJECTS) \
		$(REPL_COMPLETION_SOURCES) $< $(LDLIBS) -o $@

test-repl-completion: $(BUILD_DIR)/repl_completion_test
	$(BUILD_DIR)/repl_completion_test

test-fiber-guards: test-fibers

test-fiber-context: test-fibers

$(BUILD_DIR)/fiber_run: tests/fiber_run.c $(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) $< $(LDLIBS) -o $@

test-fiber-run: $(BUILD_DIR)/fiber_run
	$(BUILD_DIR)/fiber_run

test-vm-context: test-fiber-run

test-yield: test-fiber-run

test-continuation: test-fiber-run

test-multi-yield: test-fiber-run

test-scheduler: test-fiber-run

test-scheduler-run-all: test-fiber-run

test-fiber-gc-roots: test-fiber-run

test-nested-yield-guard: test-fiber-run

test-stack-overflow: test-fiber-run

FACET_SOURCES := tools/facet.c tools/facet_resolve.c tools/facet_package.c tools/facet_new.c
# facet_new.c #embeds these.
FACET_TEMPLATES := $(wildcard tools/new_templates/*/*)

$(BUILD_DIR)/facet: $(FACET_SOURCES) $(FACET_TEMPLATES) tools/facet_internal.h tools/semver.c tools/semver.h $(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) tools/semver.c $(FACET_SOURCES) $(LDLIBS) -o $@

facet: $(BUILD_DIR)/facet

test-facet: $(BUILD_DIR)/facet
	bash tests/facet_test.sh

test-facet-new: $(TARGET) $(BUILD_DIR)/facet
	bash tests/facet_new_test.sh

.PHONY: test-source-extensions
test-source-extensions: debug facet
	bash tests/source_extensions_test.sh

test-database-config-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/database_config/test.sh

test-http-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/http/test.sh

test-gremlin-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/gremlin/test.sh

test-websocket-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/websocket/test.sh

test-redis-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/redis/test.sh

test-rack-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/rack/test.sh

test-cookies-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/cookies/test.sh

test-multipart-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/multipart/test.sh

test-network-safety-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/network_safety/test.sh

test-div-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/div/test.sh

test-dials-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/dials/test.sh

test-graphql-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/graphql/test.sh

test-graphsql-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/graphsql/test.sh

test-mjolnir-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/mjolnir/test.sh

test-logger-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/logger/test.sh

test-minitest-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/minitest/test.sh

test-log-viewer-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/log_viewer/test.sh

test-active-karma-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/active_karma/test.sh

test-active-auth-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/active_auth/test.sh

test-active-social-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/active_social/test.sh

test-active-tagging-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/active_tagging/test.sh

test-active-discussion-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/active_discussion/test.sh

.PHONY: test-cancellation-package test-job-service

test-cancellation-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/cancellation/test.sh

test-job-service: $(TARGET) $(BUILD_DIR)/facet
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash examples/job_service/smoke_test.sh

test-jobs-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/jobs/test.sh

test-registry-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/registry/test.sh

test-registry-http: $(TARGET) $(BUILD_DIR)/facet
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/registry/backup_download_test.sh
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond $(CURDIR)/$(BUILD_DIR)/diamond packages/registry/http_test.d6

# Opt-in staging gate: requires nginx and local sockets; run in a QEMU guest.
test-registry-nginx: $(TARGET) $(BUILD_DIR)/facet
	$(BUILD_DIR)/diamond packages/registry/nginx_test.d6

# The reviewed launch inventory against freshly built archives. A changed package needs its version bumped and its inventory entry
# regenerated (docs/registry-release-plan.md); this is the part of test-registry-seed that needs no nginx.
test-registry-inventory: $(TARGET) $(BUILD_DIR)/facet
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/registry/seed_test.sh

test-registry-seed: $(TARGET) $(BUILD_DIR)/facet
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/registry/seed_test.sh
	REGISTRY_REHEARSE_SEED=1 $(BUILD_DIR)/diamond packages/registry/nginx_test.d6

# The same SQL against live MariaDB, MySQL and PostgreSQL servers in throwaway containers (podman, or docker through a `podman` on
# PATH; each script skips when there is none). CI runs it in its own job; the default `make test` stays hermetic.
test-database-dialects:
	$(MAKE) debug
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/arel/test_mariadb_dialect.sh
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/arel/test_mysql_dialect.sh
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/arel/test_postgres_dialect.sh
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/active_record/test_postgres.sh
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/mjolnir/test_dialects.sh

test-pheint-application: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash applications/pheint.dia/test.sh

LSP_SOURCES := $(wildcard lsp/*.c)

$(BUILD_DIR)/diamond-lsp: $(LSP_SOURCES) $(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ilsp $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) $(LSP_SOURCES) $(LDLIBS) -o $@

lsp: $(BUILD_DIR)/diamond-lsp

$(BUILD_DIR)/receiver_test: tests/receiver_test.c lsp/receiver.c lsp/compile_buffer.c \
		$(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ilsp $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) \
		lsp/receiver.c lsp/compile_buffer.c $< $(LDFLAGS) $(LDLIBS) -o $@

test-receiver: $(BUILD_DIR)/receiver_test
	$(BUILD_DIR)/receiver_test

$(BUILD_DIR)/analysis_cache_test: tests/analysis_cache_test.c lsp/analysis_cache.c lsp/analysis_cache.h lsp/compile_buffer.c \
		$(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ilsp $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) \
		lsp/analysis_cache.c lsp/compile_buffer.c $< $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/position_test: tests/position_test.c lsp/position.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ilsp $(CFLAGS_COMMON) $(CFLAGS_DEBUG) tests/position_test.c $(LDFLAGS) -o $@

$(BUILD_DIR)/json_test: tests/json_test.c lsp/json.c lsp/json.h lsp/rpc.c lsp/rpc.h src/message_length.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ilsp $(CFLAGS_COMMON) $(CFLAGS_DEBUG) tests/json_test.c lsp/json.c lsp/rpc.c $(LDFLAGS) -lm -o $@

$(BUILD_DIR)/formatting_test: tests/formatting_test.c lsp/formatting.c lsp/formatting.h lsp/json.c src/lexer.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ilsp $(CFLAGS_COMMON) $(CFLAGS_DEBUG) tests/formatting_test.c \
		lsp/formatting.c lsp/json.c src/lexer.c $(LDFLAGS) -lm -o $@

# Spacing, blank-line and range rules, plus whitespace-only and idempotence
# invariants over every Diamond source in the tree.
test-formatting: $(BUILD_DIR)/formatting_test
	$(BUILD_DIR)/formatting_test $$(find tests/cases examples packages lib -name '*.d6' | sort)

test-lsp: $(BUILD_DIR)/diamond-lsp $(BUILD_DIR)/receiver_test $(BUILD_DIR)/json_test $(BUILD_DIR)/position_test $(BUILD_DIR)/analysis_cache_test $(BUILD_DIR)/formatting_test
	$(BUILD_DIR)/receiver_test
	$(BUILD_DIR)/json_test
	$(BUILD_DIR)/position_test
	$(BUILD_DIR)/formatting_test $$(find tests/cases examples packages lib -name '*.d6' | sort)
	bash tests/analysis_cache_test.sh
	bash tests/v3_editor_test.sh
	bash tests/lsp_imported_generics_test.sh
	bash tests/lsp_test.sh

DAP_SOURCES := $(wildcard dap/*.c)
# Only json.c/rpc.c, not the rest of $(LSP_SOURCES): diamond-dap needs the
# same Content-Length-framed JSON transport diamond-lsp uses (both for its
# DAP-client-facing stdio and for the VM control-channel socket, see
# dap/main.c's own top comment), but none of the LSP-specific document/
# completion/hover/etc. handlers, and definitely not lsp/main.c's own
# main() (which would collide with this binary's own).
DAP_JSON_SOURCES := lsp/json.c lsp/rpc.c

$(BUILD_DIR)/diamond-dap: $(DAP_SOURCES) $(DAP_JSON_SOURCES) $(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ilsp $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) $(DAP_JSON_SOURCES) $(DAP_SOURCES) $(LDLIBS) -o $@

dap: $(BUILD_DIR)/diamond-dap

$(BUILD_DIR)/debug_frame_test: tests/debug_frame_test.c $(API_DBG_OBJECTS) $(REGINOLD_LIB) | $(PRELUDE_BIN)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_DBG_OBJECTS) $< $(LDLIBS) -o $@

test-dap: $(BUILD_DIR)/diamond-dap $(BUILD_DIR)/debug_frame_test $(TARGET)
	$(BUILD_DIR)/debug_frame_test
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash tests/dap_test.sh

# `diamond build` (src/main.c's own "build" subcommand) invokes this to
# link a user's already-compiled program into a standalone executable --
# AOT_EMBED is a generated .c file (following src/compiled_prelude_
# data.c's own #embed-plus-accessor-function shape, see tools/aot_
# runtime_main.c's own top comment) that diamond build writes to a temp
# path per invocation, never checked in. Cache the runtime objects as a
# separate archive: the ordinary CLI objects may use different debug,
# release, or sanitizer flags, while every standalone app uses -O2/-g0.
# The compiler name is part of the cache path so --cc=clang never reuses
# gcc objects. A content fingerprint in the config stamp protects persistent
# caches from copied/replaced source trees whose mtimes move backwards; the
# ordinary dependency files still avoid rebuilding for unchanged content.
# AOT_CACHE_ROOT can put the cache outside build/ (for ephemeral container
# checkouts); the caller then owns its cleanup.
AOT_EMBED ?=
AOT_OUTPUT ?= $(BUILD_DIR)/a.out
AOT_CACHE_ROOT ?= $(BUILD_DIR)/aot
AOT_CACHE_DIR := $(AOT_CACHE_ROOT)-$(subst /,_,$(CC))
AOT_CFLAGS := $(CFLAGS_COMMON) -O2 -g0
# `make aot-kit AOT_LTO=1` adds a second runtime archive for `diamond build --lto`: the same
# sources at -O3 as fat LTO objects (ordinary code that links like any other object, plus the
# compiler's intermediate form), so the app's link can optimise the runtime and the program
# together. That link recompiles the runtime for every app (tens of seconds), which is why it
# is opt-in at both ends.
AOT_LTO ?=
AOT_LTO_CFLAGS := $(CFLAGS_COMMON) -O3 -g0 -flto -ffat-lto-objects
AOT_CONFIG := $(AOT_CACHE_DIR)/config
AOT_FINGERPRINT_EXTRA ?=
AOT_FINGERPRINT_INPUTS := Makefile tools/aot_fingerprint.sh \
	$(API_SOURCES) $(wildcard src/*.h) tools/aot_runtime_main.c \
	$(wildcard $(REGINOLD_DIR)/*.h) $(PRELUDE_BIN) $(AOT_FINGERPRINT_EXTRA)
AOT_RUNTIME_OBJECTS := $(API_SOURCES:src/%.c=$(AOT_CACHE_DIR)/%.o) $(AOT_CACHE_DIR)/runtime_main.o
AOT_RUNTIME_LIB := $(AOT_CACHE_DIR)/libdiamond-aot.a
AOT_LTO_DIR := $(AOT_CACHE_DIR)/lto
AOT_LTO_OBJECTS := $(AOT_RUNTIME_OBJECTS:$(AOT_CACHE_DIR)/%.o=$(AOT_LTO_DIR)/%.o)
AOT_LTO_LIB := $(AOT_CACHE_DIR)/libdiamond-aot-lto.a

.PHONY: aot-config-check
aot-config-check:

$(AOT_CONFIG): aot-config-check $(AOT_FINGERPRINT_INPUTS)
	@mkdir -p $(AOT_CACHE_DIR)
	@fingerprint="$$(tools/aot_fingerprint.sh $(AOT_FINGERPRINT_INPUTS))" || { echo "tools/aot_fingerprint.sh failed" >&2; exit 1; }; \
	[ -n "$$fingerprint" ] || { echo "tools/aot_fingerprint.sh printed nothing" >&2; exit 1; }; \
	printf '%s\n' '$(CC)' '$(CPPFLAGS)' '$(AOT_CFLAGS)' '$(AOT_LTO_CFLAGS)' '$(shell $(CC) -dumpversion)' \
		"$$fingerprint" > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(AOT_CACHE_DIR)/%.o: src/%.c $(AOT_CONFIG)
	@mkdir -p $(AOT_CACHE_DIR)
	$(CC) $(CPPFLAGS) $(AOT_CFLAGS) -MMD -MP -c $< -o $@

$(AOT_CACHE_DIR)/runtime_main.o: tools/aot_runtime_main.c $(AOT_CONFIG)
	@mkdir -p $(AOT_CACHE_DIR)
	$(CC) $(CPPFLAGS) $(AOT_CFLAGS) -MMD -MP -c $< -o $@

$(AOT_CACHE_DIR)/compiled_prelude_data.o: $(PRELUDE_BIN)

$(AOT_RUNTIME_LIB): $(AOT_RUNTIME_OBJECTS)
	$(AR) rcs $@ $^

$(AOT_LTO_DIR)/%.o: src/%.c $(AOT_CONFIG)
	@mkdir -p $(AOT_LTO_DIR)
	$(CC) $(CPPFLAGS) $(AOT_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(AOT_LTO_DIR)/runtime_main.o: tools/aot_runtime_main.c $(AOT_CONFIG)
	@mkdir -p $(AOT_LTO_DIR)
	$(CC) $(CPPFLAGS) $(AOT_LTO_CFLAGS) -MMD -MP -c $< -o $@

$(AOT_LTO_DIR)/compiled_prelude_data.o: $(PRELUDE_BIN)

$(AOT_LTO_LIB): $(AOT_LTO_OBJECTS)
	$(AR) rcs $@ $^

.PHONY: aot-build
aot-build: $(AOT_RUNTIME_LIB)
	$(CC) $(CPPFLAGS) $(AOT_CFLAGS) $(AOT_EMBED) \
	    $(AOT_RUNTIME_LIB) $(LDLIBS) -o $(AOT_OUTPUT)

# A prebuilt AOT kit lets an installed `diamond build` link without this
# checkout or make: the runtime archive, reginold, and the exact compile and
# link arguments (one per line) used to produce them. `diamond` uses a kit at
# <bin>/../lib/diamond/aot (or $DIAMOND_AOT_KIT) and falls back to the
# aot-build target above when none exists.
AOT_KIT_DIR := $(BUILD_DIR)/aot-kit
AOT_KIT_LINK_ARGS := $(LDFLAGS_EXTRA) -lm $(OPTIONAL_LIBS) $(LDLIBS_DL) \
	-lpthread $(OPENSSL_LIBS) $(LDLIBS_CRYPT) $(if $(NO_ZLIB),,-lz) $(LDLIBS_EXTRA)
DIAMOND_VERSION := $(shell sed -n 's/.*DIAMOND_VERSION\[\] = "\(.*\)";/\1/p' src/main.c)

.PHONY: aot-kit
aot-kit: $(AOT_RUNTIME_LIB) $(if $(AOT_LTO),$(AOT_LTO_LIB)) $(REGINOLD_LIB)
	@rm -rf $(AOT_KIT_DIR)
	@mkdir -p $(AOT_KIT_DIR)
	cp $(AOT_RUNTIME_LIB) $(REGINOLD_LIB) $(AOT_KIT_DIR)/
	@if [ -n '$(AOT_LTO)' ]; then \
		cp $(AOT_LTO_LIB) $(AOT_KIT_DIR)/ && printf '%s\n' $(AOT_LTO_CFLAGS) > $(AOT_KIT_DIR)/compile-lto.args; \
	fi
	@printf '%s\n' '$(DIAMOND_VERSION)' > $(AOT_KIT_DIR)/version
	@printf '%s\n' '$(CC)' > $(AOT_KIT_DIR)/cc
	@printf '%s\n' $(AOT_CFLAGS) > $(AOT_KIT_DIR)/compile.args
	@printf '%s\n' $(AOT_KIT_LINK_ARGS) > $(AOT_KIT_DIR)/link.args
	@if [ -n '$(TARGET_TRIPLE)' ]; then printf '%s\n' '$(TARGET_TRIPLE)' > $(AOT_KIT_DIR)/target; fi

# Cross kit for `diamond build --target=TRIPLE`. Needs a C23 compiler that
# targets TRIPLE (clang 19+ with --target) and a sysroot holding the target's
# C library, headers and the libraries this Makefile links (OpenSSL, SQLite3,
# libpq, MariaDB, zlib, libcrypt). The sysroot path is recorded in the kit, so
# it has to exist wherever `diamond build --target` later links:
#
#   make aot-kit-cross TARGET_TRIPLE=aarch64-linux-gnu SYSROOT=/opt/sysroots/a64 \
#        CROSS_FLAGS_EXTRA='--gcc-install-dir=/opt/sysroots/a64/usr/lib/gcc/aarch64-redhat-linux/16'
#
# CROSS_FLAGS_EXTRA carries anything else the cross driver needs (a GCC
# install dir, -fuse-ld=...). The prelude snapshot is generated by the host
# build and is byte-order/size checked at load time; reginold is rebuilt for
# the target from a scratch copy so the host archive is left alone. The kit
# lands in $(BUILD_DIR)/aot-targets/TRIPLE, which `diamond build --target`
# finds from a checkout and `make install` copies next to the host kit.
CROSS_CC ?= clang
CROSS_FLAGS_EXTRA ?=
CROSS_FLAGS := --target=$(TARGET_TRIPLE) --sysroot=$(SYSROOT) $(CROSS_FLAGS_EXTRA)
CROSS_WORK := $(BUILD_DIR)/cross-$(TARGET_TRIPLE)
CROSS_CPPFLAGS := -Isrc -Ilsp -I$(REGINOLD_DIR) -isystem $(SYSROOT)/usr/include/mysql \
	-isystem $(SYSROOT)/usr/include/mariadb -isystem $(SYSROOT)/usr/include/postgresql \
	$(OPTIONAL_DEFINES)
# STATIC=1 makes the kit link fully statically (use with a musl target):
# -static first, then the archives libpq itself needs and libucontext, which
# musl lacks (STATIC_LDLIBS names them for sysroots that call them differently).
STATIC ?=
STATIC_LDLIBS ?= -lpgcommon_shlib -lpgport_shlib -lucontext

.PHONY: aot-kit-cross
aot-kit-cross:
	@test -n '$(TARGET_TRIPLE)' || { echo 'aot-kit-cross needs TARGET_TRIPLE=<triple> SYSROOT=<dir>' >&2; exit 1; }
	@test -d '$(SYSROOT)' || { echo 'aot-kit-cross needs SYSROOT=<dir> to be a directory' >&2; exit 1; }
	@case '$(TARGET_TRIPLE)' in *[!A-Za-z0-9_.-]*) echo 'invalid TARGET_TRIPLE' >&2; exit 1;; esac
	$(MAKE) MINIMAL= NO_MYSQL= NO_POSTGRES= NO_SQLITE= NO_ZLIB= NO_TLS= NO_CRYPTO= $(PRELUDE_BIN)
	rm -rf $(CROSS_WORK)
	mkdir -p $(CROSS_WORK)
	cp -R $(REGINOLD_DIR) $(CROSS_WORK)/reginold
	find $(CROSS_WORK)/reginold \( -name '*.o' -o -name '*.a' \) -delete
	$(MAKE) -C $(CROSS_WORK)/reginold CC='$(CROSS_CC) $(CROSS_FLAGS)' libreginold.a
	$(MAKE) -o $(PRELUDE_BIN) aot-kit CC='$(CROSS_CC)' TARGET_TRIPLE='$(TARGET_TRIPLE)' \
	    NO_MYSQL='$(NO_MYSQL)' NO_POSTGRES='$(NO_POSTGRES)' NO_SQLITE='$(NO_SQLITE)' \
	    NO_ZLIB='$(NO_ZLIB)' NO_TLS='$(NO_TLS)' NO_CRYPTO='$(NO_CRYPTO)' CPPFLAGS='$(CROSS_CPPFLAGS)' \
	    $(if $(STATIC),LDFLAGS_EXTRA='-static' LDLIBS_EXTRA='$(STATIC_LDLIBS)') \
	    AOT_CFLAGS='$(CFLAGS_COMMON) -O2 -g0 $(CROSS_FLAGS)' \
	    AOT_CACHE_ROOT='$(CROSS_WORK)/aot' \
	    AOT_KIT_DIR='$(CROSS_WORK)/kit' \
	    REGINOLD_LIB='$(CROSS_WORK)/reginold/libreginold.a'
	rm -rf $(BUILD_DIR)/aot-targets/$(TARGET_TRIPLE)
	mkdir -p $(BUILD_DIR)/aot-targets
	cp -R $(CROSS_WORK)/kit $(BUILD_DIR)/aot-targets/$(TARGET_TRIPLE)

# Installs the tools that have been built plus the AOT kit. Build first, e.g.
# `make release facet lsp dap && make install PREFIX=$HOME/.local`.
PREFIX ?= /usr/local
INSTALL_TOOLS := $(TARGET) $(BUILD_DIR)/facet $(BUILD_DIR)/diamond-lsp $(BUILD_DIR)/diamond-dap

.PHONY: install
install: aot-kit
	@test -x $(TARGET) || { echo 'Build diamond first, e.g. make release' >&2; exit 1; }
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib/diamond/aot
	for tool in $(INSTALL_TOOLS); do \
		if [ -x "$$tool" ]; then cp "$$tool" $(DESTDIR)$(PREFIX)/bin/; fi; \
	done
	rm -rf $(DESTDIR)$(PREFIX)/lib/diamond/aot
	cp -R $(AOT_KIT_DIR) $(DESTDIR)$(PREFIX)/lib/diamond/aot
	if [ -d $(BUILD_DIR)/aot-targets ]; then \
		mkdir -p $(DESTDIR)$(PREFIX)/lib/diamond/aot/targets; \
		cp -R $(BUILD_DIR)/aot-targets/. $(DESTDIR)$(PREFIX)/lib/diamond/aot/targets/; \
	fi

test-aot-cache: debug
	bash tests/build_from_app_dir.sh

test-aot-kit: debug
	bash tests/aot_kit_test.sh

test-cross-build: debug
	bash tests/cross_build_test.sh

# Run as `make clean && make MINIMAL=1 test-minimal`: builds without SQLite, PostgreSQL,
# MySQL/MariaDB and zlib and checks that nothing else changed. A plain `make test-minimal`
# runs the same script against a full build, where the error checks are expected to fail.
test-minimal: debug
	bash tests/minimal_cases.sh

# The browser module: needs WASI_SDK=/path/to/wasi-sdk (https://github.com/WebAssembly/wasi-sdk)
# and Node 18+.
test-wasm:
	@test -n '$(WASI_SDK)' || { echo 'test-wasm needs WASI_SDK=/path/to/wasi-sdk' >&2; exit 1; }
	WASI_SDK='$(WASI_SDK)' bash wasm/build.sh
	node tests/wasm_repl_test.mjs

test-repl: debug
	bash tests/repl_test.sh

test-deploy-registry:
	bash tests/deploy_registry_test.sh
	bash tests/registry_upgrade_test.sh

test-monitor: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash tests/monitor_test.sh

# guestbook, project_board, chat, library and bookmarks against the cuts in packages/ (the rest of the examples are test-examples).
test-example-apps: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash tests/example_apps_test.sh

.PHONY: test-task-board
test-task-board: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash examples/task_board/test.sh

# The ```ruby examples in the docs and the published cuts' READMEs: `# =>` claims
# must hold and every block must parse -- see tools/check_doc_examples.d6.
# The taken-down cuts are not published, so their READMEs are left out.
DOC_CHECK_UNPUBLISHED := active_auth active_discussion active_karma active_social active_tagging dials
DOC_CHECK_FILES := $(wildcard docs/*.md) README.md $(filter-out $(patsubst %,packages/%/README.md,$(DOC_CHECK_UNPUBLISHED)),$(wildcard packages/*/README.md))
test-docs: $(TARGET)
	COOKIE_SECRET=doc-check-secret-0123456789abcdef $(BUILD_DIR)/diamond tools/check_doc_examples.d6 $(DOC_CHECK_FILES)

test-exit: debug
	bash tests/exit_test.sh

# The standalone example programs (examples/*/smoke_test.sh), each run
# interpreted and as a `diamond build` binary. The web and chat examples
# need databases or cuts and have their own instructions.
EXAMPLE_SMOKE_TESTS := agenda brainfuck budget calc databases dispatcher exact generators grades huffman kvstore leaderboard ledger life logstat markdown metrics models notes parallel pathfinder pipeline plugins pngmeta redact resolver spreadsheet taskrun template tictactoe tlsecho udiff vault vending
test-examples: debug
	@for example in $(EXAMPLE_SMOKE_TESTS); do \
		bash examples/$$example/smoke_test.sh || exit 1; \
	done

EXAMPLE_SMOKE_TESTS += debug_me

test-cache: debug
	# The Makefile-wide DIAMOND_NO_CACHE=1 export above exists specifically
	# to protect every *other* test target from this feature -- this one
	# is the exception, since it exercises the caching itself and needs it
	# genuinely enabled. env -u, not DIAMOND_NO_CACHE= : an empty value is
	# still a "set" env var as far as getenv() is concerned (see src/run_
	# source.c's own presence-only check), so only actually unsetting it
	# turns caching back on for this one recipe.
	env -u DIAMOND_NO_CACHE bash tests/cache_test.sh

# Both fuzzers link the same instrumented copy of the sources. Compiling them once into
# $(FUZZ_OBJ_DIR) means a change to a harness recompiles only that file: under clang's
# ASan+UBSan, vm.c alone takes about six minutes, and it used to be compiled again for every
# harness edit and once per harness. -fsanitize=fuzzer-no-link instruments the objects exactly as
# -fsanitize=fuzzer does; only the link step pulls in libFuzzer's main.
FUZZ_OBJ_DIR := $(BUILD_DIR)/fuzz
FUZZ_OBJECTS := $(API_SOURCES:src/%.c=$(FUZZ_OBJ_DIR)/%.o)
FUZZ_COMPILE_FLAGS := $(subst -fsanitize=fuzzer,-fsanitize=fuzzer-no-link,$(CFLAGS_FUZZ))
FUZZ_LIBS := -lm $(REGINOLD_DIR)/libreginold.a -lsqlite3 -lpq -lmariadb -ldl -lpthread -lssl -lcrypto -lcrypt -lz

$(FUZZ_OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC_FUZZ) $(CPPFLAGS) $(FUZZ_COMPILE_FLAGS) -MMD -MP -c $< -o $@

$(FUZZ_OBJ_DIR)/%.o: fuzz/%.c
	@mkdir -p $(@D)
	$(CC_FUZZ) $(CPPFLAGS) $(FUZZ_COMPILE_FLAGS) -MMD -MP -c $< -o $@

# Same reason as $(BUILD_DIR)/compiled_prelude_data.o's own line.
$(FUZZ_OBJ_DIR)/compiled_prelude_data.o: $(PRELUDE_BIN)

$(BUILD_DIR)/compile_fuzzer: $(FUZZ_OBJ_DIR)/compile_fuzzer.o $(FUZZ_OBJECTS) $(REGINOLD_LIB)
	$(CC_FUZZ) $(CFLAGS_FUZZ) $(FUZZ_OBJECTS) $< $(FUZZ_LIBS) -o $@

$(BUILD_DIR)/execute_fuzzer: $(FUZZ_OBJ_DIR)/execute_fuzzer.o $(FUZZ_OBJECTS) $(REGINOLD_LIB)
	$(CC_FUZZ) $(CFLAGS_FUZZ) $(FUZZ_OBJECTS) $< $(FUZZ_LIBS) -o $@

# Runs each input under the interpreter and under the JIT plus stress GC and
# aborts when they disagree; see fuzz/differential_fuzzer.c. Not part of the
# bounded smoke test: seed it with tests/cases and run it for as long as you like.
$(BUILD_DIR)/differential_fuzzer: $(FUZZ_OBJ_DIR)/differential_fuzzer.o $(FUZZ_OBJECTS) $(REGINOLD_LIB)
	$(CC_FUZZ) $(CFLAGS_FUZZ) $(FUZZ_OBJECTS) $< $(FUZZ_LIBS) -o $@

fuzz: $(BUILD_DIR)/compile_fuzzer $(BUILD_DIR)/execute_fuzzer

test-fuzz: $(BUILD_DIR)/compile_fuzzer $(BUILD_DIR)/execute_fuzzer
	bash tests/fuzz_smoke.sh

# Re-runs the Thread/Channel/Supervisor cases many times pinned to two CPUs; see
# tests/stress_threads.sh for why and for the STRESS_* knobs.
test-stress-threads: $(TARGET)
	bash tests/stress_threads.sh

# The same pinned loop against a ThreadSanitizer build. TSan reports a race
# that never changed an answer, and pinning makes the interleavings it sees
# uneven. Fewer runs than the plain loop (TSan is ~10x slower); halt_on_error
# turns a report into a failed run.
test-stress-threads-tsan: tsan
	TSAN_OPTIONS="$${TSAN_OPTIONS:-halt_on_error=1}" STRESS_RUNS=$${STRESS_RUNS:-10} \
		STRESS_TIMEOUT=$${STRESS_TIMEOUT:-180} bash tests/stress_threads.sh

$(BUILD_DIR)/lexer_dump: tests/lexer_dump.c src/lexer.c src/lexer.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) src/lexer.c $< -o $@

test-lexer-diff: debug $(BUILD_DIR)/lexer_dump
	bash tests/lexer_diff.sh

test-parser-diff: debug
	bash tests/parser_diff.sh

# The full self-hosted differential corpus (lexer + parser, ~1400 cases
# plus one-off scenarios): opt-in/periodic, not part of test-all -- see
# tests/self_host_smoke.sh's own comment for why (self-hosting is in
# minimal-compat maintenance mode per docs/roadmap.md).
test-self-host: test-lexer-diff test-parser-diff

test-self-host-smoke: debug
	bash tests/self_host_smoke.sh

# CI runs these groups in separate jobs for each compiler/distro. Keep the
# local aggregate sequential: all groups share build/ and change build flags.
test-all:
	$(MAKE) test-builds
	$(MAKE) test-integration
	$(MAKE) test-fuzz

test-builds:
	$(MAKE) clean
	$(MAKE) test
	$(MAKE) clean
	$(MAKE) test-release
	$(MAKE) clean
	$(MAKE) test-sanitize
	$(MAKE) clean
	$(MAKE) test-tsan

# Start from a known debug build, whether invoked alone or after test-builds
# (which finishes with TSan objects). Targets stay sequential because many
# share generated binaries, ports, and filesystem fixtures.
test-integration:
	$(MAKE) clean
	$(MAKE) debug
	bash tools/install_local_cuts.sh . >/dev/null
	$(MAKE) test-api
	$(MAKE) test-semver
	$(MAKE) test-inline-cache-slots
	$(MAKE) test-incremental-compile
	$(MAKE) test-compiled-prelude
	$(MAKE) test-aot-cache
	$(MAKE) test-aot-kit
	$(MAKE) test-cross-build
	$(MAKE) test-fibers
	$(MAKE) test-fiber-guards
	$(MAKE) test-fiber-run
	$(MAKE) test-vm-context
	$(MAKE) test-scheduler
	$(MAKE) test-scheduler-run-all
	$(MAKE) test-fiber-gc-roots
	$(MAKE) test-nested-yield-guard
	$(MAKE) test-stack-overflow
	$(MAKE) test-facet
	$(MAKE) test-facet-new
	$(MAKE) test-source-extensions
	$(MAKE) test-database-config-package
	$(MAKE) test-http-package
	$(MAKE) test-gremlin-package
	$(MAKE) test-websocket-package
	$(MAKE) test-redis-package
	$(MAKE) test-rack-package
	$(MAKE) test-cookies-package
	$(MAKE) test-multipart-package
	$(MAKE) test-network-safety-package
	$(MAKE) test-div-package
	$(MAKE) test-dials-package
	$(MAKE) test-graphql-package
	$(MAKE) test-graphsql-package
	$(MAKE) test-mjolnir-package
	$(MAKE) test-task-board
	$(MAKE) test-logger-package
	$(MAKE) test-minitest-package
	$(MAKE) test-log-viewer-package
	$(MAKE) test-active-karma-package
	$(MAKE) test-active-auth-package
	$(MAKE) test-active-social-package
	$(MAKE) test-active-tagging-package
	$(MAKE) test-active-discussion-package
	$(MAKE) test-jobs-package
	$(MAKE) test-cancellation-package
	$(MAKE) test-job-service
	$(MAKE) test-registry-package
	$(MAKE) test-registry-http
	$(MAKE) test-deploy-registry
	$(MAKE) test-monitor
	$(MAKE) test-example-apps
	$(MAKE) test-docs
	$(MAKE) test-sanitize-script
	$(MAKE) test-registry-inventory
	$(MAKE) test-examples
	$(MAKE) test-pheint-application
	$(MAKE) test-lsp
	$(MAKE) test-dap
	$(MAKE) test-repl
	$(MAKE) test-repl-completion
	$(MAKE) test-exit
	$(MAKE) test-cache
	$(MAKE) test-self-host-smoke

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS) $(AOT_RUNTIME_OBJECTS:.o=.d) $(API_DBG_OBJECTS:.o=.d) $(FUZZ_OBJECTS:.o=.d) $(FUZZ_OBJ_DIR)/compile_fuzzer.d $(FUZZ_OBJ_DIR)/execute_fuzzer.d $(FUZZ_OBJ_DIR)/differential_fuzzer.d
