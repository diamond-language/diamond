#ifndef DIAMOND_LSP_FORMATTING_H
#define DIAMOND_LSP_FORMATTING_H

#include "json.h"

#include <stddef.h>

/* Computes a textDocument/formatting result: a TextEdit[] normalizing
 * every physical line's own leading indentation to `depth * 2` spaces,
 * where `depth` is a running count of open blocks (def/class/module/
 * interface/struct/begin/case/loop/do, plus block-form if/unless/
 * while/until -- never a postfix modifier, see formatting.c's own
 * doc comment) and open brackets ((/[/{), and trims trailing
 * whitespace, inserts a space after a comma and around assignment,
 * comparison and logical operators where there is none (`a=1` becomes
 * `a = 1`, `f(a,b)` becomes `f(a, b)`; arithmetic and block/union bars are
 * left alone), and collapses runs of blank lines to one. Deliberately
 * narrow, not a full pretty-printer: no line-reflow, no reordering, no
 * collapsing of existing alignment -- Diamond's native compiler retains no AST at all
 * (docs/roadmap.md's "Compiler representation"), so a real structural
 * formatter would need a whole new tree-building parser kept in sync
 * with the real grammar; this instead tracks nesting from the same
 * token stream every other lsp/ handler already uses, the same
 * "narrowest useful slice" bar struct/sealed classes/sandbox mode were
 * each held to.
 *
 * Lines that start or end inside a multi-line string literal are string
 * content, not layout, and are never edited (re-indenting them would change
 * the string's value).
 *
 * Only lines whose indentation or trailing whitespace actually needs
 * to change get an edit -- an already-conventionally-formatted
 * document gets an empty array back, not a no-op edit for every line.
 * Returns `json_null()` when `text` contains a lexer error token, or
 * when the tracked bracket/block depth ever goes negative or fails to
 * return to exactly zero by EOF (an unbalanced construct, or this
 * pass's own classification failing to keep up with something in the
 * real grammar it doesn't yet model) -- safer to make no edits at all
 * than to guess at indentation for source this pass isn't confident it
 * understood. `nullptr` only on allocation failure. */
JsonValue *formatting_compute(const char *text, size_t length);

/* The same edits, limited to those that begin on a 1-based physical line in
 * [first_line, last_line] (textDocument/rangeFormatting). The nesting depth is
 * still computed from the whole document, so a range starting mid-block
 * indents correctly. Returns `json_null()` and `nullptr` under the same
 * conditions as formatting_compute. */
JsonValue *formatting_compute_range(const char *text, size_t length, size_t first_line,
        size_t last_line);

#endif
