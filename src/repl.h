#ifndef DIAMOND_REPL_H
#define DIAMOND_REPL_H

#include <stdbool.h>
#include <stddef.h>

/* Runs an interactive read-eval-print loop on stdin/stdout until EOF
 * (Ctrl-D) or a fatal I/O error. Returns a process exit code. */
int diamond_repl_run(void);

/* Tab-completion's own logic, exposed here (rather than kept static in
 * repl.c) solely so tests/repl_completion_test.c can call it directly
 * without simulating raw terminal Tab bytes -- see repl.c's own doc
 * comment on each for the real contract. Not part of this header's
 * historical "just the REPL entry point" scope in spirit, but this
 * project has no separate internal-testing-header convention, and
 * these take plain char pointer / size_t rather than repl.c's private
 * ReplBuffer, so nothing beyond these two declarations needs to leak
 * out. */
size_t repl_word_start(const char *line, size_t cursor);
bool repl_compute_completions(const char *session, size_t session_length,
    const char *pending, size_t pending_length,
    const char *line, size_t cursor,
    char ***out_labels, size_t *out_count);

#endif
