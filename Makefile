CC := gcc
REGINOLD_DIR := ../reginold
CPPFLAGS := -Isrc -I$(REGINOLD_DIR)
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
LDLIBS := -lm $(REGINOLD_DIR)/libreginold.a -lsqlite3 -ldl -lpthread -lssl -lcrypto

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
DEPS := $(OBJECTS:.o=.d)

.PHONY: all debug sanitize tsan release test test-release test-sanitize test-tsan test-api test-fibers test-fiber-run test-fiber-context test-vm-context test-yield test-continuation test-multi-yield test-scheduler test-scheduler-run-all test-fiber-gc-roots test-fiber-guards test-nested-yield-guard test-stack-overflow test-all test-facet facet test-http-package test-gremlin-package test-rack-package test-lexer-diff test-parser-diff lsp test-lsp test-repl fuzz test-fuzz clean

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

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Built with whichever variant (debug/sanitize/release) is currently
# active, same as $(TARGET) itself -- tests/run.sh's own file-based case
# loop runs whatever `make test`/`test-release`/`test-sanitize` just
# built, and needs run_cases to match (see docs/roadmap.md for why this
# exists: running every tests/cases/*.di case in this one process
# instead of tests/run.sh spawning a fresh `diamond` per case).
$(BUILD_DIR)/run_cases: tests/run_cases.c $(SOURCES)
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

API_SOURCES := $(filter-out src/main.c,$(SOURCES))

$(BUILD_DIR)/api_invalidation: tests/api_invalidation.c $(API_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $< $(LDLIBS) -o $@

test-api: $(BUILD_DIR)/api_invalidation
	$(BUILD_DIR)/api_invalidation

$(BUILD_DIR)/fiber_states: tests/fiber_states.c $(API_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $< $(LDLIBS) -o $@

test-fibers: $(BUILD_DIR)/fiber_states
	$(BUILD_DIR)/fiber_states

test-fiber-guards: test-fibers

test-fiber-context: test-fibers

$(BUILD_DIR)/fiber_run: tests/fiber_run.c $(API_SOURCES)
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

$(BUILD_DIR)/facet: tools/facet.c $(API_SOURCES)
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

LSP_SOURCES := $(wildcard lsp/*.c)

$(BUILD_DIR)/diamond-lsp: $(LSP_SOURCES) $(API_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ilsp $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $(LSP_SOURCES) $(LDLIBS) -o $@

lsp: $(BUILD_DIR)/diamond-lsp

test-lsp: $(BUILD_DIR)/diamond-lsp
	bash tests/lsp_test.sh

test-repl: debug
	bash tests/repl_test.sh

$(BUILD_DIR)/compile_fuzzer: fuzz/compile_fuzzer.c $(API_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC_FUZZ) $(CPPFLAGS) $(CFLAGS_FUZZ) $(API_SOURCES) $< -lm $(REGINOLD_DIR)/libreginold.a -lsqlite3 -ldl -lpthread -lssl -lcrypto -o $@

$(BUILD_DIR)/execute_fuzzer: fuzz/execute_fuzzer.c $(API_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC_FUZZ) $(CPPFLAGS) $(CFLAGS_FUZZ) $(API_SOURCES) $< -lm $(REGINOLD_DIR)/libreginold.a -lsqlite3 -ldl -lpthread -lssl -lcrypto -o $@

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
	$(MAKE) test-lsp
	$(MAKE) test-repl
	$(MAKE) test-fuzz
	$(MAKE) test-lexer-diff
	$(MAKE) test-parser-diff

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
