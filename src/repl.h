#ifndef DIAMOND_REPL_H
#define DIAMOND_REPL_H

/* Runs an interactive read-eval-print loop on stdin/stdout until EOF
 * (Ctrl-D) or a fatal I/O error. Returns a process exit code. */
int diamond_repl_run(void);

#endif
