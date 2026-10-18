#include "repl.h"

#include <stdlib.h>
#include <string.h>

static bool has_label(char **labels, size_t count, const char *want) {
    for (size_t index = 0; index < count; index++)
        if (strcmp(labels[index], want) == 0) return true;
    return false;
}

int main(void) {
    static const char session[] =
        "class Foo\n"
        "  def bark()\n"
        "    1\n"
        "  end\n"
        "end\n"
        "x = Foo.new()\n"
        "def outer(a)\n"
        "  local_in_scope = a\n"
        "  local_in_scope\n"
        "end\n";
    const size_t session_length = sizeof session - 1;

    char **labels = nullptr;
    size_t count = 0;

    /* Bare top-level name prefix: a class name. */
    static const char bare[] = "Fo";
    if (!repl_compute_completions(session, session_length, "", 0, bare, 2,
            &labels, &count)) return 1;
    if (count != 1 || !has_label(labels, count, "Foo")) return 2;
    for (size_t index = 0; index < count; index++) free(labels[index]);
    free(labels);

    /* Receiver completion, nothing typed after the dot -- must resolve
     * the receiver's real method even though "x." alone doesn't compile
     * on its own (confirmed directly against completion_compute_with_
     * resolver during implementation). */
    static const char dot[] = "x.";
    if (!repl_compute_completions(session, session_length, "", 0, dot, 2,
            &labels, &count)) return 3;
    if (!has_label(labels, count, "bark")) return 4;
    /* After a dot only the receiver's members are offered: not the program's own functions,
     * classes or locals -- and no label twice. */
    if (has_label(labels, count, "Foo") || has_label(labels, count, "outer") ||
        has_label(labels, count, "x")) return 11;
    for (size_t first = 0; first < count; first++)
        for (size_t second = first + 1; second < count; second++)
            if (strcmp(labels[first], labels[second]) == 0) return 12;
    for (size_t index = 0; index < count; index++) free(labels[index]);
    free(labels);

    /* Receiver completion, partial method name typed -- prefix-filtered
     * down to exactly one match. */
    static const char partial[] = "x.ba";
    if (!repl_compute_completions(session, session_length, "", 0, partial, 4,
            &labels, &count)) return 5;
    if (count != 1 || !has_label(labels, count, "bark")) return 6;
    for (size_t index = 0; index < count; index++) free(labels[index]);
    free(labels);

    /* An Array offers its API, not the prelude helpers that happen to dispatch on it. */
    static const char array_session[] = "values = [3, 1, 2]\n";
    static const char array_dot[] = "values.";
    if (!repl_compute_completions(array_session, sizeof array_session - 1, "", 0, array_dot, 7,
            &labels, &count)) return 13;
    if (!has_label(labels, count, "map") || !has_label(labels, count, "each_with_index")) return 14;
    static const char *const internal[] = {"membership", "member", "map_int", "map_string", "map_typed", "swap_first_two"};
    for (size_t index = 0; index < sizeof internal / sizeof internal[0]; index++)
        if (has_label(labels, count, internal[index])) return 15;
    for (size_t index = 0; index < count; index++) free(labels[index]);
    free(labels);

    /* Zero matches: not a failure, just an empty result. */
    static const char no_match[] = "x.zzz";
    if (!repl_compute_completions(session, session_length, "", 0, no_match, 5,
            &labels, &count)) return 7;
    if (count != 0) return 8;

    /* A local variable already committed to `session` (a prior,
     * structurally complete statement) is visible for completion later
     * -- the common REPL shape: assign on one line, complete its name
     * on the next. Deliberately not testing completion from inside a
     * *still-open* multi-line block passed via `pending` (e.g. mid-
     * typing an unclosed `def`) -- confirmed directly that this
     * currently returns zero matches rather than a real result, since
     * completion_compute_with_resolver requires the whole buffer to
     * compile and nothing here auto-closes an open block. Accepted,
     * not fixed: the common "complete what I just assigned" case this
     * covers is the one that matters most, and auto-closing arbitrary
     * unclosed blocks (how many `end`s? an unclosed string or bracket
     * too?) is real, separate scope, not a papercut in this pass. */
    static const char with_local[] = "count = 5\n";
    static const char partial_local[] = "cou";
    if (!repl_compute_completions(with_local, sizeof with_local - 1, "", 0,
            partial_local, 3, &labels, &count)) return 9;
    if (count != 1 || !has_label(labels, count, "count")) return 10;
    for (size_t index = 0; index < count; index++) free(labels[index]);
    free(labels);

    /* The caret inside a word completes the whole word: `outer` with the caret after `ou`. */
    static const char middle[] = "outer";
    if (!repl_compute_completions(session, session_length, "", 0, middle, 2,
            &labels, &count)) return 13;
    if (!has_label(labels, count, "outer")) return 14;
    for (size_t index = 0; index < count; index++) free(labels[index]);
    free(labels);

    /* A session that redefines a name still completes: the REPL replays both definitions. */
    static const char redefined[] = "def twice() = 1\ndef twice() = 2\n";
    static const char twi[] = "twi";
    if (!repl_compute_completions(redefined, sizeof redefined - 1, "", 0, twi, 3,
            &labels, &count)) return 15;
    if (!has_label(labels, count, "twice")) return 16;
    for (size_t index = 0; index < count; index++) free(labels[index]);
    free(labels);

    /* A caret past the end of the line (a byte offset counted beyond an embedded NUL) is an
     * empty result, not an out-of-bounds copy. */
    static const char with_nul[] = {'a', '\0', 'b', '\0'};
    if (!repl_compute_completions(session, session_length, "", 0, with_nul, 3,
            &labels, &count)) return 17;
    if (count != 0) return 18;

    return 0;
}
