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
`run_source` compiles a file natively: `lib/core.di` prepended, then a
`#line 1` reset so the prelude's own line numbers never leak into a
reported diagnostic's position — confirmed against `src/lexer.c`'s
handling of that exact comment, not assumed. One consequence worth
knowing: a document that only makes sense as part of a larger project
(referring to a class or function defined in a file it `require`s) will
report spurious "undefined" diagnostics on its own — there's no
cross-file `require` resolution yet (see below).

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
- **Cross-file `require` resolution** — a document is compiled in
  isolation, prelude aside. Teaching the server to resolve a `require`d
  path relative to the requesting file (and re-diagnose the right open
  document when a file it depends on changes) is real, separable work.
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
