# Language server

`lsp/` is a Language Server Protocol implementation for Diamond — a
standalone binary (`build/diamond-lsp`, built from `lsp/*.c` plus the
same compiler/VM sources every other Diamond binary shares) that speaks
LSP's standard `Content-Length`-framed JSON-RPC over stdio, the transport
every mainstream editor already knows how to launch a custom language
server with.

This is not derived from, or dependent on, any other language's LSP
implementation. Ruby's own language servers came up once in conversation
as a reference point (Diamond's syntax is Ruby-like enough that a future
syntax-highlighting grammar could plausibly start from Ruby's), but
`lsp/` itself — the JSON parser, the RPC framing, the protocol handling —
is written from scratch in C, with zero external dependencies beyond what
the rest of this repository already uses.

## What it does today

Diagnostics only:

- `initialize` — advertises `textDocumentSync: Full` (1) and nothing
  else. No `hoverProvider`, `definitionProvider`, `completionProvider`,
  etc. are declared, since none are implemented; a compliant client
  won't ask for them.
- `textDocument/didOpen` / `didChange` / `didClose` — each recompiles the
  document's current full text (full sync only; there's no incremental
  edit application) and publishes a `textDocument/publishDiagnostics`
  notification. Diamond's compiler stops at its first error, so there is
  never more than one diagnostic per publish — an empty array means the
  document currently compiles cleanly.
- `shutdown` / `exit` — the ordinary LSP lifecycle; `exit`'s process exit
  code is 0 if `shutdown` was requested first, 1 otherwise, per spec.
- Any other request gets a JSON-RPC `MethodNotFound` (-32601) error;
  any other notification is silently ignored.

A document's text is compiled exactly the way `src/main.c`'s own
`run_source` compiles a file natively: `require`d files resolved and
bundled in via `diamond_load_program` (read from disk — so a `require`
only resolves correctly once the document has a real `file://` uri
*and* its dependencies exist on disk at their expected relative
locations; an unsaved dependency's in-editor-only edits aren't seen),
then `lib/core.di` prepended and a `#line 1` reset so the prelude's own
line numbers never leak into a reported diagnostic's position —
confirmed against `src/lexer.c`'s handling of that exact comment, not
assumed. A diagnostic's line/column are re-resolved back through
`diamond_load_program`'s own segment table
(`diamond_resolve_diagnostic_location`, shared with `src/main.c`'s CLI
diagnostic printing — not a separate reimplementation) so they land on
the right line even when a `require`d file's inlined content shifts
everything after it. One consequence worth knowing: if the *actual*
error is inside a `require`d file rather than the open document itself,
this document currently reports nothing at all (not the dependency's
error misattributed to the wrong file, but not the dependency's own
diagnostic either) — publishing a second `publishDiagnostics`
notification against the dependency's own uri is real, separable work
for later.

## Building and connecting an editor

```sh
make lsp
```

produces `build/diamond-lsp`. Point any editor's "custom language server"
configuration at that binary with stdio as the transport (no `--stdio`
flag needed or accepted — stdio is the only transport this server
speaks). The exact configuration step is editor-specific; consult your
editor's LSP client documentation for how it registers a server for a
new file type/extension (`.di`).

`make test-lsp` runs `tests/lsp_test.sh`, which drives the real binary
over its real stdio transport via a bash `coproc` — the same
no-Python/no-Node convention every other test script in this repo
follows — through the full `initialize` → `didOpen` → `didChange` →
`didClose` → `shutdown` → `exit` lifecycle, plus the unknown-method and
exit-without-shutdown edge cases.

## What's deliberately out of scope so far

- **Hover, go-to-definition, completion, symbol search** — all need a
  real symbol table (name → declaration site, with scope resolution),
  which nothing in `lsp/` builds yet. Diagnostics don't need one:
  `diamond_compile` already does all the work and hands back exactly the
  one thing needed.
- **Diagnostics for a broken dependency, published against its own
  file** — `require` itself resolves (see above), but if the error is
  inside the required file rather than the open document, nothing gets
  published for it at all yet. Doing this properly means tracking which
  open documents depend on which files (so editing a dependency
  re-diagnoses everything that requires it, not just itself) — real,
  separable work.
- **Incremental sync** — `textDocumentSync` only ever advertises `Full`.
  Diamond has no incremental-recompile story at all yet (every compile is
  a fresh `diamond_compile` call over the whole combined buffer), so
  `Incremental` sync would only add bookkeeping for no benefit until that
  changes.
- **Any transport but stdio** — no TCP/socket mode. Every mainstream
  editor already launches custom language servers over stdio, so there's
  no immediate need for another one.

Each of these is a plausible next slice, sized independently — see
`docs/roadmap.md`.
