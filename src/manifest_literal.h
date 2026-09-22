#ifndef DIAMOND_MANIFEST_LITERAL_H
#define DIAMOND_MANIFEST_LITERAL_H

#include <stdbool.h>
#include <stddef.h>

/* Check a manifest or lockfile before the Diamond compiler sees it. This
 * deliberately narrow literal grammar cannot call code or read files. */
bool diamond_manifest_literal_validate(const char *source, char *error,
                                       size_t error_size);

#endif
