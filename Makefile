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
LDLIBS := -lm $(REGINOLD_DIR)/libreginold.a -ldl -lpthread

BUILD_DIR := build
TARGET := $(BUILD_DIR)/diamond
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJECTS:.o=.d)

.PHONY: all debug sanitize release test test-release test-sanitize test-api test-fibers test-fiber-run test-fiber-context test-vm-context test-yield test-continuation test-multi-yield test-scheduler test-scheduler-run-all test-fiber-gc-roots test-fiber-guards test-nested-yield-guard test-stack-overflow test-all test-facet facet test-lexer-diff test-parser-diff lsp test-lsp clean

all: debug

debug: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_DEBUG)
debug: $(TARGET)

sanitize: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_SANITIZE)
sanitize: LDFLAGS := $(LDFLAGS_SANITIZE)
sanitize: clean $(TARGET)

release: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_RELEASE)
release: clean $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

test: debug
	bash tests/run.sh

test-release: release
	bash tests/run.sh

test-sanitize: sanitize
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 bash tests/run.sh

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

LSP_SOURCES := $(wildcard lsp/*.c)

$(BUILD_DIR)/diamond-lsp: $(LSP_SOURCES) $(API_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Ilsp $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $(LSP_SOURCES) $(LDLIBS) -o $@

lsp: $(BUILD_DIR)/diamond-lsp

test-lsp: $(BUILD_DIR)/diamond-lsp
	bash tests/lsp_test.sh

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
	$(MAKE) test-lsp
	$(MAKE) test-lexer-diff
	$(MAKE) test-parser-diff

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
