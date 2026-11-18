#ifndef DIAMOND_EMBED_H
#define DIAMOND_EMBED_H
#include <stdbool.h>
#include <stddef.h>

/* `File.embed("schema.graphql")` puts a file's contents into the compiled program as one
 * String constant, so a `d6 build` binary or a bytecode cache carries the file with it.
 *
 * The loader (src/loader.c) resolves the path against the directory of the file the call is
 * written in and rewrites the call to `File.embed("/absolute/path", "<sha256 hex>")`. The
 * digest makes the file's contents part of the expanded source, so editing the embedded file
 * invalidates a bytecode cache exactly as editing a required file does. The compiler (and the
 * self-hosted one) reads the file again and checks the digest still matches. */

enum { DIAMOND_EMBED_DIGEST_HEX = 64 };

/* False under `--sandbox` (DIAMOND_SANDBOX) unless DIAMOND_SANDBOX_ALLOW includes `filesystem`:
 * sandboxed code must not read an arbitrary file by naming it in File.embed, any more than with
 * File.read. */
bool diamond_embed_allowed(void);

/* Reads all of `path` (any bytes, NUL included) into a malloc'd buffer the caller frees, and
 * writes its SHA-256 as 64 lowercase hex characters plus a NUL into `digest_hex`. A file
 * larger than DIAMOND_MAX_STRING_CONSTANT_LENGTH is refused. On failure returns false and
 * writes a message (without the path) into `error`. `out_chars` may be nullptr when only the
 * digest is wanted. */
bool diamond_embed_read(const char *path,char **out_chars,size_t *out_length,
                        char digest_hex[DIAMOND_EMBED_DIGEST_HEX+1],
                        char *error,size_t error_capacity);

#endif
