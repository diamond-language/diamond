#ifndef DIAMOND_PRELUDE_H
#define DIAMOND_PRELUDE_H

#include <stdbool.h>
#include <stddef.h>

/* Whether `text` (the exact source that will actually be compiled --
 * the raw document/user text, or a require-resolved bundle when one
 * exists) plausibly references JSON, conservatively: any of `JSON.`,
 * `JSONCodec`, `JSONError` as a plain substring. Only ever used to
 * decide whether to *include* lib/core/json_codec.di+json.di
 * (diamond_prelude_length/diamond_prelude_write below) -- a false
 * positive here just costs the ~1.78ms those two files add to a
 * compile that didn't need them; a false negative would break
 * compilation outright, so this must stay a superset match, never
 * attempt anything cleverer (e.g. distinguishing a real reference from
 * a string literal or comment) that could narrow it. */
bool diamond_prelude_needs_json(const char *text);

/* Total byte length of the prelude diamond_prelude_write would produce
 * for the same `include_json` -- callers need this up front to size
 * their own concatenation buffer before calling either of the numeric/
 * core/string_builder(/json_codec/json) files that get embedded. */
size_t diamond_prelude_length(bool include_json);

/* Writes the prelude into `destination` (which must be at least
 * diamond_prelude_length(include_json) bytes) in a fixed order --
 * numeric, core, string_builder, and (only if include_json) json_codec,
 * json -- mirroring the concatenation every prelude-assembling call
 * site already needs immediately before appending its own "#line 1"
 * reset and the document's own source. Returns the number of bytes
 * written (always == diamond_prelude_length(include_json)), so a
 * caller can continue writing at that offset without a second
 * length computation. */
size_t diamond_prelude_write(char *destination, bool include_json);

#endif
