/* #embed's $(BUILD_DIR)/compiled_prelude.bin, generated at build time by
 * tools/gen_compiled_prelude.c -- see the Makefile's own
 * $(BUILD_DIR)/compiled_prelude.bin rule for exactly how that ordering
 * is enforced (this file, unlike src/prelude.c's own lib .di #embeds,
 * points at a build product, not a checked-in source file). */

#include "compiled_prelude_data.h"

static constexpr uint8_t DIAMOND_COMPILED_PRELUDE_BYTES[] = {
#embed "../build/compiled_prelude.bin"
};

const uint8_t *diamond_compiled_prelude_data(void) {
    return DIAMOND_COMPILED_PRELUDE_BYTES;
}

size_t diamond_compiled_prelude_size(void) {
    return sizeof(DIAMOND_COMPILED_PRELUDE_BYTES);
}
