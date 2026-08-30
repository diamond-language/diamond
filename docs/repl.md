# REPL

This document covers the interactive REPL (`build/diamond` with no file
argument, when stdin is a terminal) — line editing, history, and how it
falls back to plain-pipe behavior for scripted use.

## Multi-line, session-accumulating evaluation

Each round reads one or more physical lines and compiles the whole session
plus the pending input as one program. If the diagnostic indicates incomplete
input, such as an unclosed block, the REPL prompts for another line (`... `)
instead of reporting an error. After a successful compile, it prints the
result and retains the source so later rounds see earlier definitions and
variables.

## Interactive line editing

When stdin is a real terminal (`isatty(STDIN_FILENO)`), the REPL puts the
terminal into raw mode (`ECHO`, `ICANON`, and `ISIG` all cleared) and
reads/edits one line at a time itself, rather than deferring to the
kernel's line discipline via `getline`:

- Printable characters insert at the cursor.
- **Left/Right** move the cursor; **Home**/**End** jump to the start/end
  of the line; **Delete** removes the character under the cursor;
  **Backspace** removes the one before it.
- **Up/Down** navigate command history (see below). The first Up on a
  freshly-typed, not-yet-submitted line stashes it so Down can restore it
  after navigating back past the newest history entry — matching
  bash/readline convention.
- **Ctrl-C** discards the current line and returns to a fresh prompt
  without exiting the process. This is deliberately input-only: it does
  not interrupt a running evaluation. `ISIG` is cleared specifically so
  Ctrl-C is read as an ordinary byte (`0x03`) rather than raising a real
  `SIGINT` — this is unrelated to, and doesn't interact with, the
  `Signal.trap` language feature described in `docs/io.md`.
- **Ctrl-D** exits the REPL, but only on an empty line — on a non-empty
  line it's a no-op, matching common shell convention (not a forced
  submit, not a deletion).
- **`exit`/`quit`**, typed as a whole line by itself, also exit the REPL —
  not a language builtin (Diamond has none), recognized only here, only
  when it's the first line of a fresh statement (so it can't misfire
  partway through a multi-line block, where it's just an ordinary,
  undefined identifier like any other word). Case-sensitive, matching the
  exact spelling every other REPL with this convention (irb, pry, the
  Python REPL, ...) actually recognizes.

Redrawing is whole-line-from-scratch on every edit (`\r\x1b[K` + prompt +
buffer + a cursor-repositioning escape), not a diff against the
previously drawn state. Simpler and always correct, at the cost of a few
extra bytes written per keystroke. It does not account for the line
wrapping past the terminal's width — a known, acceptable limitation,
since the common "long input" case is already handled by multi-line
continuation (separate physical lines), not one very long single line.

CSI (arrow-key and similar) escape sequences are parsed generically —
parameter bytes collected, then dispatched on the final byte — so any
recognized-shape-but-unhandled sequence (a key combination the editor
doesn't act on) is still fully consumed rather than leaking its raw bytes
into the buffer. A bare standalone Escape keypress, with no following
CSI bytes at all, blocks waiting for the next byte rather than resolving
immediately; this is a known limitation of not implementing
read-with-a-timeout disambiguation, not something expected to matter in
practice (every arrow/function key sends its CSI bytes back-to-back).

## History

Entries persist to `$HOME/.diamond_history`, one physical line per entry,
loaded at startup and appended (with an immediate `fflush`, not buffered
until exit) as each line is submitted — so history survives a crash or
`kill -9`, not only a clean Ctrl-D exit. A missing or unwritable `$HOME`
is not fatal: history persistence is silently skipped and in-memory
navigation still works for the rest of the session.

Blank/whitespace-only lines are never added. An exact repeat of the
immediately preceding entry is skipped too, matching bash's
`ignoredups`-like default — re-running the same command repeatedly
doesn't require stepping through identical copies to get further back.

History has no size cap (no `HISTSIZE`-style trimming, in memory or in
the file) — a known simplification rather than a deliberate stance;
revisit if REPL sessions long enough for this to matter in practice turn
out to be common.

## Non-interactive fallback

When stdin is not a terminal (a pipe, a file redirect, or
`DIAMOND_FORCE_REPL=1` driving the REPL over a coprocess as
`tests/repl_test.sh` does), none of the above activates. The REPL prints the
prompt and uses a plain
`getline()` read with no editing, no history, and Ctrl-C handled however
the shell/kernel already handles it for a backgrounded or piped process.
Raw-mode line editing is only meaningful for a real terminal, and the plain
path also makes the REPL predictable in scripts and tests.
