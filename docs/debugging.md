# Debugging

Diamond's only debugging primitive at the language level is the source
`debugger()`/`breakpoint()` call ([Runtime features and
debugging](runtime-reference.md#debugging)): it pauses execution, prints
the pause site and every currently-live local, then blocks on one line of
stdin before resuming. `dap/` adds a real Debug Adapter Protocol (DAP)
server, `diamond-dap`, that gives an editor's own gutter breakpoints the
identical pause -- without editing source -- for VS Code's "Run & Debug"
view or any other DAP-compatible client (`editors/vscode`'s own
"Debugging" section covers the VS Code side specifically).

## What this is (v1 scope)

Editor-settable breakpoints, a real call stack, and locals at the paused
frame -- reusing the exact pause/print/locals machinery
`debugger()`/`breakpoint()` already had, not a new stepping engine. Two
deliberate limitations, in exchange for that low-risk scope:

- **No step-over/into/out.** `continue` is the only resume command a
  paused debuggee understands.
- **Changing breakpoints means restarting the debuggee.** Every
  breakpoint is compiled in as a `DIAMOND_OP_DEBUGGER` pause *before the
  debuggee starts running at all* -- there is no way to add or remove one
  against an already-running process.
- **Only the innermost, paused frame has locals.** An outer call-stack
  frame's own locals aren't tracked anywhere the VM can reconstruct after
  the fact (no general per-chunk local-debug table exists; only the exact
  `debugger()`/breakpoint call site's own baked-in operand list does) --
  `stackTrace` still shows every frame's own name/file/line, just no
  variables for anything but frame 0.
- **A breakpoint inside a spawned `Thread` is invisible.** Each `Thread`
  runs its own independent VM/heap (`docs/threads.md`); only the main VM's
  pauses reach the control channel below.
- **No interactive stdin forwarding to the debuggee while paused.**

Both bigger limitations (real stepping, live no-restart breakpoints) are
the explicit v2 direction -- see `docs/roadmap.md`.

## How it fits together

```text
editor (DAP client)  <--stdio, DAP-->  diamond-dap  <--control socket-->  diamond (the debuggee)
```

- `diamond-dap` (`dap/main.c`) speaks ordinary DAP over its own stdio
  (`Content-Length`-framed JSON, via `lsp/json.c`/`lsp/rpc.c` -- the
  identical transport `diamond-lsp` already uses).
- `setBreakpoints` only records `(source path, line)` pairs; the real
  spawn waits for `configurationDone` (DAP's own signal that a client is
  done sending initial breakpoints) rather than `launch` itself, so a
  breakpoint that arrives after `launch` but before `configurationDone`
  still lands in the compiled program.
- At `configurationDone`, `diamond-dap` loads and require-expands the
  program exactly the way the CLI's own non-template compile path does
  (`run_source_from_bundle_program`, `src/run_source.c`), resolves each
  stored breakpoint's original `(path, line)` against that expanded
  buffer (`diamond_resolve_source_position` + `diamond_combined_buffer_line`,
  `src/compiler.h`), and spawns `diamond <program> [args...]` with two
  environment variables set (see below) and a dedicated `AF_UNIX`
  `SOCK_STREAM` socketpair wired to one end as the "control channel" --
  `DIAMOND_DEBUG_FD` names its fd number in the child.
- The debuggee's stdout/stderr are piped back as DAP `output` events; its
  exit becomes `exited`/`terminated` events.
- Every `DIAMOND_OP_DEBUGGER` pause (compiled-in or explicit `debugger()`)
  writes one hand-formatted JSON payload, framed the same
  `Content-Length` way, to the control fd (`debugger_structured_helper`,
  `src/vm.c`) and then blocks reading exactly one framed command back
  (`{"command":"continue"}` is the only one v1 needs) before resuming --
  the VM itself never links a JSON library for this; only `diamond-dap`
  (which needs to speak real DAP to its own client) does.
- `diamond-dap` translates that payload's `(name, line, column)` --
  expressed in the same expanded-buffer terms the compiler itself
  used -- back to `(original file, original line)` via
  `diamond_resolve_diagnostic_location`, the same reverse mapping
  ordinary compiler diagnostics already use, and turns it into a DAP
  `stopped` event plus whatever `stackTrace`/`scopes`/`variables` the
  client asks for next.

## Known gap: breakpoints across multiple `require`d files

A compiled program's line numbers reset to 1 at the start of every
segment (`diamond_lexer_next`'s own `#line 1` handling, `src/lexer.c` --
the loader writes that literal marker ahead of the top-level user source
and every `require`d file's own inlined text, `src/loader.c`'s `expand`).
`DIAMOND_DEBUG_BREAKPOINTS` (below) is a flat set of these per-segment
line numbers with no file discriminator, so a breakpoint on line *N* in
one file and an unrelated statement that happens to start on line *N* in
a different file (the entry script, or another `require`d file) currently
pause identically -- not caught or resolved, just an open, documented gap
for a program that spans more than one file's own breakpoints landing on
the same line number in each.

## The env-var contract (for a second DAP-compatible client)

- `DIAMOND_DEBUG_FD=<fd>` -- an already-open, connected file descriptor
  number in the child process (read once, in `diamond_vm_init`,
  `src/vm.c`) that every `DIAMOND_OP_DEBUGGER` pause writes its
  `{"event":"paused","stack":[...],"locals":[...]}` payload to and reads
  one framed command back from, on the *same* fd -- a plain pipe can't do
  that; it needs to be a full-duplex descriptor (a `socketpair` end, as
  `dap/main.c` uses). Unset (or a negative/unparseable value) keeps
  `debugger()`/`breakpoint()`'s ordinary print-to-stdout/`getchar()`
  behavior, unaffected by anything below.
- `DIAMOND_DEBUG_BREAKPOINTS=<line>[,<line>...]` -- a comma-separated list
  of the *expanded-buffer* line numbers (not the original file's own,
  except in the common single-file, no-`require` case where they're
  identical -- see `diamond_combined_buffer_line`'s own doc comment,
  `src/compiler.h`) to pause at the start of. Read once by
  `diamond_run_source` (`src/run_source.c`), which forces the ordinary
  live prelude+source compile path (never the embedded-prelude-template
  fast path `diamond_run_source` otherwise prefers) whenever this is set,
  so `diamond_compile_with_breakpoints`'s own `breakpoint_lines` always
  lands against the exact buffer layout a client resolved positions
  against. A line with no statement start on it (blank, a comment,
  mid-expression) silently has no effect, matching how an editor already
  snaps a gutter breakpoint to the nearest valid line for most languages.

## Building it

```sh
make dap        # build/diamond-dap
make test-dap   # tests/dap_test.sh -- a scripted setBreakpoints+launch+
                 # configurationDone+continue session against a fixture
```

`make test-all` already includes `test-dap`.
