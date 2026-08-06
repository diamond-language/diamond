CC := gcc
CPPFLAGS := -Isrc
CFLAGS_COMMON := -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Werror=implicit-function-declaration
CFLAGS_DEBUG := -O0 -g3 -DDIAMOND_DEBUG
CFLAGS_RELEASE := -O3 -DNDEBUG -march=native
CFLAGS_SANITIZE := $(CFLAGS_DEBUG) -fsanitize=address,undefined \
	-fno-omit-frame-pointer
LDFLAGS_SANITIZE := -fsanitize=address,undefined

BUILD_DIR := build
TARGET := $(BUILD_DIR)/diamond
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJECTS:.o=.d)

.PHONY: all debug sanitize release test test-release test-sanitize test-api test-fibers test-fiber-run test-fiber-context test-vm-context test-yield test-continuation test-multi-yield test-scheduler test-scheduler-run-all test-fiber-gc-roots test-fiber-guards test-all clean

all: debug

debug: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_DEBUG)
debug: $(TARGET)

sanitize: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_SANITIZE)
sanitize: LDFLAGS := $(LDFLAGS_SANITIZE)
sanitize: clean $(TARGET)

release: CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_RELEASE)
release: clean $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

test: debug
	bash tests/run.sh

test-release: release
	bash tests/run.sh

test-sanitize: sanitize
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 bash tests/run.sh

API_SOURCES := $(filter-out src/main.c,$(SOURCES))

$(BUILD_DIR)/api_invalidation: tests/api_invalidation.c $(API_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $< -o $@

test-api: $(BUILD_DIR)/api_invalidation
	$(BUILD_DIR)/api_invalidation

$(BUILD_DIR)/fiber_states: tests/fiber_states.c $(API_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $< -o $@

test-fibers: $(BUILD_DIR)/fiber_states
	$(BUILD_DIR)/fiber_states

test-fiber-guards: test-fibers

test-fiber-context: test-fibers

$(BUILD_DIR)/fiber_run: tests/fiber_run.c $(API_SOURCES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) $(API_SOURCES) $< -o $@

test-fiber-run: $(BUILD_DIR)/fiber_run
	$(BUILD_DIR)/fiber_run

test-vm-context: test-fiber-run

test-yield: test-fiber-run

test-continuation: test-fiber-run

test-multi-yield: test-fiber-run

test-scheduler: test-fiber-run

test-scheduler-run-all: test-fiber-run

test-fiber-gc-roots: test-fiber-run

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

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
