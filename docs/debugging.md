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

## What this is

Editor-settable breakpoints, a real call stack, locals at the paused
frame, and `next`/`stepIn`/`stepOut` -- reusing the exact pause/print/
locals machinery `debugger()`/`breakpoint()` already had, not a separate
stepping engine. Breakpoints can be added or removed at any time,
including against an already-running debuggee, with no restart needed
(see "Live breakpoints" below). Remaining deliberate limitations:

- **Only the innermost, paused frame has locals.** An outer call-stack
  frame's own locals aren't tracked anywhere the VM can reconstruct after
  the fact (no general per-chunk local-debug table exists; only the exact
  paused call site's own baked-in operand list does) --
  `stackTrace` still shows every frame's own name/file/line, just no
  variables for anything but frame 0.
- **A breakpoint inside a spawned `Thread` is invisible.** Each `Thread`
  runs its own independent VM/heap (`docs/threads.md`); only the main VM's
  pauses reach the control channel below.
- **No interactive stdin forwarding to the debuggee while paused.**

## Live breakpoints

Every statement is compiled with a `DIAMOND_OP_BREAKPOINT_CHECK` --
regardless of which lines (if any) were selected as breakpoints when the
debuggee launched -- whenever `DIAMOND_DEBUG_FD` is set at all. Unlike
the explicit `debugger()`/`breakpoint()` call's own `DIAMOND_OP_DEBUGGER`
(which always pauses, unconditionally, the instant it's reached), this
opcode checks a runtime, freely mutable set of "currently armed" lines
(`DiamondVm.debug_active_lines`, `src/vm.h`) before deciding whether to
actually pause. A `setBreakpoints` request sent at any time -- before
launch, while the debuggee is running, or while it's already stopped at
a different line -- replaces that set wholesale and takes effect with no
recompile or restart: `diamond-dap` resolves its complete, current
breakpoint table (across every source the client has ever set
breakpoints for) and pushes it over the same control channel described
below.

This does mean a debug session's compiled bytecode is meaningfully
larger than an ordinary run's (every statement in the whole program --
prelude included, since `diamond-dap` always compiles fresh with no
cache -- gets its own check, not just the handful of lines actually
selected). Accepted deliberately: a debug session already forgoes the
bytecode cache, the embedded-prelude-template fast path, and JIT
compilation (this and every other debug opcode is unrecognized by the
JIT's own compile-time scan, so any function containing one is
automatically JIT-ineligible, exactly like `debugger()`/`breakpoint()`
already were) -- it was never the startup/steady-state-throughput-
sensitive case those optimizations target. An ordinary `diamond
script.di` run has none of this instrumentation at all and pays nothing
for the feature existing.

## Stepping

`next` (step over), `stepIn`, and `stepOut` all reuse the exact same
per-statement `DIAMOND_OP_BREAKPOINT_CHECK` live breakpoints already
install everywhere -- no new bytecode, no new compile-time mechanism.
Sending one of these while stopped arms `DiamondVm.debug_step_mode`
(`src/vm.h`) with the *current* pause's own already-tracked `run_chunk`
recursion depth as a target, then resumes; the next checkpoint hit
satisfying that mode's own depth comparison pauses (`"reason":"step"` in
the resulting `stopped` event, distinguishing it from `"reason":
"breakpoint"`) and clears the mode -- a real armed breakpoint line
always still wins/pauses first, regardless of any pending step:

- **`stepIn`** -- pauses at the very next checkpoint hit, at any depth.
- **`next`** (step over) -- pauses at the next checkpoint whose depth is
  `<=` the depth stepping started at (i.e. the same frame, or a
  shallower one if the current statement returns) -- so a call made
  *from* the stepped-over statement runs to completion uninterrupted.
- **`stepOut`** -- pauses at the next checkpoint whose depth is `<` the
  starting depth -- i.e. back in the caller, right after the current
  call returns.

