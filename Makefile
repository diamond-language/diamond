CC := gcc
REGINOLD_DIR := reginold
REGINOLD_LIB := $(REGINOLD_DIR)/libreginold.a
# -I/usr/include/mysql(/mysql): mariadb_config --cflags's own include path
# for MariaDB Connector/C (libmysqlclient-API-compatible) -- mysql.h isn't
# installed directly under /usr/include, so an explicit -I is required to
# find it, on every distro tested so far.
# -I/usr/include/postgresql: libpq-fe.h's location is itself distro-
# dependent, confirmed the hard way deploying to a real Ubuntu box after
# every prior build/test of this project happened on Fedora -- Fedora's
# libpq-devel installs it directly under /usr/include (so this extra -I
# was previously believed unnecessary, per this comment's own prior
# wording), but Debian/Ubuntu's libpq-dev installs it under
# /usr/include/postgresql instead. Harmless to add unconditionally on
# distros where it's not needed -- gcc silently ignores a nonexistent -I
# path -- so there's no reason to special-case this per platform.
# -Ilsp: src/repl.c includes lsp/completion.h/json.h directly for
# Tab-completion (see REPL_COMPLETION_SOURCES below) -- global rather
# than scoped to just that one file's own compile step, since no
# src/*.h/lsp/*.h basename collision exists to make that a risk.
CPPFLAGS := -Isrc -Ilsp -I$(REGINOLD_DIR) -I/usr/include/mysql -I/usr/include/mysql/mysql \
	-I/usr/include/postgresql
CFLAGS_COMMON := -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Werror=implicit-function-declaration
CFLAGS_DEBUG := -O0 -g3 -DDIAMOND_DEBUG
CFLAGS_RELEASE := -O3 -DNDEBUG -march=native
# -O1, not CFLAGS_DEBUG's -O0: run_chunk (src/vm.c) is one ~6,600-line
# function whose giant opcode switch declares its own locals (registers,
# per-opcode buffers, DiamondTypeBinding[8] arrays for generic-call
# opcodes, TLS setup buffers, etc.) in dozens of mutually-exclusive case
# blocks. -O0 disables stack-slot coalescing across non-overlapping
# lexical scopes, so every one of those locals gets its own permanent
# slot in one shared frame regardless of which case actually runs --
# confirmed via -fstack-usage at 105,680 bytes/frame, enough that
# depth(5000) (the DIAMOND_MAX_CALL_DEPTH regression test) hit a real
# ASan stack-overflow at ~71 recursive frames, well before
# DIAMOND_MAX_CALL_DEPTH=95's own guard (src/vm.c) could trip. -O1
# restores stack-slot coalescing (measured 68,624 bytes/frame, ~35%
# smaller) while keeping ASan/UBSan instrumentation and frame pointers
# (-fno-omit-frame-pointer) fully intact for readable backtraces; some
# locals may show "optimized out" under gdb, an accepted tradeoff scoped
# to this diagnostic build only -- `debug` stays -O0 for full
# variable visibility.
CFLAGS_SANITIZE := -O1 -g3 -DDIAMOND_DEBUG -fsanitize=address,undefined \
	-fno-omit-frame-pointer
LDFLAGS_SANITIZE := -fsanitize=address,undefined
# ThreadSanitizer can't combine with ASan+UBSan above (mutually exclusive
# instrumentation), so this is its own build variant rather than an
# addition to CFLAGS_SANITIZE -- see docs/threads.md and tests/tsan_test.sh.
CFLAGS_TSAN := $(CFLAGS_DEBUG) -fsanitize=thread
LDFLAGS_TSAN := -fsanitize=thread
LDLIBS := -lm $(REGINOLD_DIR)/libreginold.a -lsqlite3 -lpq -lmariadb -ldl -lpthread -lssl -lcrypto -lcrypt -lz

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
# The REPL's Tab-completion (src/repl.c) reuses lsp/completion.c's
# completion_compute_with_resolver directly (in-process, no LSP
# transport) rather than re-implementing candidate-list logic -- these
# are its own transitive dependencies (compile_buffer.c for the shared
# "build the buffer diamond_compile expects" step, receiver.c for
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
REPL_COMPLETION_SOURCES := lsp/completion.c lsp/compile_buffer.c \
	lsp/receiver.c lsp/json.c
REPL_COMPLETION_OBJECTS := $(REPL_COMPLETION_SOURCES:lsp/%.c=$(BUILD_DIR)/lsp-%.o)
DEPS := $(OBJECTS:.o=.d) $(REPL_COMPLETION_OBJECTS:.o=.d)

.PHONY: all debug sanitize tsan release test test-release test-sanitize test-tsan test-api test-fibers test-fiber-run test-fiber-context test-vm-context test-yield test-continuation test-multi-yield test-scheduler test-scheduler-run-all test-fiber-gc-roots test-fiber-guards test-nested-yield-guard test-stack-overflow test-all test-facet facet test-database-config-package test-http-package test-gremlin-package test-websocket-package test-redis-package test-rack-package test-cookies-package test-multipart-package test-network-safety-package test-div-package test-dials-package test-graphql-package test-graphsql-package test-logger-package test-log-viewer-package test-active-karma-package test-active-auth-package test-active-social-package test-active-tagging-package test-active-discussion-package test-pheint-application test-lexer-diff test-parser-diff test-self-host test-self-host-smoke lsp test-lsp test-repl test-repl-completion fuzz test-fuzz clean

all: debug

debug: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_DEBUG)
debug: $(TARGET) $(BUILD_DIR)/run_cases

