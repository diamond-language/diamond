#ifndef DIAMOND_COMPILED_PRELUDE_DATA_H
#define DIAMOND_COMPILED_PRELUDE_DATA_H

#include <stddef.h>
#include <stdint.h>

/* The build-time-generated, diamond_program_write_compiled-shaped byte
 * buffer #embed'd from $(BUILD_DIR)/compiled_prelude.bin (see
 * tools/gen_compiled_prelude.c and src/compiled_prelude.h) -- pass
 * straight through to diamond_program_read_compiled. */
const uint8_t *diamond_compiled_prelude_data(void);
size_t diamond_compiled_prelude_size(void);

#endif
