#include "position.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(const char *label, size_t actual, size_t expected) {
    if (actual != expected) {
        fprintf(stderr, "position_test: %s: got %zu, expected %zu\n", label, actual, expected);
        failures++;
    }
}

int main(void) {
    /* line 1: ascii. line 2: cafe + acute e (2 bytes), euro sign (3 bytes), then ascii.
     * line 3: an emoji (4 bytes, 2 UTF-16 units) followed by ascii. */
    const char *text = "plain\ncaf\xC3\xA9 \xE2\x82\xAC x\n\xF0\x9F\x92\x8E y\n";
    const size_t length = strlen(text);

    expect("ascii column", lsp_character_for(text, length, 1, 3), 3);
    /* "caf" 3, e-acute (2 bytes -> 1 unit) => byte 5 is unit 4 */
    expect("after 2-byte", lsp_character_for(text, length, 2, 5), 4);
    /* "cafe-acute " is 6 bytes -> 5 units; the euro sign is 3 bytes, 1 unit: byte 9 -> unit 6 */
    expect("after 3-byte", lsp_character_for(text, length, 2, 9), 6);
    /* emoji is 4 bytes, 2 units: byte 4 -> unit 2, the space and y follow */
    expect("after 4-byte", lsp_character_for(text, length, 3, 4), 2);
    expect("after emoji and space", lsp_character_for(text, length, 3, 5), 3);

    expect("back, ascii", lsp_byte_column_for(text, length, 1, 3), 3);
    expect("back, 2-byte", lsp_byte_column_for(text, length, 2, 4), 5);
    expect("back, 3-byte", lsp_byte_column_for(text, length, 2, 6), 9);
    expect("back, emoji", lsp_byte_column_for(text, length, 3, 2), 4);
    /* a position between the halves of a surrogate pair rounds down to the character's start */
    expect("back, inside surrogate pair", lsp_byte_column_for(text, length, 3, 1), 0);
    /* past the end of the line clamps to its length (line 2 is 11 bytes) */
    expect("back, past end", lsp_byte_column_for(text, length, 2, 99), 11);

    /* a character boundary survives the round trip */
    expect("round trip boundary", lsp_byte_column_for(text, length, 2, lsp_character_for(text, length, 2, 9)), 9);

    /* a missing line or text falls back to the byte count */
    expect("no such line", lsp_character_for(text, length, 9, 7), 7);
    expect("no text", lsp_character_for(nullptr, 0, 1, 7), 7);
    expect("last line without newline", lsp_character_for("ab", 2, 1, 2), 2);

    if (failures == 0) puts("position_test: ok");
    return failures == 0 ? 0 : 1;
}
