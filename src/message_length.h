#ifndef DIAMOND_MESSAGE_LENGTH_H
#define DIAMOND_MESSAGE_LENGTH_H

#include <stddef.h>
#include <stdckdint.h>

/* Bound allocations and draining work on LSP/DAP and VM debug transports. */
static constexpr size_t DIAMOND_MAX_MESSAGE_LENGTH = 64 * 1024 * 1024;

static inline bool diamond_parse_message_length(const char *text, size_t *out) {
    while (*text == ' ' || *text == '\t') text++;
    if (*text < '0' || *text > '9') return false;
    size_t length = 0;
    do {
        if (ckd_mul(&length, length, 10) ||
            ckd_add(&length, length, *text - '0') ||
            length > DIAMOND_MAX_MESSAGE_LENGTH) return false;
        text++;
    } while (*text >= '0' && *text <= '9');
    while (*text == ' ' || *text == '\t') text++;
    if (*text != '\0') return false;
    *out = length;
    return true;
}

#endif
