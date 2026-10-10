#ifndef DIAMOND_LSP_ANALYSIS_CACHE_H
#define DIAMOND_LSP_ANALYSIS_CACHE_H

#include "compiler.h"

/* Single-entry cache for the sequential stdio request loop. Callers must build
 * a fresh compile buffer first so disk imports and open-document overrides are
 * included in the key. The borrowed result lives until the next cache miss or
 * clear; do not retain it across requests or use it for nested workspace scans.
 * Source bundles and position mappings remain owned by each request. */
const DiamondProgram *diamond_lsp_analyze(const char *combined);
void diamond_lsp_analysis_clear(void);

#endif
