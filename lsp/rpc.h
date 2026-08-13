#ifndef DIAMOND_LSP_RPC_H
#define DIAMOND_LSP_RPC_H

#include "json.h"

#include <stdio.h>

/* The LSP wire protocol: an HTTP-style `Content-Length: N` header block
 * terminated by a blank line, then exactly N raw bytes of JSON -- not
 * line-delimited, so reading the body needs an exact byte count, not
 * gets()-style line reads. */

/* Reads one framed message from `stream`. Returns nullptr on clean EOF
 * (before any header was read -- the normal way a client closes the
 * connection) with *error left untouched, or on a framing/parse error
 * with *error set to a static description. `error` may be nullptr if the
 * caller doesn't need to distinguish the two. */
JsonValue *rpc_read_message(FILE *stream,const char **error);

/* Serializes `message` and writes it framed to `stream`, then flushes.
 * Returns false on a serialization or write failure. */
bool rpc_write_message(FILE *stream,const JsonValue *message);

#endif
