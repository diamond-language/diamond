CC := gcc
REGINOLD_DIR := reginold
REGINOLD_LIB := $(REGINOLD_DIR)/libreginold.a
# -I/usr/include/mysql(/mysql): mariadb_config --cflags's own include path
# for MariaDB Connector/C (libmysqlclient-API-compatible) -- unlike
# sqlite3.h/libpq-fe.h, mysql.h isn't installed directly under /usr/include,
# so (unlike those two) an explicit -I is required to find it.
# -Ilsp: src/repl.c includes lsp/completion.h/json.h directly for
# Tab-completion (see REPL_COMPLETION_SOURCES below) -- global rather
# than scoped to just that one file's own compile step, since no
# src/*.h/lsp/*.h basename collision exists to make that a risk.
CPPFLAGS := -Isrc -Ilsp -I$(REGINOLD_DIR) -I/usr/include/mysql -I/usr/include/mysql/mysql
CFLAGS_COMMON := -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Werror=implicit-function-declaration
CFLAGS_DEBUG := -O0 -g3 -DDIAMOND_DEBUG
CFLAGS_RELEASE := -O3 -DNDEBUG -march=native
CFLAGS_SANITIZE := $(CFLAGS_DEBUG) -fsanitize=address,undefined \
	-fno-omit-frame-pointer
LDFLAGS_SANITIZE := -fsanitize=address,undefined
# ThreadSanitizer can't combine with ASan+UBSan above (mutually exclusive
# instrumentation), so this is its own build variant rather than an
# addition to CFLAGS_SANITIZE -- see docs/threads.md and tests/tsan_test.sh.
CFLAGS_TSAN := $(CFLAGS_DEBUG) -fsanitize=thread
LDFLAGS_TSAN := -fsanitize=thread
LDLIBS := -lm $(REGINOLD_DIR)/libreginold.a -lsqlite3 -lpq -lmariadb -ldl -lpthread -lssl -lcrypto -lcrypt

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

.PHONY: all debug sanitize tsan release test test-release test-sanitize test-tsan test-api test-fibers test-fiber-run test-fiber-context test-vm-context test-yield test-continuation test-multi-yield test-scheduler test-scheduler-run-all test-fiber-gc-roots test-fiber-guards test-nested-yield-guard test-stack-overflow test-all test-facet facet test-http-package test-gremlin-package test-rack-package test-cookies-package test-multipart-package test-div-package test-dials-package test-graphql-package test-graphsql-package test-logger-package test-log-viewer-package test-active-karma-package test-pheint-application test-lexer-diff test-parser-diff test-self-host test-self-host-smoke lsp test-lsp test-repl test-repl-completion fuzz test-fuzz clean

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

test-http-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/http/test.sh

test-gremlin-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/gremlin/test.sh

test-rack-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/rack/test.sh

test-cookies-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/cookies/test.sh

test-multipart-package: $(TARGET)
	DIAMOND_BIN=$(CURDIR)/$(BUILD_DIR)/diamond bash packages/multipart/test.sh

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
	$(CC_FUZZ) $(CPPFLAGS) $(CFLAGS_FUZZ) $(API_SOURCES) $< -lm $(REGINOLD_DIR)/libreginold.a -lsqlite3 -lpq -lmariadb -ldl -lpthread -lssl -lcrypto -lcrypt -o $@

$(BUILD_DIR)/execute_fuzzer: fuzz/execute_fuzzer.c $(API_SOURCES) $(REGINOLD_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CC_FUZZ) $(CPPFLAGS) $(CFLAGS_FUZZ) $(API_SOURCES) $< -lm $(REGINOLD_DIR)/libreginold.a -lsqlite3 -lpq -lmariadb -ldl -lpthread -lssl -lcrypto -lcrypt -o $@

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
	$(MAKE) test-http-package
	$(MAKE) test-gremlin-package
	$(MAKE) test-rack-package
	$(MAKE) test-cookies-package
	$(MAKE) test-multipart-package
	$(MAKE) test-div-package
	$(MAKE) test-dials-package
	$(MAKE) test-graphql-package
	$(MAKE) test-graphsql-package
	$(MAKE) test-logger-package
	$(MAKE) test-log-viewer-package
	$(MAKE) test-active-karma-package
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