sanitize: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_SANITIZE)
sanitize: LDFLAGS := $(LDFLAGS_SANITIZE)
sanitize: clean $(TARGET) $(BUILD_DIR)/run_cases

tsan: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_TSAN)
tsan: LDFLAGS := $(LDFLAGS_TSAN)
tsan: clean $(TARGET) $(BUILD_DIR)/run_cases

release: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_RELEASE)
release: clean $(TARGET) $(BUILD_DIR)/run_cases

$(REGINOLD_LIB):
	$(MAKE) -C $(REGINOLD_DIR) libreginold.a

$(TARGET): $(OBJECTS) $(REPL_COMPLETION_OBJECTS) $(REGINOLD_LIB)
	$(CC) $(OBJECTS) $(REPL_COMPLETION_OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/lsp-%.o: lsp/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Built with whichever variant (debug/sanitize/release) is currently
# active, same as $(TARGET) itself -- tests/run.sh's own file-based case
# loop runs whatever `make test`/`test-release`/`test-sanitize` just
# built, and needs run_cases to match (see docs/roadmap.md for why this
# exists: running every tests/cases/*.di case in this one process
# instead of tests/run.sh spawning a fresh `diamond` per case).
$(BUILD_DIR)/run_cases: tests/run_cases.c $(SOURCES) lib/core.di $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(API_SOURCES) $< $(LDFLAGS) $(LDLIBS) -o $@

test: debug
	bash tests/run.sh

test-release: release
	bash tests/run.sh

test-sanitize: sanitize
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 bash tests/run.sh

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

$(BUILD_DIR)/api_invalidation: tests/api_invalidation.c $(API_SOURCES) $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $< $(LDLIBS) -o $@

test-api: $(BUILD_DIR)/api_invalidation
	$(BUILD_DIR)/api_invalidation

$(BUILD_DIR)/fiber_states: tests/fiber_states.c $(API_SOURCES) $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $< $(LDLIBS) -o $@

test-fibers: $(BUILD_DIR)/fiber_states
	$(BUILD_DIR)/fiber_states

$(BUILD_DIR)/repl_completion_test: tests/repl_completion_test.c src/repl.c \
		$(API_SOURCES) $(REPL_COMPLETION_SOURCES) $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) src/repl.c $(API_SOURCES) \
		$(REPL_COMPLETION_SOURCES) $< $(LDLIBS) -o $@

test-repl-completion: $(BUILD_DIR)/repl_completion_test
	$(BUILD_DIR)/repl_completion_test

test-fiber-guards: test-fibers

test-fiber-context: test-fibers

$(BUILD_DIR)/fiber_run: tests/fiber_run.c $(API_SOURCES) $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $< $(LDLIBS) -o $@

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

$(BUILD_DIR)/facet: tools/facet.c $(API_SOURCES) $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $< $(LDLIBS) -o $@

facet: $(BUILD_DIR)/facet

test-facet: $(BUILD_DIR)/facet
	bash tests/facet_test.sh

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

test-logger-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/logger/test.sh

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

test-pheint-application: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash applications/pheint.dia/test.sh

LSP_SOURCES := $(wildcard lsp/*.c)

$(BUILD_DIR)/diamond-lsp: $(LSP_SOURCES) $(API_SOURCES) $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ilsp $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $(LSP_SOURCES) $(LDLIBS) -o $@

lsp: $(BUILD_DIR)/diamond-lsp

test-lsp: $(BUILD_DIR)/diamond-lsp
	bash tests/lsp_test.sh

test-repl: debug
	bash tests/repl_test.sh

test-exit: debug
	bash tests/exit_test.sh

$(BUILD_DIR)/compile_fuzzer: fuzz/compile_fuzzer.c $(API_SOURCES) $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC_FUZZ) $(CPPFLAGS) $(CFLAGS_FUZZ) $(API_SOURCES) $< -lm $(REGINOLD_DIR)/libreginold.a -lsqlite3 -lpq -lmariadb -ldl -lpthread -lssl -lcrypto -lcrypt -lz -o $@

$(BUILD_DIR)/execute_fuzzer: fuzz/execute_fuzzer.c $(API_SOURCES) $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC_FUZZ) $(CPPFLAGS) $(CFLAGS_FUZZ) $(API_SOURCES) $< -lm $(REGINOLD_DIR)/libreginold.a -lsqlite3 -lpq -lmariadb -ldl -lpthread -lssl -lcrypto -lcrypt -lz -o $@

fuzz: $(BUILD_DIR)/compile_fuzzer $(BUILD_DIR)/execute_fuzzer

test-fuzz: $(BUILD_DIR)/compile_fuzzer $(BUILD_DIR)/execute_fuzzer
	bash tests/fuzz_smoke.sh

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

test-all:
	$(MAKE) clean
	$(MAKE) test
	$(MAKE) clean
	$(MAKE) test-release
	$(MAKE) clean
	$(MAKE) test-sanitize
	$(MAKE) clean
	$(MAKE) test-tsan
	$(MAKE) test-api
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
	$(MAKE) test-logger-package
	$(MAKE) test-log-viewer-package
	$(MAKE) test-active-karma-package
	$(MAKE) test-active-auth-package
	$(MAKE) test-active-social-package
	$(MAKE) test-active-tagging-package
	$(MAKE) test-active-discussion-package
	$(MAKE) test-pheint-application
	$(MAKE) test-lsp
	$(MAKE) test-repl
	$(MAKE) test-repl-completion
	$(MAKE) test-exit
	$(MAKE) test-fuzz
	$(MAKE) test-self-host-smoke

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
