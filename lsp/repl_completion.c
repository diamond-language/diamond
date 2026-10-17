#include "repl.h"

#include "analysis_cache.h"
#include "completion.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

/* The REPL's completion logic. Kept apart from src/repl.c (the terminal loop) so the WebAssembly
 * build, which has no terminal, can compile it for the website REPL. */

/* Scans backward from `cursor` over identifier characters (letters,
 * digits, underscore) to find where the word being completed starts.
 * Deliberately doesn't look at '.' specially -- a dot isn't an
 * identifier character, so scanning naturally stops right after one,
 * and completion_compute_with_resolver's own receiver resolution
 * (lsp/receiver.c) only cares about tokens strictly before the query
 * offset, not about repl.c distinguishing "bare name" from
 * "receiver.name" completion itself. */
static bool repl_is_identifier_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

size_t repl_word_start(const char *line, size_t cursor) {
    size_t start = cursor;
    while (start > 0 && repl_is_identifier_char((unsigned char)line[start - 1])) start--;
    return start;
}

void repl_free_completions(char **labels, size_t count) {
    if (labels == nullptr) return;
    for (size_t index = 0; index < count; index++) free(labels[index]);
    free(labels);
}

/* Computes Tab-completion candidates for a REPL session: everything
 * already committed (`session`), the current in-progress statement's
 * earlier lines (`pending`), and the line being typed right now
 * (`line`, full text -- not truncated at `cursor`, since completion
 * needs to see what's after the cursor too, exactly as
 * completion_compute's own LSP callers already provide the whole
 * document). Plain char pointer / size_t parameters rather than ReplBuffer*,
 * deliberately -- ReplBuffer is private to this file, and this
 * function is called directly (not through the raw-terminal byte
 * loop) by tests/repl_completion_test.c, so it can't depend on a type
 * only visible here.
 *
 * The word at `cursor` (found via repl_word_start) is replaced with a
 * harmless placeholder identifier before compiling: `completion_
 * compute_with_resolver` requires the whole buffer to compile, and a
 * dangling `.`/partial name is a parse error on its own -- confirmed
 * directly (not assumed) that substituting a placeholder and querying
 * at the *original* (pre-substitution) offset still resolves a
 * receiver's real methods correctly, since receiver resolution only
 * looks at tokens strictly before the query offset. The returned
 * candidate list is deliberately unfiltered by completion_compute_
 * with_resolver itself (every caller narrows it -- see completion.h),
 * so this prefix-filters by whatever was actually typed before
 * replacing it.
 *
 * On success (including zero matches, which is not a failure), returns
 * true with `*out_labels`/`*out_count` set (freed via
 * repl_free_completions -- caller's responsibility); returns false
 * only on allocation failure, with both output parameters left at
 * nullptr/0. */
