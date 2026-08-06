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

.PHONY: all debug sanitize release test test-release test-sanitize test-api test-fibers test-all clean

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

test-all:
	$(MAKE) clean
	$(MAKE) test
	$(MAKE) clean
	$(MAKE) test-release
	$(MAKE) clean
	$(MAKE) test-sanitize
	$(MAKE) test-api
	$(MAKE) test-fibers

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