One known, accepted edge case: a self-recursive tail call
(`docs/callables.md`'s "Tail-call optimization") deliberately reuses
the *same* `run_chunk` recursion depth rather than incrementing it (that
feature's own whole point). Stepping over or out of a statement that
happens to be such a call therefore can't distinguish "still the same
logical call" from "a fresh tail-recursive invocation" by depth alone --
in practice this means step-over lands on the first statement of the
new invocation rather than skipping it entirely. Not fixed: an
accepted, narrow consequence of two features that were never designed
to interact, not a bug in either one on its own.

## How it fits together

```text
editor (DAP client)  <--stdio, DAP-->  diamond-dap  <--control socket-->  diamond (the debuggee)
```

- `diamond-dap` (`dap/main.c`) speaks ordinary DAP over its own stdio
  (`Content-Length`-framed JSON, via `lsp/json.c`/`lsp/rpc.c` -- the
  identical transport `diamond-lsp` already uses).
- `setBreakpoints` records `(source path, line)` pairs, replacing
  whatever was previously stored for that one path; the real spawn waits
  for `configurationDone` (DAP's own signal that a client is done
  sending its initial breakpoints) rather than `launch` itself, so a
  breakpoint that arrives after `launch` but before `configurationDone`
  still lands in the debuggee's *initial* armed set.
- At `configurationDone`, `diamond-dap` loads and require-expands the
  program exactly the way the CLI's own non-template compile path does
  (`run_source_from_bundle_program`, `src/run_source.c`), resolves every
  stored breakpoint's original `(path, line)` against that expanded
  buffer (`diamond_resolve_source_position` + `diamond_combined_buffer_line`,
  `src/compiler.h`), and spawns `diamond <program> [args...]` with two
  environment variables set (see below) and a dedicated `AF_UNIX`
  `SOCK_STREAM` socketpair wired to one end as the "control channel" --
  `DIAMOND_DEBUG_FD` names its fd number in the child.
- A `setBreakpoints` request that arrives *after* the debuggee is already
  running takes the exact same resolve step, then instead sends the
  complete, freshly-recomputed set as a `{"command":"setBreakpoints",
  "lines":[...]}` message over the already-open control channel
  (`send_live_breakpoints`, `dap/main.c`) -- no recompile, no restart.
  `next`/`stepIn`/`stepOut` write a bare `{"command":"next"}`/`"stepIn"`/
  `"stepOut"` the same way (`handle_next`/`handle_step_in`/
  `handle_step_out`, `dap/main.c`) -- no arguments needed, since the VM
  already knows its own current call depth at the exact moment it
  processes the command (see "Stepping" above).
- The debuggee's stdout/stderr are piped back as DAP `output` events; its
  exit becomes `exited`/`terminated` events.
- Every pause (an armed `DIAMOND_OP_BREAKPOINT_CHECK`, a step condition
  being satisfied, or the explicit `debugger()`/`breakpoint()` call's own
  always-unconditional `DIAMOND_OP_DEBUGGER`) writes one hand-formatted
  JSON payload -- including a `"reason":"breakpoint"`/`"step"` field
  `dap/main.c`'s own `stopped` event passes straight through -- framed
  the same `Content-Length` way, to the control fd
  (`debugger_structured_helper`, `src/vm.c`) and then blocks reading
  framed commands back in a loop -- applying any `setBreakpoints` in
  place and only resuming once a `continue`/`next`/`stepIn`/`stepOut`
  arrives -- rather than a single fixed read; the VM itself never links a
  JSON library for this (`parse_debug_command` hand-scans the fixed set
  of shapes it needs to recognize instead), only `diamond-dap` (which
  needs to speak real DAP to its own client) does. A
  `DIAMOND_OP_BREAKPOINT_CHECK` that *isn't* currently armed also does a
  quick non-blocking check of the same control fd before falling
  through -- see "Live breakpoints" above for
  why that has to happen there rather than via some other mechanism.
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
`DIAMOND_DEBUG_BREAKPOINTS` (below) -- and, equally, a live
`setBreakpoints` command's own `DiamondVm.debug_active_lines` set -- is a
flat set of these per-segment line numbers with no file discriminator,
so a breakpoint on line *N* in one file and an unrelated statement that
happens to start on line *N* in a different file (the entry script, or
another `require`d file) currently pause identically -- not caught or
resolved, just an open, documented gap for a program that spans more
than one file's own breakpoints landing on the same line number in
each. Unaffected by the move to live breakpoints: unchanged from v1.

## The env-var contract (for a second DAP-compatible client)

- `DIAMOND_DEBUG_FD=<fd>` -- an already-open, connected file descriptor
  number in the child process (read once, in `diamond_vm_init`,
  `src/vm.c`) that every pause writes its `{"event":"paused","stack":
  [...],"locals":[...]}` payload to and reads framed commands back from,
  on the *same* fd -- a plain pipe can't do that; it needs to be a
  full-duplex descriptor (a `socketpair` end, as `dap/main.c` uses).
  Unset (or a negative/unparseable value) keeps `debugger()`/
  `breakpoint()`'s ordinary print-to-stdout/`getchar()` behavior,
  unaffected by anything below. **Also the sole trigger for full-program
  `DIAMOND_OP_BREAKPOINT_CHECK` instrumentation and the live/uncached
  compile path** (`src/run_source.c`) -- a real DAP session sets this
  unconditionally, so a debuggee that starts with zero breakpoints
  selected is still fully instrumented and ready for a live
  `setBreakpoints` later; `DIAMOND_DEBUG_BREAKPOINTS` alone (no
  `DIAMOND_DEBUG_FD`, e.g. direct manual testing with no DAP client at
  all) also still triggers it.
- `DIAMOND_DEBUG_BREAKPOINTS=<line>[,<line>...]` -- a comma-separated list
  of the *expanded-buffer* line numbers (not the original file's own,
  except in the common single-file, no-`require` case where they're
  identical -- see `diamond_combined_buffer_line`'s own doc comment,
  `src/compiler.h`) that start out armed (`DiamondVm.debug_active_lines`,
  seeded once in `diamond_vm_init`, `src/vm.c`) -- freely replaceable
  afterward by a live `setBreakpoints` command over the control channel,
  never itself re-read after process startup. May be empty or absent
  entirely (a session with nothing selected yet). A line with no
  statement start on it (blank, a comment, mid-expression) silently has
  no effect either way, matching how an editor already snaps a gutter
  breakpoint to the nearest valid line for most languages.

## Building it

```sh
make dap        # build/diamond-dap
make test-dap   # tests/dap_test.sh -- a scripted setBreakpoints+launch+
                 # configurationDone+continue session against a fixture
```

`make test-all` already includes `test-dap`.