bool repl_compute_completions(const char *session, size_t session_length,
        const char *pending, size_t pending_length,
        const char *line, size_t cursor,
        char ***out_labels, size_t *out_count) {
    *out_labels = nullptr;
    *out_count = 0;
    /* A session redefines names the way the REPL evaluates them. */
    diamond_lsp_analysis_allow_redefinition(true);
    const size_t line_length = strlen(line);
    /* A cursor past the end of the line (the caller's offset counts bytes after an embedded NUL,
     * say) has nothing to complete; going on would size the copies below from it. */
    if (cursor > line_length) return true;
    const size_t word_start_in_line = repl_word_start(line, cursor);
    /* The word being completed also covers identifier characters after the caret: with the
     * caret inside `squared`, only `squ` is replaced by the placeholder, which would leave
     * `__cared` behind, an undefined name that stops the buffer compiling. The candidates are
     * still filtered by what was typed before the caret. */
    size_t word_end_in_line = cursor;
    while (word_end_in_line < line_length && repl_is_identifier_char((unsigned char)line[word_end_in_line]))
        word_end_in_line++;

    const size_t full_length = session_length + pending_length + line_length;
    char *full_text = malloc(full_length + 1);
    if (full_text == nullptr) return false;
    memcpy(full_text, session, session_length);
    memcpy(full_text + session_length, pending, pending_length);
    const size_t line_offset_in_full = session_length + pending_length;
    memcpy(full_text + line_offset_in_full, line, line_length);
    full_text[full_length] = '\0';

    const size_t word_start = line_offset_in_full + word_start_in_line;
    const size_t word_end = line_offset_in_full + word_end_in_line;
    const size_t typed_prefix_length = cursor - word_start_in_line;

    /* The placeholder identifier also replaces a *bare* (non-receiver)
     * partial name -- e.g. completing a fresh top-level statement like
     * `Fo<TAB>` -- where it's read as an ordinary expression, not just
     * a method name after a `.`. Diamond statically rejects a bare read
     * of an undefined name (confirmed directly: a document ending in
     * `__c` alone, with `__c` never assigned anywhere, fails to compile
     * with "undefined local variable" -- not a runtime-only check).
     * Pre-declaring `__c = nil` as a real local on its own line at the
     * very start of the buffer makes every later bare reference to it
     * valid regardless of what expression-shaped position it lands in
     * (a plain read, a reassignment target, ...), while a receiver
     * position (`x.__c`) was already unaffected either way, since a
     * method name's own validity doesn't depend on any local by that
     * name existing. Every offset downstream of this prefix shifts by
     * exactly its own length; query_line's own "+1" below accounts for
     * it being exactly one additional whole line. */
    static const char placeholder_declaration[] = "__c = nil\n";
    const size_t declaration_length = sizeof placeholder_declaration - 1;
    static const char placeholder[] = "__c";
    const size_t placeholder_length = sizeof placeholder - 1;
    const size_t suffix_length = full_length - word_end;
    const size_t mutated_length =
        declaration_length + word_start + placeholder_length + suffix_length;
    char *mutated_text = malloc(mutated_length + 1);
    if (mutated_text == nullptr) { free(full_text); return false; }
    memcpy(mutated_text, placeholder_declaration, declaration_length);
    memcpy(mutated_text + declaration_length, full_text, word_start);
    memcpy(mutated_text + declaration_length + word_start, placeholder, placeholder_length);
    memcpy(mutated_text + declaration_length + word_start + placeholder_length,
        full_text + word_end, suffix_length);
    mutated_text[mutated_length] = '\0';

    size_t query_line = 1, query_character = 0;
    for (size_t index = 0; index < word_start; index++) {
        if (full_text[index] == '\n') { query_line++; query_character = 0; }
        else query_character++;
    }

    /* resolver=nullptr, resolver_data=nullptr: "no override, fall back
     * to reading require'd files from disk" -- already a fully
     * supported mode (DiamondSourceOverride, src/loader.h), exactly
     * correct for a REPL with no open-document table. path=nullptr:
     * a REPL session is never file-backed, matching completion_
     * compute's own "untitled document" mode. */
    JsonValue *result = completion_compute_with_resolver(nullptr, nullptr,
        nullptr, mutated_text, mutated_length, query_line, query_character);
    free(full_text);
    free(mutated_text);
    if (result == nullptr) return false;
    if (result->kind != JSON_ARRAY) { json_free(result); return true; }

    const char *typed_prefix = line + word_start_in_line;
    char **labels = nullptr;
    size_t count = 0, capacity = 0;
    for (size_t index = 0; index < result->as.array.count; index++) {
        const JsonValue *item = result->as.array.items[index];
        const JsonValue *label_value = json_object_get(item, "label");
        const char *label_chars = nullptr;
        size_t label_length = 0;
        if (label_value == nullptr ||
            !json_as_string(label_value, &label_chars, &label_length)) continue;
        /* The placeholder's own pre-declared local (see above) --
         * never a real completion candidate, just an implementation
         * artifact that would otherwise leak into an unfiltered
         * (nothing typed yet) result. */
        if (label_length == placeholder_length &&
            memcmp(label_chars, placeholder, placeholder_length) == 0) continue;
        if (label_length < typed_prefix_length) continue;
        if (typed_prefix_length > 0 &&
            memcmp(label_chars, typed_prefix, typed_prefix_length) != 0) continue;
        if (count == capacity) {
            const size_t grown_capacity = capacity == 0 ? 8 : capacity * 2;
            char **grown = realloc(labels, grown_capacity * sizeof *labels);
            if (grown == nullptr) {
                repl_free_completions(labels, count);
                json_free(result);
                return false;
            }
            labels = grown;
            capacity = grown_capacity;
        }
        labels[count] = malloc(label_length + 1);
        if (labels[count] == nullptr) {
            repl_free_completions(labels, count);
            json_free(result);
            return false;
        }
        memcpy(labels[count], label_chars, label_length);
        labels[count][label_length] = '\0';
        count++;
    }
    json_free(result);
    *out_labels = labels;
    *out_count = count;
    return true;
}
