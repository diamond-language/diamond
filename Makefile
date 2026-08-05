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

.PHONY: all debug sanitize release test clean

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

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
