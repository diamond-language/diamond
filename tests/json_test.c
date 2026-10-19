#include "json.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_string(const char *label, const char *source, const char *expected) {
    const char *error = nullptr;
    JsonValue *value = json_parse(source, strlen(source), &error);
    const char *chars = nullptr;
    size_t length = 0;
    if (value == nullptr || !json_as_string(value, &chars, &length) ||
        length != strlen(expected) || memcmp(chars, expected, length) != 0) {
        fprintf(stderr, "json_test: %s: got %zu bytes, expected %zu\n", label,
                chars == nullptr ? (size_t)0 : length, strlen(expected));
        failures++;
    }
    json_free(value);
}

int main(void) {
    /* Raw UTF-8 bytes in a JSON string are copied through, not re-encoded. */
    expect_string("raw 2-byte", "\"caf\xC3\xA9\"", "caf\xC3\xA9");
    expect_string("raw 3-byte", "\"\xE2\x82\xAC\"", "\xE2\x82\xAC");
    expect_string("raw 4-byte", "\"\xF0\x9F\x92\x8E\"", "\xF0\x9F\x92\x8E");
    /* Escapes still produce UTF-8. */
    expect_string("escaped", "\"caf\\u00e9\"", "caf\xC3\xA9");
    expect_string("surrogate pair", "\"\\ud83d\\udc8e\"", "\xF0\x9F\x92\x8E");
    expect_string("ascii", "\"plain\\n\"", "plain\n");
    if (failures == 0) puts("json_test: ok");
    return failures == 0 ? 0 : 1;
}
